#include "HermesProtocol.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace hermes {

namespace {

void putLe16(uint8_t* out, const uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

void putLe32(uint8_t* out, const uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

// Error bodies look like {"error": {"message": "..."}} or {"error": "..."}.
void readError(const JsonDocument& doc, std::string& error) {
  const JsonVariantConst err = doc["error"];
  if (err.isNull()) return;
  const char* message = err["message"] | static_cast<const char*>(nullptr);
  if (!message) message = err | "";
  if (message[0] != '\0') error = message;
}

bool startsWith(const std::string& s, const size_t at, const char* prefix) {
  return s.compare(at, strlen(prefix), prefix) == 0;
}

constexpr const char* kBookExtensions[] = {".epub", ".xtch", ".xtc", ".txt", ".md"};

// Extension of the URL path (before ?/#), lower-cased, or "" if not a book.
std::string bookExtension(const std::string& url) {
  const size_t end = url.find_first_of("?#");
  std::string path = url.substr(0, end);
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
  for (const char* ext : kBookExtensions) {
    const size_t n = strlen(ext);
    if (path.size() > n && path.compare(path.size() - n, n, ext) == 0) return ext;
  }
  return {};
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::vector<std::string> findBookLinks(const std::string& text, const size_t maxLinks) {
  std::vector<std::string> links;
  size_t pos = 0;
  while (links.size() < maxLinks) {
    const size_t http = text.find("http", pos);
    if (http == std::string::npos) break;
    pos = http + 4;
    if (text.compare(http, 7, "http://") != 0 && text.compare(http, 8, "https://") != 0) continue;
    size_t end = http;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '"' &&
           text[end] != '\'' && text[end] != '<' && text[end] != '>' && text[end] != ')' && text[end] != ']' &&
           text[end] != '`') {
      ++end;
    }
    std::string url = text.substr(http, end - http);
    while (!url.empty() && std::strchr(".,;:!?*", url.back())) url.pop_back();  // sentence punctuation
    pos = end;
    if (bookExtension(url).empty()) continue;
    if (std::find(links.begin(), links.end(), url) == links.end()) links.push_back(url);
  }
  return links;
}

std::string bookFileName(const std::string& url) {
  const std::string ext = bookExtension(url);
  std::string path = url.substr(0, url.find_first_of("?#"));
  const size_t slash = path.find_last_of('/');
  std::string raw = slash == std::string::npos ? path : path.substr(slash + 1);

  std::string name;
  name.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '%' && i + 2 < raw.size() && hexValue(raw[i + 1]) >= 0 && hexValue(raw[i + 2]) >= 0) {
      c = static_cast<char>(hexValue(raw[i + 1]) * 16 + hexValue(raw[i + 2]));
      i += 2;
    } else if (c == '+') {
      c = ' ';
    }
    const unsigned char u = static_cast<unsigned char>(c);
    // Keep UTF-8 bytes (titles), ASCII word characters and a few separators.
    const bool keep = u >= 0x80 || std::isalnum(u) || std::strchr(" ._-()[],'&", c);
    name += keep && u >= 0x20 ? c : '_';
  }
  // Drop the extension (re-added below), trim separators, cap the stem.
  if (!ext.empty() && name.size() >= ext.size()) name.resize(name.size() - ext.size());
  while (!name.empty() && std::strchr(" ._", name.front())) name.erase(0, 1);
  while (!name.empty() && std::strchr(" ._", name.back())) name.pop_back();
  constexpr size_t kMaxStem = 80;
  if (name.size() > kMaxStem) {
    name.resize(kMaxStem);
    while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();  // UTF-8 tail
    if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) name.pop_back();              // lead byte
  }
  if (name.empty()) name = "book";
  return name + (ext.empty() ? ".epub" : ext);
}

std::string joinUrl(const std::string& base, const char* path) {
  if (base.find("/v1/") != std::string::npos) return base;
  std::string url = base;
  while (!url.empty() && url.back() == '/') url.pop_back();
  // Accept a base that already ends in /v1.
  if (url.size() >= 3 && url.compare(url.size() - 3, 3, "/v1") == 0 && strncmp(path, "/v1", 3) == 0) {
    url.resize(url.size() - 3);
  }
  url += path;
  return url;
}

std::string buildResponsesBody(const std::string& model, const std::string& conversation, const std::string& text) {
  JsonDocument doc;
  doc["model"] = model;
  doc["input"] = text;
  if (!conversation.empty()) doc["conversation"] = conversation;
  doc["store"] = true;
  std::string body;
  serializeJson(doc, body);
  return body;
}

std::string buildChatCompletionsBody(const std::string& model, const std::string& text) {
  JsonDocument doc;
  doc["model"] = model;
  JsonObject message = doc["messages"].add<JsonObject>();
  message["role"] = "user";
  message["content"] = text;
  std::string body;
  serializeJson(doc, body);
  return body;
}

ParseResult parseReply(const std::string& body, const ApiStyle style, std::string& text, std::string& serverError) {
  text.clear();
  serverError.clear();
  // Proxies answer failures with HTML; a filtered parse does not reject that.
  const size_t first = body.find_first_not_of(" \t\r\n");
  if (first == std::string::npos || body[first] != '{') return ParseResult::BadJson;
  // Responses output also carries tool calls and their (possibly large)
  // outputs; the filter keeps only what we render so parsing stays small.
  JsonDocument filter;
  filter["error"] = true;
  if (style == ApiStyle::Responses) {
    filter["output_text"] = true;
    filter["output"][0]["type"] = true;
    filter["output"][0]["content"][0]["type"] = true;
    filter["output"][0]["content"][0]["text"] = true;
  } else {
    filter["choices"][0]["message"]["content"] = true;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.size(), DeserializationOption::Filter(filter))) {
    return ParseResult::BadJson;
  }
  readError(doc, serverError);

  if (style == ApiStyle::Responses) {
    const char* flat = doc["output_text"] | "";
    if (flat[0] != '\0') {
      text = flat;
    } else {
      for (JsonVariantConst item : doc["output"].as<JsonArrayConst>()) {
        if (strcmp(item["type"] | "", "message") != 0) continue;
        for (JsonVariantConst part : item["content"].as<JsonArrayConst>()) {
          const char* type = part["type"] | "";
          if (strcmp(type, "output_text") != 0 && strcmp(type, "text") != 0) continue;
          if (!text.empty()) text += "\n\n";
          text += part["text"] | "";
        }
      }
    }
  } else {
    text = doc["choices"][0]["message"]["content"] | "";
  }
  return text.empty() ? ParseResult::NoText : ParseResult::Ok;
}

ParseResult parseTranscript(const std::string& body, std::string& text, std::string& serverError) {
  static constexpr char kSpace[] = " \t\r\n";
  text.clear();
  serverError.clear();
  const size_t first = body.find_first_not_of(kSpace);
  if (first == std::string::npos) return ParseResult::NoText;
  if (body[first] != '{') {
    text = body.substr(first, body.find_last_not_of(kSpace) - first + 1);
    return ParseResult::Ok;
  }
  JsonDocument filter;
  filter["text"] = true;
  filter["error"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.size(), DeserializationOption::Filter(filter))) {
    return ParseResult::BadJson;
  }
  readError(doc, serverError);
  text = doc["text"] | "";
  const size_t start = text.find_first_not_of(kSpace);
  if (start == std::string::npos) {
    text.clear();
    return ParseResult::NoText;
  }
  text.erase(0, start);
  text.erase(text.find_last_not_of(kSpace) + 1);
  return ParseResult::Ok;
}

void writeWavHeader(uint8_t* out, const uint32_t pcmBytes, const uint32_t sampleRate) {
  constexpr uint16_t channels = 1;
  constexpr uint16_t bitsPerSample = 16;
  memcpy(out, "RIFF", 4);
  putLe32(out + 4, 36 + pcmBytes);
  memcpy(out + 8, "WAVEfmt ", 8);
  putLe32(out + 16, 16);  // fmt chunk size
  putLe16(out + 20, 1);   // PCM
  putLe16(out + 22, channels);
  putLe32(out + 24, sampleRate);
  putLe32(out + 28, sampleRate * channels * bitsPerSample / 8);
  putLe16(out + 32, channels * bitsPerSample / 8);
  putLe16(out + 34, bitsPerSample);
  memcpy(out + 36, "data", 4);
  putLe32(out + 40, pcmBytes);
}

std::string multipartHead(const char* boundary, const std::string& model, const std::string& language) {
  std::string head;
  const auto field = [&](const char* name, const std::string& value) {
    head += "--";
    head += boundary;
    head += "\r\nContent-Disposition: form-data; name=\"";
    head += name;
    head += "\"\r\n\r\n";
    head += value;
    head += "\r\n";
  };
  if (!model.empty()) field("model", model);
  if (!language.empty()) field("language", language);
  field("response_format", "json");
  head += "--";
  head += boundary;
  head += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"sticky.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";
  return head;
}

std::string multipartTail(const char* boundary) {
  std::string tail = "\r\n--";
  tail += boundary;
  tail += "--\r\n";
  return tail;
}

std::string plainText(const std::string& markdown) {
  std::string out;
  out.reserve(markdown.size());
  size_t pos = 0;
  while (pos <= markdown.size()) {
    size_t end = markdown.find('\n', pos);
    if (end == std::string::npos) end = markdown.size();
    std::string line = markdown.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    size_t lead = line.find_first_not_of(' ');
    if (lead == std::string::npos) lead = line.size();
    if (startsWith(line, lead, "```")) continue;  // drop fence lines, keep code
    if (startsWith(line, lead, "#")) {
      size_t hashes = lead;
      while (hashes < line.size() && line[hashes] == '#') ++hashes;
      if (hashes < line.size() && line[hashes] == ' ') line = line.substr(hashes + 1);
    } else if (startsWith(line, lead, "- ") || startsWith(line, lead, "* ") || startsWith(line, lead, "+ ")) {
      line = line.substr(0, lead) + "\xE2\x80\xA2 " + line.substr(lead + 2);  // bullet
    } else if (startsWith(line, lead, "> ")) {
      line = line.substr(0, lead) + line.substr(lead + 2);
    } else if (line.find_first_not_of("-*_ ") == std::string::npos && line.size() >= 3) {
      line = "";  // horizontal rule
    }

    // Inline: drop emphasis/code markers, turn [label](url) into label.
    std::string inl;
    inl.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (c == '*' || c == '`' || (c == '_' && i + 1 < line.size() && line[i + 1] == '_')) {
        if (c == '_') ++i;
        continue;
      }
      if (c == '[') {
        const size_t close = line.find("](", i);
        const size_t paren = close == std::string::npos ? close : line.find(')', close);
        if (paren != std::string::npos) {
          inl.append(line, i + 1, close - i - 1);
          i = paren;
          continue;
        }
      }
      inl += c;
    }
    out += inl;
    if (end < markdown.size()) out += '\n';
    if (end >= markdown.size()) break;
  }
  // Collapse runs of 3+ newlines into a single blank line.
  std::string collapsed;
  collapsed.reserve(out.size());
  int newlines = 0;
  for (const char c : out) {
    newlines = c == '\n' ? newlines + 1 : 0;
    if (newlines <= 2) collapsed += c;
  }
  while (!collapsed.empty() && (collapsed.back() == '\n' || collapsed.back() == ' ')) collapsed.pop_back();
  return collapsed;
}

}  // namespace hermes

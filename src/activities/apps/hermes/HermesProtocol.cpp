#include "HermesProtocol.h"

#include <ArduinoJson.h>

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

}  // namespace

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

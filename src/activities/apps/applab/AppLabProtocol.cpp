#include "AppLabProtocol.h"

#include <ArduinoJson.h>

#include <cctype>

namespace applab {

namespace {

constexpr size_t kMaxIdLength = 31;
constexpr size_t kMaxModelLength = 64;

bool isIdChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

std::string endpointFor(const std::string& model) {
  return "https://generativelanguage.googleapis.com/v1beta/models/" + model + ":generateContent";
}

bool isValidModel(const std::string& model) {
  if (model.empty() || model.size() > kMaxModelLength) return false;
  for (const char c : model) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
  }
  return true;
}

std::string userMessage(const RequestKind kind, const std::string& request, const std::string& code,
                        const std::vector<std::string>& problems) {
  std::string text;
  switch (kind) {
    case RequestKind::Create:
      text = "Make this app:\n" + request;
      break;
    case RequestKind::Change:
      text = "Here is the current app:\n```lua\n" + code + "\n```\nChange it as follows and return the whole app:\n" +
             request;
      break;
    case RequestKind::Fix:
      text = "Here is an app:\n```lua\n" + code + "\n```\nRunning it in the sandbox found these problems:\n";
      for (const auto& problem : problems) text += "- " + problem + "\n";
      text += "Fix them and return the whole app. The user originally asked for:\n" + request;
      break;
  }
  return text;
}

std::string buildRequestBody(const char* systemInstruction, const std::string& userText, const int maxOutputTokens) {
  JsonDocument doc;
  doc["systemInstruction"]["parts"][0]["text"] = systemInstruction;
  JsonObject turn = doc["contents"].add<JsonObject>();
  turn["role"] = "user";
  turn["parts"][0]["text"] = userText;

  JsonObject config = doc["generationConfig"].to<JsonObject>();
  config["responseMimeType"] = "application/json";
  config["maxOutputTokens"] = maxOutputTokens;
  config["temperature"] = 0.4;
  JsonObject schema = config["responseSchema"].to<JsonObject>();
  schema["type"] = "OBJECT";
  schema["properties"]["name"]["type"] = "STRING";
  schema["properties"]["main_lua"]["type"] = "STRING";
  schema["required"].add("name");
  schema["required"].add("main_lua");

  std::string body;
  serializeJson(doc, body);
  return body;
}

ParseResult extractText(const std::string& body, std::string& text, std::string& message) {
  text.clear();
  message.clear();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return ParseResult::BadJson;

  if (doc["error"].is<JsonObjectConst>()) {
    message = doc["error"]["message"] | "";
    return ParseResult::ApiError;
  }
  const char* blockReason = doc["promptFeedback"]["blockReason"] | "";
  if (blockReason[0] != '\0') {
    message = blockReason;
    return ParseResult::Blocked;
  }

  JsonVariantConst candidate = doc["candidates"][0];
  for (JsonVariantConst part : candidate["content"]["parts"].as<JsonArrayConst>()) {
    if (part["thought"] | false) continue;
    text += part["text"] | "";
  }
  const std::string finish = candidate["finishReason"] | "";
  if (finish == "MAX_TOKENS") return ParseResult::TooLong;
  if (finish == "SAFETY" || finish == "RECITATION" || finish == "PROHIBITED_CONTENT" || finish == "BLOCKLIST") {
    message = finish;
    return ParseResult::Blocked;
  }
  return text.empty() ? ParseResult::NoApp : ParseResult::Ok;
}

ParseResult parseApp(const std::string& text, std::string& name, std::string& lua) {
  name.clear();
  lua.clear();
  JsonDocument doc;
  if (!deserializeJson(doc, text)) {
    name = doc["name"] | "";
    lua = doc["main_lua"] | "";
    return lua.empty() ? ParseResult::NoApp : ParseResult::Ok;
  }
  // The model ignored the JSON schema: accept a fenced Lua block.
  const size_t fence = text.find("```lua");
  if (fence == std::string::npos) return ParseResult::NoApp;
  const size_t start = text.find('\n', fence);
  const size_t end = start == std::string::npos ? std::string::npos : text.find("```", start);
  if (end == std::string::npos) return ParseResult::NoApp;
  lua = text.substr(start + 1, end - start - 1);
  return lua.empty() ? ParseResult::NoApp : ParseResult::Ok;
}

bool isValidId(const std::string& id) {
  if (id.empty() || id.size() > kMaxIdLength || !(id[0] >= 'a' && id[0] <= 'z')) return false;
  for (const char c : id) {
    if (!isIdChar(c)) return false;
  }
  return true;
}

std::string idFromName(const std::string& name) {
  std::string id;
  bool gap = false;
  for (const char raw : name) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
    if (isIdChar(c) && c != '_') {
      if (gap && !id.empty()) id += '_';
      gap = false;
      if (id.empty() && !(c >= 'a' && c <= 'z')) id = "app_";
      id += c;
    } else {
      gap = true;
    }
    if (id.size() >= kMaxIdLength) break;
  }
  if (id.size() > kMaxIdLength) id.resize(kMaxIdLength);
  while (!id.empty() && id.back() == '_') id.pop_back();
  return id.empty() ? "app" : id;
}

}  // namespace applab

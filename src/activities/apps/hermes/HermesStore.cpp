#include "HermesStore.h"

#include <ObfuscationUtils.h>

namespace {

std::string readSecret(JsonVariantConst value) {
  const char* encoded = value | "";
  if (encoded[0] == '\0') return {};
  bool ok = false;
  bool tooLong = false;
  std::string secret = obfuscation::deobfuscateFromBase64(encoded, 512, &ok, &tooLong);
  return ok && !tooLong ? secret : std::string{};
}

template <typename T, size_t N>
T nextOption(const T (&options)[N], const T current) {
  for (size_t i = 0; i < N; ++i) {
    if (options[i] == current) return options[(i + 1) % N];
  }
  return options[0];
}

template <typename T, size_t N>
T validOption(const T (&options)[N], const T value, const T fallback) {
  for (size_t i = 0; i < N; ++i) {
    if (options[i] == value) return value;
  }
  return fallback;
}

}  // namespace

void HermesStore::toJson(JsonDocument& doc) const {
  doc["chatUrl"] = chatUrl;
  doc["chatKey_obf"] = obfuscation::obfuscateToBase64(chatKey);
  doc["chatModel"] = chatModel;
  doc["conversation"] = conversation;
  doc["chatApi"] = static_cast<uint8_t>(chatApi);
  doc["timeoutSeconds"] = timeoutSeconds;
  doc["sttUrl"] = sttUrl;
  doc["sttKey_obf"] = obfuscation::obfuscateToBase64(sttKey);
  doc["sttModel"] = sttModel;
  doc["sttLanguage"] = sttLanguage;
  doc["voiceAutoSend"] = voiceAutoSend;
  doc["maxRecordSeconds"] = maxRecordSeconds;
}

bool HermesStore::fromJson(JsonVariantConst doc) {
  chatUrl = doc["chatUrl"] | chatUrl.c_str();
  chatKey = readSecret(doc["chatKey_obf"]);
  chatModel = doc["chatModel"] | chatModel.c_str();
  conversation = doc["conversation"] | conversation.c_str();
  const uint8_t api = doc["chatApi"] | static_cast<uint8_t>(0);
  chatApi = api == static_cast<uint8_t>(ChatApi::ChatCompletions) ? ChatApi::ChatCompletions : ChatApi::Responses;
  timeoutSeconds = validOption(kTimeoutSecondsOptions, static_cast<uint16_t>(doc["timeoutSeconds"] | 180),
                               static_cast<uint16_t>(180));
  sttUrl = doc["sttUrl"] | "";
  sttKey = readSecret(doc["sttKey_obf"]);
  sttModel = doc["sttModel"] | sttModel.c_str();
  sttLanguage = doc["sttLanguage"] | "";
  voiceAutoSend = doc["voiceAutoSend"] | true;
  maxRecordSeconds = validOption(kRecordSecondsOptions, static_cast<uint8_t>(doc["maxRecordSeconds"] | 30),
                                 static_cast<uint8_t>(30));
  return true;
}

void HermesStore::cycleChatApi() {
  chatApi = chatApi == ChatApi::Responses ? ChatApi::ChatCompletions : ChatApi::Responses;
}

void HermesStore::cycleTimeout() { timeoutSeconds = nextOption(kTimeoutSecondsOptions, timeoutSeconds); }

void HermesStore::cycleMaxRecord() { maxRecordSeconds = nextOption(kRecordSecondsOptions, maxRecordSeconds); }

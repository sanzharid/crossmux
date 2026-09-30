#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// Settings for the Hermes chat app: the Hermes Agent API server used for chat,
// and an OpenAI-compatible speech-to-text endpoint used for voice input.
// API keys are XOR-obfuscated with the device MAC on disk (same as KOReader/OPDS).
class HermesStore : public PersistableStore<HermesStore> {
 public:
  enum class ChatApi : uint8_t { Responses = 0, ChatCompletions = 1 };

  static constexpr uint8_t kRecordSecondsOptions[] = {15, 30, 60, 120};
  static constexpr uint16_t kTimeoutSecondsOptions[] = {60, 180, 300, 600};

  static const char* getFilePath() { return "/.crosspoint/hermes.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // --- Chat (Hermes Agent API server) ---
  std::string chatUrl;                                // base URL, e.g. http://hermes.local:8642
  std::string chatKey;                                // API_SERVER_KEY (Bearer)
  std::string chatModel = "hermes-agent";             // profile / model name
  std::string conversation = "sticky";                // server-side conversation name
  ChatApi chatApi = ChatApi::Responses;
  uint16_t timeoutSeconds = 180;  // agent turns can run tools for a while

  // --- Voice (OpenAI-compatible /v1/audio/transcriptions, multipart WAV) ---
  std::string sttUrl;  // full endpoint URL; empty = voice disabled
  std::string sttKey;  // optional Bearer token
  std::string sttModel = "whisper-1";
  std::string sttLanguage;  // optional ISO code, e.g. "en"; empty = auto-detect
  bool voiceAutoSend = true;  // send the transcript straight to Hermes
  uint8_t maxRecordSeconds = 30;

  // A bare "http://" prefill left in place does not count as configured.
  bool hasChatConfig() const {
    const size_t scheme = chatUrl.find("://");
    return scheme != std::string::npos && chatUrl.size() > scheme + 3;
  }
  bool hasVoiceConfig() const { return !sttUrl.empty(); }

  void cycleChatApi();
  void cycleTimeout();
  void cycleMaxRecord();

 private:
  HermesStore() = default;
  friend class PersistableStore<HermesStore>;
};

#define HERMES_STORE HermesStore::getInstance()

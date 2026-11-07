#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

// App Lab settings: the Gemini API key and model. The key is XOR-obfuscated
// with the device MAC on disk, like the Hermes and OPDS credentials.
class AppLabStore : public PersistableStore<AppLabStore> {
 public:
  // Supported until at least May 2027 per Google's model lifecycle table.
  static constexpr const char* kDefaultModel = "gemini-3.5-flash";

  static const char* getFilePath() { return "/.crosspoint/applab.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  std::string geminiKey;
  std::string model = kDefaultModel;

  bool hasKey() const { return !geminiKey.empty(); }

 private:
  AppLabStore() = default;
  friend class PersistableStore<AppLabStore>;
};

#define APPLAB_STORE AppLabStore::getInstance()

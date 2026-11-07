#include "AppLabStore.h"

#include <ObfuscationUtils.h>

#include "AppLabProtocol.h"

void AppLabStore::toJson(JsonDocument& doc) const {
  doc["geminiKey_obf"] = obfuscation::obfuscateToBase64(geminiKey);
  doc["model"] = model;
}

bool AppLabStore::fromJson(JsonVariantConst doc) {
  const char* encoded = doc["geminiKey_obf"] | "";
  geminiKey.clear();
  if (encoded[0] != '\0') {
    bool ok = false;
    bool tooLong = false;
    std::string key = obfuscation::deobfuscateFromBase64(encoded, 512, &ok, &tooLong);
    if (ok && !tooLong) geminiKey = std::move(key);
  }
  const char* stored = doc["model"] | kDefaultModel;
  model = applab::isValidModel(stored) ? stored : kDefaultModel;
  return true;
}

#include "AppLabFiles.h"

#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "AppLabProtocol.h"

namespace applab {

namespace {

constexpr char kRoot[] = "/.crosspoint/applab";
constexpr char kPendingPath[] = "/.crosspoint/applab/pending.txt";
constexpr size_t kMaxDataKeys = 32;
constexpr size_t kMaxDataBytes = 2048;

std::string appDir(const std::string& id) { return std::string(kRoot) + "/" + id; }

bool writeAtomically(const std::string& path, const char* data, const size_t size) {
  const std::string temp = path + ".part";
  {
    HalFile file = Storage.open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!file || file.write(reinterpret_cast<const uint8_t*>(data), size) != size) {
      Storage.remove(temp.c_str());
      return false;
    }
    file.flush();
  }
  return Storage.replaceFile(temp.c_str(), path.c_str());
}

bool readInfo(const std::string& id, AppInfo& info) {
  std::string json;
  if (!Storage.readFileToString("APPLAB", appDir(id) + "/app.json", 8 * 1024, json)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  info.id = id;
  info.name = doc["name"] | id.c_str();
  info.prompt = doc["prompt"] | "";
  return true;
}

}  // namespace

std::vector<AppInfo> listApps() {
  std::vector<AppInfo> apps;
  if (!Storage.exists(kRoot)) return apps;
  // Storage.listFiles() skips directories, and each app is a directory.
  std::vector<std::string> ids;
  {
    HalFile root = Storage.open(kRoot);
    if (!root || !root.isDirectory()) return apps;
    char name[64];
    for (HalFile entry = root.openNextFile(); entry && ids.size() < kMaxApps; entry = root.openNextFile()) {
      if (entry.isDirectory() && entry.getName(name, sizeof(name)) > 0 && isValidId(name)) ids.emplace_back(name);
    }
  }
  apps.reserve(ids.size());
  for (const auto& id : ids) {
    AppInfo info;
    if (readInfo(id, info)) apps.push_back(std::move(info));
  }
  std::sort(apps.begin(), apps.end(), [](const AppInfo& a, const AppInfo& b) { return a.name < b.name; });
  return apps;
}

bool readApp(const std::string& id, AppInfo& info, std::string& lua) {
  if (!isValidId(id) || !readInfo(id, info)) return false;
  return Storage.readFileToString("APPLAB", appDir(id) + "/main.lua", crossstore::MAX_SCRIPT_BYTES, lua);
}

bool saveApp(AppInfo& info, const std::string& lua) {
  if (lua.empty() || lua.size() > crossstore::MAX_SCRIPT_BYTES) return false;
  if (info.id.empty()) {
    const std::string base = idFromName(info.name);
    std::string id = base;
    for (int n = 2; Storage.exists(appDir(id).c_str()); ++n) {
      if (n > 99) return false;
      const std::string suffix = "_" + std::to_string(n);
      id = base.substr(0, std::min(base.size(), static_cast<size_t>(31) - suffix.size())) + suffix;
    }
    info.id = id;
  }
  if (!isValidId(info.id)) return false;
  const std::string dir = appDir(info.id);
  if (!Storage.ensureDirectoryExists(kRoot) || !Storage.ensureDirectoryExists(dir.c_str())) return false;

  JsonDocument doc;
  doc["name"] = info.name;
  doc["prompt"] = info.prompt;
  std::string json;
  serializeJson(doc, json);
  return writeAtomically(dir + "/main.lua", lua.data(), lua.size()) &&
         writeAtomically(dir + "/app.json", json.data(), json.size());
}

bool deleteApp(const std::string& id) { return isValidId(id) && Storage.removeDir(appDir(id).c_str()); }

bool readData(const std::string& id, const char* key, char* out, const size_t capacity) {
  if (!isValidId(id) || !isValidId(key)) return false;
  const std::string path = appDir(id) + "/data/" + key;
  HalFile file = Storage.open(path.c_str());
  if (!file || file.size() >= capacity) return false;
  const size_t bytes = file.size();
  if (file.read(reinterpret_cast<uint8_t*>(out), bytes) != static_cast<int>(bytes)) return false;
  out[bytes] = '\0';
  return true;
}

bool writeData(const std::string& id, const char* key, const char* value, const size_t size) {
  if (!isValidId(id) || !isValidId(key) || size > kMaxDataBytes) return false;
  const std::string dir = appDir(id) + "/data";
  const std::string path = dir + "/" + key;
  if (!Storage.ensureDirectoryExists(dir.c_str())) return false;
  if (!Storage.exists(path.c_str()) &&
      Storage.listFiles(dir.c_str(), static_cast<int>(kMaxDataKeys) + 1).size() >= kMaxDataKeys) {
    return false;
  }
  return writeAtomically(path, value, size);
}

bool readPendingPrompt(std::string& prompt) {
  prompt.clear();
  return Storage.exists(kPendingPath) && Storage.readFileToString("APPLAB", kPendingPath, kMaxPromptBytes, prompt) &&
         !prompt.empty();
}

bool writePendingPrompt(const std::string& prompt) {
  if (prompt.size() > kMaxPromptBytes || !Storage.ensureDirectoryExists(kRoot)) return false;
  if (prompt.empty()) {
    clearPendingPrompt();
    return true;
  }
  return writeAtomically(kPendingPath, prompt.data(), prompt.size());
}

void clearPendingPrompt() {
  if (Storage.exists(kPendingPath)) Storage.remove(kPendingPath);
}

bool systemCall(const char* command, crossstore::ServiceReply& reply) {
  if (std::strcmp(command, "device.time") == 0) {
    struct tm now{};
    if (!halClock.hasValidTime() || !halClock.localTime(now)) return false;
    std::snprintf(reply.text, sizeof(reply.text), "%02d:%02d", now.tm_hour, now.tm_min);
    std::snprintf(reply.extra, sizeof(reply.extra), "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
    reply.a = now.tm_wday;
    reply.b = now.tm_sec;
    reply.c = static_cast<int64_t>(time(nullptr));
    return true;
  }
  if (std::strcmp(command, "device.battery") == 0) {
    reply.a = powerManager.getBatteryPercentage();
    std::snprintf(reply.text, sizeof(reply.text), "%d%%", static_cast<int>(reply.a));
    return true;
  }
  return false;
}

}  // namespace applab

#pragma once

#include <CrossStoreRuntime.h>

#include <cstddef>
#include <string>
#include <vector>

// SD-card layout for App Lab:
//   /.crosspoint/applab/<id>/main.lua   the app
//   /.crosspoint/applab/<id>/app.json   {"name": ..., "prompt": ...}
//   /.crosspoint/applab/<id>/data/<key> the app's own app.read/app.write values
//   /.crosspoint/applab/pending.txt     a prompt sent from the phone page
namespace applab {

constexpr size_t kMaxApps = 64;
constexpr size_t kMaxPromptBytes = 2000;

struct AppInfo {
  std::string id;
  std::string name;
  std::string prompt;
};

std::vector<AppInfo> listApps();
bool readApp(const std::string& id, AppInfo& info, std::string& lua);
// Writes main.lua and app.json through temp files. A new id is made unique.
bool saveApp(AppInfo& info, const std::string& lua);
bool deleteApp(const std::string& id);

// app.read / app.write storage, as in CrossStore: up to 32 keys of 2 KiB.
bool readData(const std::string& id, const char* key, char* out, size_t capacity);
bool writeData(const std::string& id, const char* key, const char* value, size_t size);

bool readPendingPrompt(std::string& prompt);
bool writePendingPrompt(const std::string& prompt);
void clearPendingPrompt();

// app.system(): only the read-only device.time and device.battery commands.
bool systemCall(const char* command, crossstore::ServiceReply& reply);

}  // namespace applab

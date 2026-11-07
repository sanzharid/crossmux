#pragma once

#include <string>
#include <vector>

#include "AppLabProtocol.h"
#include "activities/UiListActivity.h"

namespace applab {
struct GeminiJob;
}

// Turns a request into a saved app: connects Wi-Fi, asks Gemini, runs the
// result in the sandbox, sends any problems back for up to two fixes, then
// saves it. Finishes with FilePathResult{app id}, or cancelled.
class AppLabGenerateActivity final : public UiListActivity {
 public:
  // For Change, `appId` names the app whose code is sent along and replaced.
  AppLabGenerateActivity(GfxRenderer& renderer, MappedInputManager& input, applab::RequestKind kind,
                         std::string request, std::string appId = "");
  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  enum class Phase : uint8_t { Wifi, Asking, Done, Failed };

  const applab::RequestKind kind_;
  const std::string request_;
  std::string appId_;
  std::string appName_;
  std::string code_;  // the app being changed, then each generated version
  std::vector<std::string> problems_;
  int fixes_ = 0;
  Phase phase_ = Phase::Wifi;
  applab::GeminiJob* job_ = nullptr;
  bool ownsWifi_ = false;
  uint32_t phaseStartedMs_ = 0;
  uint32_t wifiDeadlineMs_ = 0;
  uint32_t lastSecond_ = 0;
  std::string status_;
  std::string detail_;
  freeink::ui::ListItem rows_[2]{};

  void startWifi();
  void sendRequest();
  void handleReply();
  void fail(const std::string& message);
  void setStatus(std::string status, std::string detail = "");
  void teardownWifi();

  int listCount() const override { return 2; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
};

#include "AppLabGenerateActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cstdio>

#include "AppLabCheck.h"
#include "AppLabFiles.h"
#include "AppLabPrompt.h"
#include "AppLabStore.h"
#include "GeminiJob.h"
#include "LabAppActivity.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

constexpr int kMaxFixes = 2;
constexpr int kMaxOutputTokens = 24576;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;

std::string formatted(const char* format, const unsigned value) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), format, value);
  return buffer;
}

}  // namespace

AppLabGenerateActivity::AppLabGenerateActivity(GfxRenderer& renderer, MappedInputManager& input,
                                               const applab::RequestKind kind, std::string request, std::string appId)
    : UiListActivity("AppLabGenerate", renderer, input),
      kind_(kind),
      request_(std::move(request)),
      appId_(std::move(appId)) {}

void AppLabGenerateActivity::onEnter() {
  UiListActivity::onEnter();
  APPLAB_STORE.loadFromFile();
  if (!APPLAB_STORE.hasKey()) {
    fail(tr(STR_APPLAB_NO_KEY));
    return;
  }
  if (kind_ == applab::RequestKind::Change) {
    applab::AppInfo info;
    if (!applab::readApp(appId_, info, code_)) {
      fail(tr(STR_APPLAB_ERR_MISSING));
      return;
    }
    appName_ = info.name;
  }
  startWifi();
}

void AppLabGenerateActivity::onExit() {
  if (job_) {
    job_->cancel = true;
    job_->release();
    job_ = nullptr;
  }
  teardownWifi();
  UiListActivity::onExit();
}

void AppLabGenerateActivity::setStatus(std::string status, std::string detail) {
  {
    RenderLock lock(*this);
    status_ = std::move(status);
    detail_ = std::move(detail);
  }
  requestUpdate();
}

void AppLabGenerateActivity::fail(const std::string& message) {
  LOG_ERR("APPLAB", "Generation failed: %s", message.c_str());
  if (job_) {
    job_->cancel = true;
    job_->release();
    job_ = nullptr;
  }
  phase_ = Phase::Failed;
  teardownWifi();
  setStatus(tr(STR_APPLAB_FAILED), message);
}

void AppLabGenerateActivity::startWifi() {
  phase_ = Phase::Wifi;
  phaseStartedMs_ = millis();
  if (WiFi.status() == WL_CONNECTED) {
    sendRequest();
    return;
  }
  setStatus(tr(STR_HERMES_STATUS_WIFI));
  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  ownsWifi_ = true;
  if (!credential) {
    const bool started = startActivityForResultWith<WifiSelectionActivity>(
        [this](const ActivityResult& result) {
          if (!result.isCancelled && WiFi.status() == WL_CONNECTED) {
            sendRequest();
          } else {
            fail(tr(STR_WIFI_CONN_FAILED));
          }
        },
        true);
    if (!started) fail(tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  WiFi.persistent(false);
  if (!NetworkStartup::setMode(renderer, WIFI_STA)) {
    fail(tr(STR_WIFI_CONN_FAILED));
    return;
  }
  WiFi.disconnect(true, true);
  delay(100);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }
  wifiDeadlineMs_ = millis() + kWifiConnectTimeoutMs;
}

void AppLabGenerateActivity::teardownWifi() {
  if (!ownsWifi_) return;
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  esp_wifi_deinit();
  ownsWifi_ = false;
}

void AppLabGenerateActivity::sendRequest() {
  auto* job = applab::GeminiJob::create();
  if (!job) {
    fail(tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  const applab::RequestKind kind = problems_.empty() ? kind_ : applab::RequestKind::Fix;
  job->url = applab::endpointFor(APPLAB_STORE.model);
  job->apiKey = APPLAB_STORE.geminiKey;
  job->body = applab::buildRequestBody(applab::kSystemInstruction,
                                       applab::userMessage(kind, request_, code_, problems_), kMaxOutputTokens);
  if (!applab::startGeminiJob(job)) {
    job->release();
    fail(tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  job_ = job;
  phase_ = Phase::Asking;
  phaseStartedMs_ = millis();
  lastSecond_ = 0;
  setStatus(formatted(fixes_ == 0 ? tr(STR_APPLAB_STATUS_ASKING) : tr(STR_APPLAB_STATUS_FIXING), 0),
            fixes_ == 0 ? request_ : problems_.front());
}

void AppLabGenerateActivity::handleReply() {
  applab::GeminiJob* job = job_;
  job_ = nullptr;
  const int status = job->httpStatus;
  const auto error = job->error;
  std::string reply = std::move(job->reply);
  job->release();

  if (error != applab::GeminiJob::Error::None) {
    fail(error == applab::GeminiJob::Error::NoMemory ? tr(STR_HERMES_ERR_MEMORY) : tr(STR_HERMES_ERR_UNREACHABLE));
    return;
  }
  std::string text;
  std::string message;
  const auto parsed = applab::extractText(reply, text, message);
  if (parsed == applab::ParseResult::ApiError || (status >= 400 && parsed != applab::ParseResult::Ok)) {
    fail(message.empty() ? formatted(tr(STR_HERMES_ERR_HTTP), static_cast<unsigned>(status)) : message);
    return;
  }
  if (parsed == applab::ParseResult::Blocked) {
    fail(std::string(tr(STR_APPLAB_ERR_BLOCKED)) + " " + message);
    return;
  }
  if (parsed == applab::ParseResult::TooLong) {
    fail(tr(STR_APPLAB_ERR_TOO_LONG));
    return;
  }
  std::string name;
  std::string lua;
  if (parsed != applab::ParseResult::Ok || applab::parseApp(text, name, lua) != applab::ParseResult::Ok) {
    fail(tr(STR_APPLAB_ERR_NO_APP));
    return;
  }
  code_ = std::move(lua);
  if (appName_.empty()) appName_ = name.empty() ? std::string(tr(STR_APPLAB_UNTITLED)) : name;

  setStatus(tr(STR_APPLAB_STATUS_CHECKING));
  const Rect bounds = LabAppActivity::canvasBoundsFor(renderer);
  problems_ = applab::checkApp(code_, bounds.width, bounds.height);
  for (const auto& problem : problems_) LOG_INF("APPLAB", "Check: %s", problem.c_str());
  if (!problems_.empty() && fixes_ < kMaxFixes) {
    ++fixes_;
    sendRequest();
    return;
  }

  // Saved even when problems remain after the fixes: the user can still try
  // it, and "Change it" sends the code back for another round.
  applab::AppInfo info;
  info.id = appId_;
  info.name = appName_;
  info.prompt = request_;
  if (kind_ == applab::RequestKind::Change) {
    applab::AppInfo previous;
    std::string previousCode;
    if (applab::readApp(appId_, previous, previousCode)) info.prompt = previous.prompt + "\n" + request_;
  }
  if (!applab::saveApp(info, code_)) {
    fail(tr(STR_APPLAB_ERR_SAVE));
    return;
  }
  LOG_INF("APPLAB", "Saved %s (%u bytes, %d fixes, %u problems left)", info.id.c_str(),
          static_cast<unsigned>(code_.size()), fixes_, static_cast<unsigned>(problems_.size()));
  phase_ = Phase::Done;
  teardownWifi();
  setResult(FilePathResult{info.id});
  finish();
}

void AppLabGenerateActivity::loop() {
  if (phase_ == Phase::Wifi && ownsWifi_ && wifiDeadlineMs_ != 0) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiDeadlineMs_ = 0;
      sendRequest();
    } else if (static_cast<int32_t>(millis() - wifiDeadlineMs_) >= 0) {
      wifiDeadlineMs_ = 0;
      fail(tr(STR_WIFI_CONN_FAILED));
    }
  }
  if (phase_ == Phase::Asking && job_) {
    if (job_->state.load() != applab::GeminiJob::State::Running) {
      handleReply();
      return;
    }
    const uint32_t seconds = (millis() - phaseStartedMs_) / 1000;
    if (seconds != lastSecond_) {
      lastSecond_ = seconds;
      setStatus(formatted(fixes_ == 0 ? tr(STR_APPLAB_STATUS_ASKING) : tr(STR_APPLAB_STATUS_FIXING), seconds),
                detail_);
    }
  }
  UiListActivity::loop();
}

const char* AppLabGenerateActivity::headerTitle() const { return tr(STR_APPLAB_TITLE); }

void AppLabGenerateActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin({static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                           static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  rows_[0] = {};
  rows_[0].label = status_.c_str();
  rows_[0].subtitle = detail_.empty() ? nullptr : detail_.c_str();
  rows_[0].actionValue = 0;
  rows_[1] = {};
  rows_[1].label = phase_ == Phase::Failed ? tr(STR_BACK) : tr(STR_CANCEL);
  rows_[1].actionValue = 1;
  fui::ListProps props;
  props.items = rows_;
  props.count = 2;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void AppLabGenerateActivity::activateIndex(const int index) {
  if (index == 1) onBackButton();
}

void AppLabGenerateActivity::onBackButton() {
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  finish();
}

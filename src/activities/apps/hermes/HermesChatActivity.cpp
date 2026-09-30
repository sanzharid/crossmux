#include "HermesChatActivity.h"

#include <ArduinoJson.h>
#include <BleKeyboardHost.h>
#include <I18n.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdio>

#include "BleInput.h"
#include "CrossPointSettings.h"
#include "HermesSettingsActivity.h"
#include "HermesStore.h"
#include "NetworkStartup.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "HalStorage.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr char kHistoryPath[] = "/.crosspoint/hermes_chat.json";
constexpr size_t kMaxMessages = 24;
constexpr size_t kMaxSavedMessageBytes = 6000;
constexpr size_t kMaxDraftBytes = 2000;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;
constexpr uint32_t kBleRetryMs = 5000;
constexpr uint32_t kMinRecordingSamples = 16000 / 3;  // ~0.33 s
constexpr int kBodyFont = NOTOSANS_14_FONT_ID;
constexpr int kLabelFont = UI_10_FONT_ID;
constexpr int kDraftFont = UI_12_FONT_ID;
constexpr int kMinButtonGap = 8;
constexpr char kBooksFolder[] = "/Books/Hermes";
constexpr uint32_t kDownloadTimeoutMs = 10 * 60 * 1000;  // AGENTS rule 14: >= 6 px between controls

// Removes the last UTF-8 code point.
void popCodePoint(std::string& s) {
  if (s.empty()) return;
  size_t i = s.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80) --i;
  s.erase(i);
}

bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle

void HermesChatActivity::onEnter() {
  Activity::onEnter();
  HERMES_STORE.loadFromFile();
  loadHistory();
  mappedInput.setBleTextMode(true);
  lastTickMs_ = millis();
  if (launch_ == Launch::PushToTalk) beginPushToTalk();
  requestUpdate();
}

void HermesChatActivity::beginPushToTalk() {
  // Swallow the release that ends this hold even if recording can't start
  // (no mic / no STT URL), so it isn't read as a Confirm click afterwards.
  pushToTalk_ = true;
  if (phase_ == Phase::Idle) toggleRecording();
}

void HermesChatActivity::onExit() {
  // ActivityManager already holds the render lock here: release work without
  // re-locking (the render mutex is not recursive).
  releaseWork();
  phase_ = Phase::Idle;
  mappedInput.setBleTextMode(false);
  teardownWifi();
  saveHistory();
  Activity::onExit();
}

void HermesChatActivity::loop() {
  pollJobs();
  pollWifi();
  maintainBluetooth();
  handleBleKeys();
  handleButtons();
  handleTouch();

  // Tick the elapsed-seconds status while busy. Recording ticks every second so
  // the user sees it is listening; agent turns can run for minutes, so they
  // tick every 5 s to limit e-paper partial refreshes.
  if (phase_ == Phase::Thinking || phase_ == Phase::Recording) {
    const uint32_t now = millis();
    const uint32_t interval = phase_ == Phase::Recording ? 1000 : 5000;
    if (now - lastTickMs_ >= interval) {
      lastTickMs_ = now;
      requestUpdate();
    }
  }
}

// ---------------------------------------------------------------------------
// Input

void HermesChatActivity::handleBleKeys() {
  MappedInputManager::BleTextKey key;
  bool changed = false;
  while (mappedInput.popBleTextKey(key)) {
    using freeink::SpecialKey;
    const auto special = static_cast<SpecialKey>(key.special);
    if (special == SpecialKey::None) {
      if (key.ch == 0 || draft_.size() >= kMaxDraftBytes) continue;
      RenderLock lock(*this);
      draft_ += key.ch;
      changed = true;
      continue;
    }
    switch (special) {
      case SpecialKey::Enter:
        submitDraft();
        break;
      case SpecialKey::Backspace: {
        RenderLock lock(*this);
        popCodePoint(draft_);
        changed = true;
        break;
      }
      case SpecialKey::Escape:
        if (phase_ != Phase::Idle) {
          cancelWork();
        } else {
          RenderLock lock(*this);
          draft_.clear();
          changed = true;
        }
        break;
      case SpecialKey::Up:
        scrollBy(3);
        break;
      case SpecialKey::Down:
        scrollBy(-3);
        break;
      case SpecialKey::PageUp:
        scrollBy(std::max(1, visibleRows_ - 1));
        break;
      case SpecialKey::PageDown:
        scrollBy(-std::max(1, visibleRows_ - 1));
        break;
      default:
        break;
    }
  }
  if (changed) requestUpdate();
}

void HermesChatActivity::handleButtons() {
  using Button = MappedInputManager::Button;
  if (pushToTalk_) {
    // Hold-to-talk: releasing the AI (power) button ends the clip. The release
    // itself is consumed here so it never doubles as a Confirm click.
    if (!mappedInput.isPressed(Button::Power)) {
      pushToTalk_ = false;
      if (phase_ == Phase::Recording && rec_) rec_->stop = true;
    }
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    if (phase_ != Phase::Idle) {
      cancelWork();
      return;
    }
    if (launch_ == Launch::PushToTalk) {
      finish();  // back to the screen the AI button was held on
    } else {
      activityManager.goToApps();
    }
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    // Voice is hold-to-talk only (main.cpp handles the AI-button hold); a
    // Confirm click types or sends, like the Type/Send buttons.
    if (phase_ == Phase::Idle) {
      if (draft_.empty()) {
        openKeyboard();
      } else {
        submitDraft();
      }
    }
    return;
  }
  const int page = std::max(1, visibleRows_ - 1);
  if (mappedInput.wasReleased(Button::PageBack) || mappedInput.wasReleased(Button::Up) ||
      mappedInput.wasReleased(Button::Left)) {
    scrollBy(page);
  } else if (mappedInput.wasReleased(Button::PageForward) || mappedInput.wasReleased(Button::Down) ||
             mappedInput.wasReleased(Button::Right)) {
    scrollBy(-page);
  }
}

void HermesChatActivity::handleTouch() {
  const Layout l = layout();
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    for (int i = 0; i < kActionCount; ++i) {
      if (contains(l.buttons[i], x, y)) {
        runAction(static_cast<Action>(i));
        return;
      }
    }
    if (contains(l.draft, x, y) && phase_ == Phase::Idle) openKeyboard();
    if (contains(l.transcript, x, y)) openTappedBook(y);
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  const int page = std::max(1, visibleRows_ - 1);
  if (swipe == MappedInputManager::SwipeDir::Down) {
    scrollBy(page);  // drag content down: reveal older lines
  } else if (swipe == MappedInputManager::SwipeDir::Up) {
    scrollBy(-page);
  }
}

void HermesChatActivity::runAction(const Action action) {
  switch (action) {
    case Action::Type:
      if (phase_ == Phase::Idle) openKeyboard();
      break;
    case Action::Talk:
      if (phase_ == Phase::Idle || phase_ == Phase::Recording) {
        toggleRecording();
      } else {
        cancelWork();  // the Talk slot doubles as Cancel while busy
      }
      break;
    case Action::Send:
      submitDraft();
      break;
    case Action::Setup:
      if (phase_ == Phase::Idle) openSettings();
      break;
    case Action::Count:
      break;
  }
}

bool HermesChatActivity::openTappedBook(const int y) {
  std::string path;
  {
    RenderLock lock(*this);
    for (const TapTarget& target : bookTargets_) {
      if (y >= target.top && y < target.bottom && target.message >= 0 &&
          target.message < static_cast<int>(messages_.size())) {
        path = messages_[static_cast<size_t>(target.message)].bookPath;
        break;
      }
    }
  }
  if (path.empty() || phase_ != Phase::Idle) return false;
  if (!Storage.exists(path.c_str())) {
    addMessage(Role::Notice, tr(STR_HERMES_ERR_FILE));
    return false;
  }
  activityManager.goToReader(path);
  return true;
}

void HermesChatActivity::scrollBy(const int lines) {
  RenderLock lock(*this);
  const int maxScroll = std::max(0, static_cast<int>(lines_.size()) - visibleRows_);
  const int next = std::max(0, std::min(maxScroll, scrollLines_ + lines));
  if (next == scrollLines_) return;
  scrollLines_ = next;
  lock.unlock();
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Flows

void HermesChatActivity::openKeyboard() {
  mappedInput.setBleTextMode(false);
  const bool started = startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        mappedInput.setBleTextMode(true);
        if (result.isCancelled) return;
        {
          RenderLock lock(*this);
          draft_ = std::get<KeyboardResult>(result.data).text;
        }
        submitDraft();
      },
      tr(STR_HERMES_TITLE), draft_, kMaxDraftBytes, InputType::Text);
  if (!started) mappedInput.setBleTextMode(true);
}

void HermesChatActivity::openSettings() {
  mappedInput.setBleTextMode(false);
  const bool started = startActivityForResultWith<HermesSettingsActivity>([this](const ActivityResult&) {
    mappedInput.setBleTextMode(true);
    if (HermesSettingsActivity::takeNewConversationRequest()) {
      RenderLock lock(*this);
      messages_.clear();
      scrollLines_ = 0;
      ++messagesVersion_;
      lock.unlock();
      saveHistory();
    }
  });
  if (!started) mappedInput.setBleTextMode(true);
}

void HermesChatActivity::submitDraft() {
  if (phase_ != Phase::Idle) return;
  std::string text;
  {
    RenderLock lock(*this);
    const size_t first = draft_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return;
    text = draft_.substr(first);
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    draft_.clear();
  }
  sendText(text);
}

void HermesChatActivity::sendText(const std::string& text) {
  if (!HERMES_STORE.hasChatConfig()) {
    addMessage(Role::Notice, tr(STR_HERMES_ERR_NO_URL));
    return;
  }
  addMessage(Role::User, text);
  pending_ = Pending::Chat;
  pendingText_ = text;
  if (ensureWifi()) dispatchPending();
}

void HermesChatActivity::toggleRecording() {
  if (phase_ == Phase::Recording) {
    if (rec_) rec_->stop = true;  // pollJobs() picks up the finished clip
    return;
  }
  if (phase_ != Phase::Idle) return;
  if (!hermes::micAvailable()) {
    addMessage(Role::Notice, tr(STR_HERMES_ERR_NO_MIC));
    return;
  }
  if (hermes::micBusy()) return;  // a cancelled clip is still shutting the mic down
  if (!HERMES_STORE.hasVoiceConfig()) {
    addMessage(Role::Notice, tr(STR_HERMES_ERR_NO_STT));
    return;
  }
  rec_ = hermes::RecordJob::create(HERMES_STORE.maxRecordSeconds);
  if (!rec_) {
    addMessage(Role::Notice, tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  if (!hermes::startRecordJob(rec_)) {
    rec_->release();
    rec_ = nullptr;
    addMessage(Role::Notice, tr(STR_HERMES_ERR_MIC));
    return;
  }
  discardRecording_ = false;
  RenderLock lock(*this);
  phase_ = Phase::Recording;
  phaseStartedMs_ = millis();
  lock.unlock();
  requestUpdate();
}

void HermesChatActivity::finishRecording() {
  hermes::RecordJob* rec = rec_;
  rec_ = nullptr;
  {
    RenderLock lock(*this);
    phase_ = Phase::Idle;
  }
  if (discardRecording_ || rec->state == hermes::JobState::Failed) {
    if (!discardRecording_) addError(rec->error, nullptr, true);
    rec->release();
    requestUpdate();
    return;
  }
  if (rec->samples < kMinRecordingSamples) {
    rec->release();
    addMessage(Role::Notice, tr(STR_HERMES_ERR_HEARD_NOTHING));
    return;
  }
  const size_t wavBytes = rec->finalizeWav();
  hermes::NetJob* job = hermes::NetJob::create();
  if (!job) {
    LOG_ERR("HERMES", "OOM: NetJob");
    rec->release();
    addMessage(Role::Notice, tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  job->kind = hermes::NetJob::Kind::Transcribe;
  job->url = HERMES_STORE.sttUrl;
  job->apiKey = HERMES_STORE.sttKey;
  job->sttModel = HERMES_STORE.sttModel;
  job->sttLanguage = HERMES_STORE.sttLanguage;
  job->audio = std::move(rec->buffer);
  job->audioBytes = wavBytes;
  job->timeoutMs = 60000;
  rec->release();

  if (pendingJob_) pendingJob_->release();
  pendingJob_ = job;
  pending_ = Pending::Transcribe;
  if (ensureWifi()) dispatchPending();
}

void HermesChatActivity::releaseWork() {
  if (rec_) {
    discardRecording_ = true;
    rec_->stop = true;
    rec_->release();
    rec_ = nullptr;
  }
  if (job_) {
    job_->cancel = true;
    job_->downloadCancel = true;
    job_->release();
    job_ = nullptr;
  }
  pendingDownloads_.clear();
  if (pendingJob_) {
    pendingJob_->release();
    pendingJob_ = nullptr;
  }
  pending_ = Pending::None;
}

void HermesChatActivity::cancelWork() {
  const bool wasBusy = phase_ != Phase::Idle;
  releaseWork();
  {
    RenderLock lock(*this);
    phase_ = Phase::Idle;
  }
  if (wasBusy) afterRequest();
  requestUpdate();
}

void HermesChatActivity::pollJobs() {
  if (rec_ && rec_->state != hermes::JobState::Running) finishRecording();
  if (!job_ || job_->state == hermes::JobState::Running) return;
  hermes::NetJob* job = job_;
  job_ = nullptr;
  {
    RenderLock lock(*this);
    phase_ = Phase::Idle;
  }
  if (job->kind == hermes::NetJob::Kind::Chat) {
    onChatDone(*job);
  } else if (job->kind == hermes::NetJob::Kind::Download) {
    onDownloadDone(*job);
  } else {
    onTranscribeDone(*job);
  }
  job->release();
}

void HermesChatActivity::onChatDone(hermes::NetJob& job) {
  if (job.state != hermes::JobState::Done) {
    addError(job.error, &job, false);
    afterRequest();
    return;
  }
  // Links first: plainText() drops Markdown link targets.
  if (HERMES_STORE.downloadBooks) pendingDownloads_ = hermes::findBookLinks(job.text);
  addMessage(Role::Agent, hermes::plainText(job.text));
  startNextDownload();  // Wi-Fi is still up; afterRequest() runs when the queue is empty
}

void HermesChatActivity::startNextDownload() {
  if (pendingDownloads_.empty()) {
    afterRequest();
    return;
  }
  const std::string url = pendingDownloads_.front();
  pendingDownloads_.erase(pendingDownloads_.begin());

  if (!Storage.exists(kBooksFolder) && !Storage.mkdir(kBooksFolder)) {
    LOG_ERR("HERMES", "Cannot create %s", kBooksFolder);
    addError(hermes::JobError::FileError, nullptr, false);
    pendingDownloads_.clear();
    afterRequest();
    return;
  }
  // Never overwrite: "Title.epub", "Title (2).epub", ...
  const std::string name = hermes::bookFileName(url);
  const size_t dot = name.find_last_of('.');
  std::string dest = std::string(kBooksFolder) + "/" + name;
  for (int n = 2; Storage.exists(dest.c_str()) && n < 100; ++n) {
    dest = std::string(kBooksFolder) + "/" + name.substr(0, dot) + " (" + std::to_string(n) + ")" + name.substr(dot);
  }
  if (Storage.exists(dest.c_str())) {  // 99 copies already: never overwrite
    addError(hermes::JobError::FileError, nullptr, false);
    startNextDownload();
    return;
  }

  hermes::NetJob* job = hermes::NetJob::create();
  if (!job) {
    LOG_ERR("HERMES", "OOM: NetJob");
    addError(hermes::JobError::NoMemory, nullptr, false);
    pendingDownloads_.clear();
    afterRequest();
    return;
  }
  job->kind = hermes::NetJob::Kind::Download;
  job->url = url;
  job->destPath = dest;
  job->timeoutMs = kDownloadTimeoutMs;
  if (!hermes::startNetJob(job)) {
    job->release();
    addError(hermes::JobError::NoMemory, nullptr, false);
    pendingDownloads_.clear();
    afterRequest();
    return;
  }
  LOG_INF("HERMES", "Downloading book to %s", dest.c_str());
  job_ = job;
  RenderLock lock(*this);
  phase_ = Phase::Downloading;
  phaseStartedMs_ = millis();
  lock.unlock();
  requestUpdate();
}

void HermesChatActivity::onDownloadDone(hermes::NetJob& job) {
  if (job.state == hermes::JobState::Done) {
    const size_t slash = job.text.find_last_of('/');
    char buffer[160];
    snprintf(buffer, sizeof(buffer), tr(STR_HERMES_BOOK_SAVED), job.text.substr(slash + 1).c_str());
    addMessage(Role::Book, buffer, job.text);
  } else {
    addError(job.error, &job, false);
  }
  startNextDownload();
}

void HermesChatActivity::onTranscribeDone(hermes::NetJob& job) {
  if (job.state != hermes::JobState::Done) {
    addError(job.error, &job, true);
    afterRequest();
    return;
  }
  if (HERMES_STORE.voiceAutoSend) {
    sendText(job.text);  // Wi-Fi is already up
    if (phase_ == Phase::Idle && pending_ == Pending::None) afterRequest();  // nothing was sent
    return;
  }
  {
    RenderLock lock(*this);
    if (!draft_.empty()) draft_ += ' ';
    draft_ += job.text;
  }
  afterRequest();
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Radios

bool HermesChatActivity::ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
  const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!credential) {
    openWifiPicker();
    return false;
  }
  WiFi.persistent(false);
  NetworkStartup::setMode(renderer, WIFI_STA);
  WiFi.disconnect(true, true);
  delay(100);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }
  ownsWifi_ = true;
  wifiDeadlineMs_ = millis() + kWifiConnectTimeoutMs;
  RenderLock lock(*this);
  phase_ = Phase::WifiConnecting;
  phaseStartedMs_ = millis();
  lock.unlock();
  requestUpdate();
  return false;
}

void HermesChatActivity::openWifiPicker() {
  mappedInput.setBleTextMode(false);
  ownsWifi_ = true;  // the picker powers the radio and leaves it on for us
  const bool started = startActivityForResultWith<WifiSelectionActivity>(
      [this](const ActivityResult& result) {
        mappedInput.setBleTextMode(true);
        if (!result.isCancelled && WiFi.status() == WL_CONNECTED) {
          dispatchPending();
        } else {
          cancelWork();
          teardownWifi();  // let the BLE keyboard back on
          addMessage(Role::Notice, tr(STR_WIFI_CONN_FAILED));
        }
      },
      true);
  if (!started) {
    mappedInput.setBleTextMode(true);
    cancelWork();
    addMessage(Role::Notice, tr(STR_HERMES_ERR_MEMORY));
  }
}

void HermesChatActivity::pollWifi() {
  if (phase_ != Phase::WifiConnecting) return;
  if (WiFi.status() == WL_CONNECTED) {
    dispatchPending();
    return;
  }
  if (static_cast<int32_t>(millis() - wifiDeadlineMs_) >= 0) {
    LOG_ERR("HERMES", "Wi-Fi association timed out");
    cancelWork();
    addMessage(Role::Notice, tr(STR_WIFI_CONN_FAILED));
  }
}

void HermesChatActivity::dispatchPending() {
  hermes::NetJob* job = nullptr;
  Phase next = Phase::Thinking;
  if (pending_ == Pending::Chat) {
    job = hermes::NetJob::create();
    if (job) {
      const bool responses = HERMES_STORE.chatApi == HermesStore::ChatApi::Responses;
      job->kind = hermes::NetJob::Kind::Chat;
      job->style = responses ? hermes::ApiStyle::Responses : hermes::ApiStyle::ChatCompletions;
      job->url = hermes::joinUrl(HERMES_STORE.chatUrl, responses ? "/v1/responses" : "/v1/chat/completions");
      job->apiKey = HERMES_STORE.chatKey;
      job->body = responses
                      ? hermes::buildResponsesBody(HERMES_STORE.chatModel, HERMES_STORE.conversation, pendingText_)
                      : hermes::buildChatCompletionsBody(HERMES_STORE.chatModel, pendingText_);
      job->sessionId = HERMES_STORE.conversation;
      job->timeoutMs = static_cast<uint32_t>(HERMES_STORE.timeoutSeconds) * 1000;
    } else {
      LOG_ERR("HERMES", "OOM: NetJob");
    }
  } else if (pending_ == Pending::Transcribe) {
    job = pendingJob_;
    pendingJob_ = nullptr;
    next = Phase::Transcribing;
  }
  pending_ = Pending::None;
  pendingText_.clear();
  if (!job) {
    RenderLock lock(*this);
    phase_ = Phase::Idle;
    lock.unlock();
    addMessage(Role::Notice, tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  if (!hermes::startNetJob(job)) {
    job->release();
    RenderLock lock(*this);
    phase_ = Phase::Idle;
    lock.unlock();
    addMessage(Role::Notice, tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  job_ = job;
  RenderLock lock(*this);
  phase_ = next;
  phaseStartedMs_ = millis();
  lastTickMs_ = phaseStartedMs_;
  lock.unlock();
  requestUpdate();
}

void HermesChatActivity::teardownWifi() {
  if (!ownsWifi_) return;
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  esp_wifi_deinit();
  ownsWifi_ = false;
}

void HermesChatActivity::afterRequest() {
  // Give the radio back to the BLE keyboard; without one, keep Wi-Fi warm.
  if (SETTINGS.bluetoothEnabled) teardownWifi();
}

void HermesChatActivity::maintainBluetooth() {
#if FREEINK_CAP_BLE_HID_HOST
  if (phase_ != Phase::Idle || !SETTINGS.bluetoothEnabled || WiFi.getMode() != WIFI_MODE_NULL) return;
  if (bleinput::isRunning() || static_cast<int32_t>(millis() - nextBleAttemptMs_) < 0) return;
  RenderLock lock;
  const auto result = bleinput::ensureStarted(renderer, bleinput::StartContext::Explicit);
  if (result != bleinput::StartResult::Started && result != bleinput::StartResult::AlreadyRunning) {
    nextBleAttemptMs_ = millis() + kBleRetryMs;
  }
#endif
}

// ---------------------------------------------------------------------------
// Transcript

void HermesChatActivity::addMessage(const Role role, std::string text, std::string bookPath) {
  {
    RenderLock lock(*this);
    if (messages_.size() >= kMaxMessages) messages_.erase(messages_.begin());
    messages_.push_back(Message{role, std::move(text), std::move(bookPath)});
    scrollLines_ = 0;
    ++messagesVersion_;
  }
  if (role != Role::Notice) saveHistory();
  requestUpdate();
}

void HermesChatActivity::addError(const hermes::JobError error, const hermes::NetJob* job, const bool transcription) {
  using hermes::JobError;
  char buffer[96];
  const char* message = tr(STR_HERMES_ERR_BAD_REPLY);
  switch (error) {
    case JobError::InvalidUrl:
      message = tr(STR_HERMES_ERR_URL);
      break;
    case JobError::Unreachable:
      message = tr(STR_HERMES_ERR_UNREACHABLE);
      break;
    case JobError::Cancelled:
      message = tr(STR_HERMES_ERR_CANCELLED);
      break;
    case JobError::NoText:
      message = transcription ? tr(STR_HERMES_ERR_HEARD_NOTHING) : tr(STR_HERMES_ERR_NO_TEXT);
      break;
    case JobError::HttpStatus:
      snprintf(buffer, sizeof(buffer), tr(STR_HERMES_ERR_HTTP), job ? job->httpStatus : 0);
      message = buffer;
      break;
    case JobError::NoMemory:
      message = tr(STR_HERMES_ERR_MEMORY);
      break;
    case JobError::MicUnavailable:
      message = tr(STR_HERMES_ERR_NO_MIC);
      break;
    case JobError::MicFailed:
      message = tr(STR_HERMES_ERR_MIC);
      break;
    case JobError::FileError:
      message = tr(STR_HERMES_ERR_FILE);
      break;
    case JobError::BadReply:
    case JobError::None:
      break;
  }
  std::string text = message;
  if (job && !job->serverMessage.empty()) {
    text += ": ";
    text += job->serverMessage;
  }
  addMessage(Role::Notice, std::move(text));
}

void HermesChatActivity::loadHistory() {
  JsonDocument doc;
  RenderLock lock(*this);
  messages_.clear();
  if (PersistableStoreBase::readDocFromFile(kHistoryPath, doc)) {
    JsonArrayConst items = doc["messages"].as<JsonArrayConst>();
    messages_.reserve(std::min(items.size(), kMaxMessages));
    for (JsonVariantConst item : items) {
      if (messages_.size() >= kMaxMessages) break;
      const uint8_t role = item["r"] | static_cast<uint8_t>(0);
      const Role r = role == 1 ? Role::Agent : role == 2 ? Role::Book : Role::User;
      messages_.push_back(Message{r, std::string(item["t"] | ""), std::string(item["p"] | "")});
    }
  }
  scrollLines_ = 0;
  ++messagesVersion_;
}

void HermesChatActivity::saveHistory() const {
  JsonDocument doc;
  JsonArray items = doc["messages"].to<JsonArray>();
  for (const Message& message : messages_) {
    if (message.role == Role::Notice) continue;
    JsonObject item = items.add<JsonObject>();
    item["r"] = message.role == Role::Agent ? 1 : message.role == Role::Book ? 2 : 0;
    if (message.role == Role::Book) item["p"] = message.bookPath;
    if (message.text.size() > kMaxSavedMessageBytes) {
      std::string clipped = message.text.substr(0, kMaxSavedMessageBytes);
      popCodePoint(clipped);  // never split a code point
      item["t"] = clipped;
    } else {
      item["t"] = message.text;
    }
  }
  if (!PersistableStoreBase::writeDocToFile(kHistoryPath, doc)) LOG_ERR("HERMES", "Failed to save chat history");
}

void HermesChatActivity::rebuildLines(const int width) {
  lines_.clear();
  for (size_t index = 0; index < messages_.size(); ++index) {
    const Message& message = messages_[index];
    const auto id = static_cast<int16_t>(index);
    const char* label = message.role == Role::User    ? tr(STR_HERMES_YOU)
                        : message.role == Role::Agent ? tr(STR_HERMES_AGENT)
                                                      : nullptr;
    if (!lines_.empty()) lines_.push_back(Line{std::string(), message.role, false, -1});
    if (label) lines_.push_back(Line{label, message.role, true, id});
    size_t pos = 0;
    const std::string& text = message.text;
    while (pos <= text.size()) {
      size_t end = text.find('\n', pos);
      if (end == std::string::npos) end = text.size();
      const std::string paragraph = text.substr(pos, end - pos);
      if (paragraph.empty()) {
        lines_.push_back(Line{std::string(), message.role, false, id});
      } else {
        const EpdFontFamily::Style style = message.role == Role::Notice ? EpdFontFamily::ITALIC
                                           : message.role == Role::Book ? EpdFontFamily::BOLD
                                                                        : EpdFontFamily::REGULAR;
        for (std::string& wrapped : renderer.wrappedText(kBodyFont, paragraph.c_str(), width, 400, style)) {
          lines_.push_back(Line{std::move(wrapped), message.role, false, id});
        }
      }
      if (end >= text.size()) break;
      pos = end + 1;
    }
  }
  linesVersion_ = messagesVersion_;
  linesWidth_ = width;
}

// ---------------------------------------------------------------------------
// Rendering

HermesChatActivity::Layout HermesChatActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int left = safe.x + metrics.contentSidePadding;
  const int width = std::max(0, safe.width - 2 * metrics.contentSidePadding);
  const int top = safe.y + metrics.topPadding;
  const int bottom = safe.y + safe.height - metrics.verticalSpacing;

  Layout l;
  l.header = Rect{left, top, width, metrics.headerHeight};

  const int buttonHeight = std::max(44, renderer.getLineHeight(UI_10_FONT_ID) * 2 + 8);
  const int buttonTop = bottom - buttonHeight;
  const int gap = std::max(kMinButtonGap, metrics.verticalSpacing);
  const int buttonWidth = (width - gap * (kActionCount - 1)) / kActionCount;
  for (int i = 0; i < kActionCount; ++i) {
    l.buttons[i] = Rect{left + i * (buttonWidth + gap), buttonTop, buttonWidth, buttonHeight};
  }

  const int draftHeight = renderer.getLineHeight(kDraftFont) * 2 + 16;
  l.draft = Rect{left, buttonTop - gap - draftHeight, width, draftHeight};

  const int transcriptTop = l.header.y + l.header.height + metrics.verticalSpacing / 2;
  l.transcript = Rect{left, transcriptTop, width, std::max(0, l.draft.y - gap - transcriptTop)};
  return l;
}

const char* HermesChatActivity::actionLabel(const Action action) const {
  switch (action) {
    case Action::Type:
      return tr(STR_HERMES_TYPE);
    case Action::Talk:
      if (phase_ == Phase::Recording) return tr(STR_HERMES_STOP);
      if (phase_ != Phase::Idle) return tr(STR_CANCEL);
      return tr(STR_HERMES_TALK);
    case Action::Send:
      return tr(STR_HERMES_SEND);
    case Action::Setup:
      return tr(STR_HERMES_SETUP);
    case Action::Count:
      break;
  }
  return "";
}

std::string HermesChatActivity::statusText() const {
  char buffer[96];
  const auto elapsed = static_cast<unsigned>((millis() - phaseStartedMs_) / 1000);
  switch (phase_) {
    case Phase::WifiConnecting:
      return tr(STR_HERMES_STATUS_WIFI);
    case Phase::Thinking:
      snprintf(buffer, sizeof(buffer), tr(STR_HERMES_STATUS_THINKING), elapsed);
      return buffer;
    case Phase::Recording:
      snprintf(buffer, sizeof(buffer), tr(STR_HERMES_STATUS_LISTENING), elapsed);
      return buffer;
    case Phase::Transcribing:
      return tr(STR_HERMES_STATUS_TRANSCRIBING);
    case Phase::Downloading:
      return tr(STR_HERMES_STATUS_DOWNLOADING);
    case Phase::Idle:
      break;
  }
  if (bleinput::isConnected()) {
    snprintf(buffer, sizeof(buffer), tr(STR_HERMES_STATUS_KEYBOARD), BleHid.connectedName());
    return buffer;
  }
  return tr(STR_HERMES_STATUS_READY);
}

void HermesChatActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Layout l = layout();
  const std::string status = statusText();
  GUI.drawHeader(renderer, l.header, tr(STR_HERMES_TITLE), status.c_str());

  // Transcript, newest at the bottom.
  if (linesVersion_ != messagesVersion_ || linesWidth_ != l.transcript.width) rebuildLines(l.transcript.width);
  const int bodyHeight = renderer.getLineHeight(kBodyFont);
  const int labelHeight = renderer.getLineHeight(kLabelFont);
  visibleRows_ = std::max(1, l.transcript.height / bodyHeight);
  if (lines_.empty()) {
    const auto hint = renderer.wrappedText(UI_12_FONT_ID, tr(STR_HERMES_EMPTY), l.transcript.width, 8);
    int y = l.transcript.y + l.transcript.height / 3;
    for (const std::string& line : hint) {
      const int w = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
      renderer.drawText(UI_12_FONT_ID, l.transcript.x + (l.transcript.width - w) / 2, y, line.c_str());
      y += renderer.getLineHeight(UI_12_FONT_ID);
    }
  } else {
    const int total = static_cast<int>(lines_.size());
    scrollLines_ = std::min(scrollLines_, std::max(0, total - visibleRows_));
    const GfxRenderer::ClipScope clip(renderer, l.transcript.x, l.transcript.y, l.transcript.width,
                                      l.transcript.height);
    // Walk up from the newest visible line, stacking mixed-height rows.
    int y = l.transcript.y + l.transcript.height;
    bookTargets_.clear();
    for (int i = total - 1 - scrollLines_; i >= 0; --i) {
      const Line& line = lines_[static_cast<size_t>(i)];
      const int h = line.label ? labelHeight + 2 : bodyHeight;
      y -= h;
      if (y < l.transcript.y) break;
      if (line.text.empty()) continue;
      if (line.label) {
        renderer.drawText(kLabelFont, l.transcript.x, y, line.text.c_str(), true, EpdFontFamily::BOLD);
      } else {
        const EpdFontFamily::Style style = line.role == Role::Notice ? EpdFontFamily::ITALIC
                                           : line.role == Role::Book    ? EpdFontFamily::BOLD
                                                                        : EpdFontFamily::REGULAR;
        renderer.drawText(kBodyFont, l.transcript.x, y, line.text.c_str(), true, style);
        if (line.role == Role::Book) {
          // Underline marks it as tappable (opens the book).
          const int w = renderer.getTextWidth(kBodyFont, line.text.c_str(), style);
          renderer.drawLine(l.transcript.x, y + h - 3, l.transcript.x + w, y + h - 3, 1, true);
          bookTargets_.push_back(
              TapTarget{static_cast<int16_t>(y), static_cast<int16_t>(y + h), line.message});
        }
      }
    }
    if (total > visibleRows_) {
      GUI.drawSideScrollBar(renderer, l.transcript, total, std::max(0, total - visibleRows_ - scrollLines_),
                            visibleRows_);
    }
  }

  // Draft box: the last two wrapped lines, so the cursor end stays visible.
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.drawRoundedRect(l.draft.x, l.draft.y, l.draft.width, l.draft.height, 1,
                           metrics.optionPopupSelectionRadius, true);
  {
    const int pad = 8;
    const int innerWidth = std::max(1, l.draft.width - 2 * pad);
    const int lineHeight = renderer.getLineHeight(kDraftFont);
    if (draft_.empty()) {
      renderer.drawText(kDraftFont, l.draft.x + pad, l.draft.y + pad, tr(STR_HERMES_DRAFT_HINT), true,
                        EpdFontFamily::ITALIC);
    } else {
      const std::string withCursor = draft_ + "_";
      auto wrapped = renderer.wrappedText(kDraftFont, withCursor.c_str(), innerWidth, 64);
      const size_t first = wrapped.size() > 2 ? wrapped.size() - 2 : 0;
      int y = l.draft.y + pad;
      for (size_t i = first; i < wrapped.size(); ++i) {
        renderer.drawText(kDraftFont, l.draft.x + pad, y, wrapped[i].c_str());
        y += lineHeight;
      }
    }
  }

  for (int i = 0; i < kActionCount; ++i) {
    const auto action = static_cast<Action>(i);
    const bool active = (action == Action::Talk && phase_ != Phase::Idle) ||
                        (action == Action::Send && !draft_.empty() && phase_ == Phase::Idle);
    GUI.drawActionButton(renderer, l.buttons[i], actionLabel(action), active);
  }

  const char* confirm = tr(STR_HERMES_TYPE);
  GUI.drawButtonHints(renderer, tr(STR_BACK), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

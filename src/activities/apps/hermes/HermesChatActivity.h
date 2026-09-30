#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "HermesJobs.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

// Chat with a Hermes Agent over its OpenAI-compatible API server.
//
// Input: the on-screen keyboard (Type), a BLE keyboard (typed straight into the
// draft line), or voice (AI/Confirm button or Talk) transcribed by an
// OpenAI-compatible speech-to-text endpoint. Replies render as a scrollable
// transcript; side buttons and swipes scroll.
//
// Radio budget: CrossMux never runs Wi-Fi and BLE together (NetworkStartup
// stops BLE). With Bluetooth enabled the app brings Wi-Fi up per request and
// tears it down afterwards so the keyboard reconnects; otherwise Wi-Fi stays up
// while the app is open.
class HermesChatActivity final : public Activity {
 public:
  explicit HermesChatActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Hermes", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return phase_ != Phase::Idle; }
  bool keepsBluetoothAlive() const override { return true; }

 private:
  enum class Phase : uint8_t { Idle, WifiConnecting, Thinking, Recording, Transcribing };
  enum class Pending : uint8_t { None, Chat, Transcribe };
  enum class Role : uint8_t { User, Agent, Notice };
  enum class Action : uint8_t { Type, Talk, Send, Setup, Count };

  struct Message {
    Role role;
    std::string text;
  };
  struct Line {
    std::string text;
    Role role;
    bool label;
  };

  // --- state (mutated under RenderLock; read by render()) ---
  std::vector<Message> messages_;
  std::string draft_;
  Phase phase_ = Phase::Idle;
  int scrollLines_ = 0;  // lines scrolled up from the newest
  uint32_t phaseStartedMs_ = 0;
  uint32_t messagesVersion_ = 1;

  // Render-side wrap cache (only touched on the render task).
  std::vector<Line> lines_;
  uint32_t linesVersion_ = 0;
  int linesWidth_ = 0;
  int visibleRows_ = 1;

  // --- work in flight ---
  hermes::NetJob* job_ = nullptr;
  hermes::RecordJob* rec_ = nullptr;
  bool discardRecording_ = false;
  Pending pending_ = Pending::None;
  std::string pendingText_;
  hermes::NetJob* pendingJob_ = nullptr;  // transcription waiting for Wi-Fi

  // --- radios ---
  bool ownsWifi_ = false;
  uint32_t wifiDeadlineMs_ = 0;
  uint32_t nextBleAttemptMs_ = 0;
  uint32_t lastTickMs_ = 0;

  // Input
  void handleBleKeys();
  void handleTouch();
  void handleButtons();
  void runAction(Action action);
  void scrollBy(int lines);

  // Flows
  void openKeyboard();
  void openSettings();
  void submitDraft();
  void sendText(const std::string& text);
  void toggleRecording();
  void finishRecording();
  void cancelWork();
  void releaseWork();  // drops jobs without touching phase_ or the render lock
  void pollJobs();
  void onChatDone(hermes::NetJob& job);
  void onTranscribeDone(hermes::NetJob& job);

  // Radios
  bool ensureWifi();  // true = connected now; false = connecting/picker opened
  void openWifiPicker();
  void pollWifi();
  void dispatchPending();
  void teardownWifi();
  void afterRequest();
  void maintainBluetooth();

  // Transcript
  void addMessage(Role role, std::string text);
  void addError(hermes::JobError error, const hermes::NetJob* job, bool transcription);
  void loadHistory();
  void saveHistory() const;
  void rebuildLines(int width);

  // Layout (shared by render and hit-testing)
  static constexpr int kActionCount = static_cast<int>(Action::Count);
  struct Layout {
    Rect header;
    Rect transcript;
    Rect draft;
    Rect buttons[kActionCount];
  };
  Layout layout() const;
  const char* actionLabel(Action action) const;
  std::string statusText() const;
};

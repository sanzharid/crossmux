#pragma once

#include <cstdint>
#include <string>

#include <I18n.h>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

// Full-screen PIN pad.
//
// Unlock: pushed over whatever is open when the device locks; finish()es on
// the right PIN (or a tap on "Unlock" when no PIN is set). While it is on top,
// ActivityManager and main.cpp skip every global shortcut (tabs, home swipe,
// push-to-talk) via blocksGlobalShortcuts().
// Verify / Enter: used by Settings to check the current PIN and to collect a
// new one; they return KeyboardResult{pin} (empty = "remove PIN") or cancel.
class LockScreenActivity final : public Activity {
 public:
  static constexpr const char* kName = "Lock";
  enum class Mode : uint8_t { Unlock, Verify, Enter };

  LockScreenActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::Unlock,
                     StrId prompt = StrId::STR_LOCK_ENTER_PIN, bool offerRemove = false)
      : Activity(kName, renderer, mappedInput), mode_(mode), prompt_(prompt), offerRemove_(offerRemove) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool blocksGlobalShortcuts() const override { return mode_ == Mode::Unlock; }
  bool handleHomeGesture() override { return true; }  // swallow the home swipe

 private:
  static constexpr int kColumns = 3;
  static constexpr int kRows = 4;
  static constexpr int kKeyCount = kColumns * kRows;

  struct Layout {
    Rect title;
    Rect dots;
    Rect message;
    Rect keys[kKeyCount];
    Rect unlock;  // single button when no PIN is set
  };

  const Mode mode_;
  const StrId prompt_;
  const bool offerRemove_;
  bool pinRequired_ = true;
  std::string entry_;
  StrId message_ = StrId::STR_NONE_OPT;
  uint32_t lastCountdownS_ = 0;

  Layout layout() const;
  void press(int key);
  void submit();
  void done(std::string pin);
  const char* keyLabel(int key) const;
};

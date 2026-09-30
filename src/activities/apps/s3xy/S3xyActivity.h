#pragma once

#include <cstdint>
#include <string>

#include "S3xyLink.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

// "S3XY" app: the device acts as one S3XY Button for an Enhance Auto
// Commander while this screen is open. Three large tiles send the button's
// single / double / long press (their car actions are assigned in the S3XY
// app); the page buttons send single (Up) and double (Down). Tile labels are
// user-editable and stored in /.crosspoint/s3xy.json.
class S3xyActivity final : public Activity {
 public:
  static constexpr const char* kName = "S3XY";

  explicit S3xyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(kName, renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Keep the Commander link up while it is in use (a car usually powers the device).
  bool preventAutoSleep() override;

 private:
  static constexpr int kTileCount = 3;
  static constexpr int kSentFlashMs = 600;

  struct Layout {
    Rect header;
    Rect status;
    Rect tiles[kTileCount];
    Rect edit;
  };

  std::string labels_[kTileCount];
  bool started_ = false;
  bool editing_ = false;
  int flashTile_ = -1;
  uint32_t flashUntilMs_ = 0;
  bool sendFailed_ = false;
  s3xy::LinkState lastState_ = s3xy::LinkState::Off;

  Layout layout() const;
  void press(int tile);
  void editLabel(int tile);
  void loadLabels();
  void saveLabels() const;
};

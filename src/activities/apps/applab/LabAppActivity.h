#pragma once

#include <CrossStoreCanvas.h>
#include <CrossStoreRuntime.h>

#include <cstdio>
#include <memory>
#include <string>

#include "activities/UiListActivity.h"

// Runs one App Lab app. A slimmed-down CrossStore LuaAppActivity: the same Lua
// runtime and canvas, but no permissions at all (no network, sound, routes,
// notifications or assets), with storage under the App Lab folder.
class LabAppActivity final : public UiListActivity, private crossstore::Host {
 public:
  LabAppActivity(GfxRenderer& renderer, MappedInputManager& input, const std::string& id)
      : UiListActivity("LabApp", renderer, input, true), runtime_(*this) {
    std::snprintf(id_, sizeof(id_), "%s", id.c_str());
  }
  void onEnter() override;
  void onExit() override;
  void loop() override;

  // The canvas area an app gets on this screen, shared with the sandbox check.
  static Rect canvasBoundsFor(const GfxRenderer& renderer);

 private:
  std::unique_ptr<crossstore::Canvas> canvas_;
  crossstore::Runtime runtime_;
  bool canvasMode_ = false;
  bool exitPending_ = false;
  bool active_ = false;
  bool dirty_ = false;
  bool inputPending_ = false;
  bool keyReleasePending_ = false;
  char id_[32]{};
  char heading_[96]{};
  char inputTitle_[96]{};
  char inputText_[192]{};
  char labels_[crossstore::MAX_ROWS][96]{};
  char values_[crossstore::MAX_ROWS][192]{};
  freeink::ui::ListItem rows_[crossstore::MAX_ROWS]{};
  size_t count_ = 0;
  unsigned columns_ = 0;
  char caption_[192]{};
  uint32_t tickAt_ = 0;
  int lastWidth_ = 0;
  int lastHeight_ = 0;

  void showError(const char* detail);
  void dispatch(const char* event, int index = 0, const char* text = "", int x = -1, int y = -1);
  void guardHeldButtons();
  bool handleKeyInput();
  Rect canvasBounds() const { return canvasBoundsFor(renderer); }
  void buildCanvas(UiScreen& screen);
  static void canvasAction(const freeink::ui::ActionEvent& event, void* user);

  // UiListActivity
  int listCount() const override {
    return static_cast<int>(canvasMode_ && canvas_ ? canvas_->controlCount : count_);
  }
  const char* headerTitle() const override { return heading_; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleListSwipe(MappedInputManager::SwipeDir swipe, int x, int y) override;

  // crossstore::Host
  void* resizeAllocation(void* ptr, size_t bytes) override;
  void title(const char* text) override;
  void clear() override;
  void grid(unsigned columns) override {
    columns_ = columns;
    dirty_ = true;
  }
  void caption(const char* text) override;
  bool row(const char* label, const char* value) override;
  bool capability(const char*) const override { return false; }
  bool chinese() const override;
  uint32_t milliseconds() const override;
  bool read(const char* key, char* out, size_t capacity) override;
  bool write(const char* key, const char* value, size_t size) override;
  void input(const char* title, const char* initial) override;
  void notify(const char*) override {}
  bool open(const char*) override { return false; }
  void size(int& width, int& height) const override;
  bool canvas() override;
  bool draw(const crossstore::DrawCommand& command, const char* text) override;
  void exit() override { exitPending_ = true; }
  void measure(const char* text, int maximum, unsigned font, int& width, int& height) override;
  bool system(const char* command, const char*, int64_t, crossstore::ServiceReply& reply) override;
};

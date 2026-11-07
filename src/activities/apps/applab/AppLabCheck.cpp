#include "AppLabCheck.h"

#include <CrossStoreCanvas.h>
#include <CrossStoreRuntime.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

#include "AppLabFiles.h"
#include "AppLabProtocol.h"

namespace applab {

namespace {

constexpr size_t kVmBytes = 131072;
constexpr size_t kMaxProblems = 6;
constexpr uint32_t kTickMs = 500;

// The device host's behaviour, minus the screen: draws go to a Canvas so the
// same limits apply, storage stays in memory, and time advances per event.
class HeadlessHost final : public crossstore::Host {
 public:
  HeadlessHost(const int width, const int height) : width_(width), height_(height) {}

  bool ready() const { return canvas_ != nullptr; }
  bool allocate() {
    canvas_ = makeUniqueNoThrow<crossstore::Canvas>();
    return canvas_ != nullptr;
  }
  void advance(const uint32_t ms) { now_ += ms; }
  size_t controlCount() const { return canvasMode_ ? canvas_->controlCount : 0; }
  int controlAction(const size_t index) const { return canvas_->commands[canvas_->controls[index]].action; }
  size_t rows() const { return rows_; }
  bool drewSomething() const { return canvasMode_ ? canvas_->count > 0 : rows_ > 0 || caption_; }
  const std::string& rejection() const { return rejection_; }
  void clearRejection() { rejection_.clear(); }

  void* resizeAllocation(void* ptr, const size_t bytes) override {
    if (!bytes) {
      heap_caps_free(ptr);
      return nullptr;
    }
    return heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  void title(const char*) override {}
  void clear() override {
    canvas_->clear();
    canvasMode_ = false;
    rows_ = 0;
    caption_ = false;
  }
  void grid(unsigned) override {}
  void caption(const char*) override { caption_ = true; }
  bool row(const char*, const char*) override {
    if (rows_ == crossstore::MAX_ROWS) {
      reject("more than 32 rows");
      return false;
    }
    ++rows_;
    return true;
  }
  bool capability(const char*) const override { return false; }
  bool chinese() const override { return false; }
  uint32_t milliseconds() const override { return now_; }
  bool read(const char* key, char* out, const size_t capacity) override {
    const auto found = data_.find(key);
    if (found == data_.end() || found->second.size() >= capacity) return false;
    std::memcpy(out, found->second.c_str(), found->second.size() + 1);
    return true;
  }
  bool write(const char* key, const char* value, const size_t size) override {
    if (!isValidId(key) || size > 2048 || (data_.size() >= 32 && !data_.count(key))) return false;
    data_[key].assign(value, size);
    return true;
  }
  void input(const char*, const char*) override {}
  void notify(const char*) override {}
  bool open(const char*) override { return false; }
  void size(int& width, int& height) const override {
    width = width_;
    height = height_;
  }
  bool canvas() override {
    canvasMode_ = true;
    return true;
  }
  bool draw(const crossstore::DrawCommand& command, const char* text) override {
    if (!canvasMode_) {
      reject("drew without calling app.canvas() after app.clear()");
      return false;
    }
    const bool interactive =
        command.kind == crossstore::DrawKind::Button || command.kind == crossstore::DrawKind::Hit;
    if (interactive && (command.x < 0 || command.y < 0 || command.x + command.width > width_ ||
                        command.y + command.height > height_)) {
      char detail[160];
      std::snprintf(detail, sizeof(detail), "control %d at (%d,%d,%dx%d) is outside the %dx%d canvas", command.action,
                    command.x, command.y, command.width, command.height, width_, height_);
      reject(detail);
      return false;
    }
    if (canvas_->add(command, text)) return true;
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "a %s was rejected: overlapping or duplicate control id %d, zero size, or over the frame limits",
                  interactive ? "control" : "drawing", command.action);
    reject(detail);
    return false;
  }
  void measure(const char* text, const int maximum, const unsigned font, int& width, int& height) override {
    static constexpr int kCharWidth[] = {9, 11, 13, 22};
    static constexpr int kLineHeight[] = {18, 24, 30, 48};
    const unsigned index = font < 4 ? font : 1;
    const int full = static_cast<int>(std::strlen(text)) * kCharWidth[index];
    const int lines = maximum > 0 ? (full + maximum - 1) / maximum : 1;
    width = full < maximum ? full : maximum;
    height = (lines > 0 ? lines : 1) * kLineHeight[index];
  }
  bool system(const char* command, const char*, int64_t, crossstore::ServiceReply& reply) override {
    return systemCall(command, reply);
  }

 private:
  void reject(const char* detail) {
    if (rejection_.empty()) rejection_ = detail;
  }

  const int width_;
  const int height_;
  std::unique_ptr<crossstore::Canvas> canvas_;
  bool canvasMode_ = false;
  bool caption_ = false;
  size_t rows_ = 0;
  uint32_t now_ = 1000;
  std::map<std::string, std::string> data_;
  std::string rejection_;
};

class Checker {
 public:
  Checker(const int width, const int height) : host_(width, height), runtime_(host_) {}

  std::vector<std::string> run(const std::string& lua) {
    if (!host_.allocate()) return {"the device ran out of memory while checking the app"};
    if (!runtime_.load(lua.data(), lua.size(), kVmBytes)) {
      add("loading", runtime_.error());
      return problems_;
    }
    if (!event("on_start")) return problems_;
    event("on_resize");
    if (!host_.drewSomething()) problems_.emplace_back("on_start drew nothing: no canvas drawing and no rows");
    for (int i = 0; i < 3; ++i) {
      host_.advance(kTickMs);
      event("on_tick");
    }
    // Tap every control of the current frame, then every list row.
    const size_t controls = host_.controlCount();
    std::vector<int> actions;
    for (size_t i = 0; i < controls; ++i) actions.push_back(host_.controlAction(i));
    for (const int action : actions) {
      if (problems_.size() >= kMaxProblems) break;
      event("on_select", action, "", 1, 1);
      event("on_tick");
    }
    const size_t rows = host_.rows();
    for (size_t i = 1; i <= rows && problems_.size() < kMaxProblems; ++i) event("on_select", static_cast<int>(i));
    event("on_swipe", 0, "left");
    event("on_key", 0, "down");
    event("on_stop");
    return problems_;
  }

 private:
  bool event(const char* name, const int index = 0, const char* text = "", const int x = -1, const int y = -1) {
    host_.clearRejection();
    host_.advance(1);
    const bool ok = runtime_.event(name, index, text, x, y);
    if (!ok) {
      // A rejected draw raises a generic Lua error; the host knows why.
      if (host_.rejection().empty()) {
        add(name, runtime_.error());
      } else {
        add(name, (host_.rejection() + " (" + runtime_.error() + ")").c_str());
      }
      return false;
    }
    if (!host_.rejection().empty()) add(name, host_.rejection().c_str());
    return true;
  }

  void add(const char* during, const char* what) {
    if (problems_.size() >= kMaxProblems) return;
    char line[256];
    std::snprintf(line, sizeof(line), "during %s: %s", during, what);
    for (const auto& existing : problems_) {
      if (existing == line) return;
    }
    problems_.emplace_back(line);
  }

  HeadlessHost host_;
  crossstore::Runtime runtime_;
  std::vector<std::string> problems_;
};

}  // namespace

std::vector<std::string> checkApp(const std::string& lua, const int width, const int height) {
  if (lua.size() > crossstore::MAX_SCRIPT_BYTES) return {"the program is over 32768 bytes; make it shorter"};
  // The canvas and the runtime state are large: build the checker on the heap.
  auto checker = makeUniqueNoThrow<Checker>(width, height);
  if (!checker) return {"the device ran out of memory while checking the app"};
  return checker->run(lua);
}

}  // namespace applab

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

// "System" app: a task manager for the device. Overview shows firmware,
// uptime, CPU load, memory, storage, battery, sensors and radios; Tasks lists
// every FreeRTOS task with its CPU share since the last sample, core, priority,
// state and free stack. Samples every 2 s; page buttons and swipes scroll.
class SystemMonitorActivity final : public Activity {
 public:
  explicit SystemMonitorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SystemMonitor", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Overview, Tasks };
  struct Row {
    std::string label;
    std::string value;
    bool heading;
  };
  struct TaskSample {
    std::string name;
    uint32_t runtime;
    int core;
    unsigned priority;
    char state;
    uint32_t stackFreeBytes;
    uint16_t cpuPermille;  // share of CPU time since the previous sample
  };
  struct Layout {
    Rect header;
    Rect body;
    Rect toggle;
  };

  static constexpr uint32_t kSampleMs = 2000;

  Page page_ = Page::Overview;
  int scroll_ = 0;
  int visibleRows_ = 1;
  uint32_t lastSampleMs_ = 0;
  std::vector<Row> overview_;
  std::vector<TaskSample> tasks_;
  std::vector<TaskSample> previousTasks_;
  uint32_t previousTotalRuntime_ = 0;
  uint16_t cpuLoadPermille_ = 0;
  // Storage and sensor reads are slow or wake hardware; refresh them less often.
  uint64_t sdTotal_ = 0;
  uint64_t sdFree_ = 0;
  bool sdKnown_ = false;
  float tempC_ = 0;
  float humidity_ = 0;
  bool envKnown_ = false;
  uint8_t slowCounter_ = 0;

  void sample();
  void sampleTasks();
  void buildOverview();
  void scrollBy(int rows);
  int rowCount() const;
  Layout layout() const;
};

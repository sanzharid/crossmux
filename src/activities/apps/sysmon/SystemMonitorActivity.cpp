#include "SystemMonitorActivity.h"

#include <BatteryMonitor.h>
#include <BleKeyboardHost.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

#if SYSMON_ENV_SENSOR
#include <EnvironmentSensor.h>
#endif

namespace {

constexpr int kMinGap = 10;
constexpr int kLabelFont = UI_12_FONT_ID;

bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

std::string bytes(const uint64_t value) {
  char buffer[32];
  if (value >= 1024ULL * 1024 * 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(value) / (1024.0 * 1024 * 1024));
  } else if (value >= 1024ULL * 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(value) / (1024.0 * 1024));
  } else {
    snprintf(buffer, sizeof(buffer), "%u KB", static_cast<unsigned>(value / 1024));
  }
  return buffer;
}

std::string uptime() {
  const uint64_t s = esp_timer_get_time() / 1000000ULL;
  char buffer[32];
  if (s >= 86400) {
    snprintf(buffer, sizeof(buffer), "%ud %02u:%02u:%02u", static_cast<unsigned>(s / 86400),
             static_cast<unsigned>(s % 86400 / 3600), static_cast<unsigned>(s % 3600 / 60),
             static_cast<unsigned>(s % 60));
  } else {
    snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u", static_cast<unsigned>(s / 3600),
             static_cast<unsigned>(s % 3600 / 60), static_cast<unsigned>(s % 60));
  }
  return buffer;
}

char stateChar(const eTaskState state) {
  switch (state) {
    case eRunning:
      return 'R';
    case eReady:
      return 'r';
    case eBlocked:
      return 'B';
    case eSuspended:
      return 'S';
    case eDeleted:
      return 'D';
    default:
      return '?';
  }
}

}  // namespace

void SystemMonitorActivity::onEnter() {
  Activity::onEnter();
  slowCounter_ = 0;
  sample();
  requestUpdate();
}

void SystemMonitorActivity::sampleTasks() {
  const UBaseType_t count = uxTaskGetNumberOfTasks() + 4;
  // Transient heap copy: one TaskStatus_t per task, freed at the end of the sample.
  auto* status = static_cast<TaskStatus_t*>(heap_caps_malloc(count * sizeof(TaskStatus_t), MALLOC_CAP_8BIT));
  if (!status) {
    LOG_ERR("SYSMON", "OOM: task snapshot");
    return;
  }
  uint32_t totalRuntime = 0;
  const UBaseType_t n = uxTaskGetSystemState(status, count, &totalRuntime);

  std::vector<TaskSample> now;
  now.reserve(n);
  uint32_t idleDelta = 0;
  const uint32_t totalDelta = totalRuntime - previousTotalRuntime_;
  for (UBaseType_t i = 0; i < n; ++i) {
    const TaskStatus_t& t = status[i];
    TaskSample s;
    s.name = t.pcTaskName ? t.pcTaskName : "?";
    s.runtime = t.ulRunTimeCounter;
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    s.core = t.xCoreID == tskNO_AFFINITY ? -1 : static_cast<int>(t.xCoreID);
#else
    s.core = -1;
#endif
    s.priority = static_cast<unsigned>(t.uxCurrentPriority);
    s.state = stateChar(t.eCurrentState);
    s.stackFreeBytes = static_cast<uint32_t>(t.usStackHighWaterMark) * sizeof(StackType_t);
    s.cpuPermille = 0;
    uint32_t delta = 0;
    for (const auto& prev : previousTasks_) {
      if (prev.name == s.name) {
        delta = s.runtime - prev.runtime;
        break;
      }
    }
    // Run-time counters are per task; two cores contribute, so normalise to
    // the combined capacity of both.
    if (previousTotalRuntime_ != 0 && totalDelta > 0) {
      const uint64_t capacity = static_cast<uint64_t>(totalDelta) * portNUM_PROCESSORS;
      s.cpuPermille = static_cast<uint16_t>(std::min<uint64_t>(1000, delta * 1000ULL / capacity));
      if (s.name.rfind("IDLE", 0) == 0) idleDelta += delta;
    }
    now.push_back(std::move(s));
  }
  heap_caps_free(status);

  if (previousTotalRuntime_ != 0 && totalDelta > 0) {
    const uint64_t capacity = static_cast<uint64_t>(totalDelta) * portNUM_PROCESSORS;
    cpuLoadPermille_ = static_cast<uint16_t>(1000 - std::min<uint64_t>(1000, idleDelta * 1000ULL / capacity));
  }
  previousTasks_ = now;
  previousTotalRuntime_ = totalRuntime;
  std::sort(now.begin(), now.end(), [](const TaskSample& a, const TaskSample& b) {
    return a.cpuPermille != b.cpuPermille ? a.cpuPermille > b.cpuPermille : a.name < b.name;
  });
  tasks_ = std::move(now);
}

void SystemMonitorActivity::buildOverview() {
  overview_.clear();
  char buffer[96];
  const auto heading = [&](const StrId id) { overview_.push_back(Row{I18N.get(id), std::string(), true}); };
  const auto row = [&](const StrId id, std::string value) {
    overview_.push_back(Row{I18N.get(id), std::move(value), false});
  };

  heading(StrId::STR_SYSMON_DEVICE);
  row(StrId::STR_SYSMON_FIRMWARE, CROSSPOINT_VERSION);
  row(StrId::STR_SYSMON_UPTIME, uptime());
  snprintf(buffer, sizeof(buffer), "%u MHz, %u%% load", static_cast<unsigned>(getCpuFrequencyMhz()),
           static_cast<unsigned>((cpuLoadPermille_ + 5) / 10));
  row(StrId::STR_SYSMON_CPU, buffer);
  snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(uxTaskGetNumberOfTasks()));
  row(StrId::STR_SYSMON_TASKS, buffer);

  heading(StrId::STR_SYSMON_MEMORY);
  constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  snprintf(buffer, sizeof(buffer), "%s free / %s", bytes(heap_caps_get_free_size(kInternal)).c_str(),
           bytes(heap_caps_get_total_size(kInternal)).c_str());
  row(StrId::STR_SYSMON_RAM, buffer);
  snprintf(buffer, sizeof(buffer), "%s low, %s block", bytes(heap_caps_get_minimum_free_size(kInternal)).c_str(),
           bytes(heap_caps_get_largest_free_block(kInternal)).c_str());
  row(StrId::STR_SYSMON_RAM_DETAIL, buffer);
  const size_t psramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psramTotal > 0) {
    snprintf(buffer, sizeof(buffer), "%s free / %s", bytes(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)).c_str(),
             bytes(psramTotal).c_str());
    row(StrId::STR_SYSMON_PSRAM, buffer);
  }
  if (sdKnown_) {
    snprintf(buffer, sizeof(buffer), "%s free / %s", bytes(sdFree_).c_str(), bytes(sdTotal_).c_str());
    row(StrId::STR_SYSMON_SD, buffer);
  } else {
    row(StrId::STR_SYSMON_SD, tr(STR_SYSMON_UNAVAILABLE));
  }

  heading(StrId::STR_SYSMON_POWER);
  static const BatteryMonitor battery;
  bool externalKnown = false;
  const bool external = battery.isExternalPowerPresent(&externalKnown);
  snprintf(buffer, sizeof(buffer), "%u%%, %.2f V", static_cast<unsigned>(powerManager.getBatteryPercentage()),
           battery.readMillivolts() / 1000.0);
  row(StrId::STR_SYSMON_BATTERY, buffer);
  row(StrId::STR_SYSMON_CHARGING, battery.isCharging()               ? tr(STR_SYSMON_CHARGING_YES)
                                  : externalKnown && external        ? tr(STR_SYSMON_PLUGGED_FULL)
                                                                     : tr(STR_SYSMON_ON_BATTERY));
  if (envKnown_) {
    snprintf(buffer, sizeof(buffer), "%.1f C, %.0f%% RH", tempC_, humidity_);
    row(StrId::STR_SYSMON_ENVIRONMENT, buffer);
  }

  heading(StrId::STR_SYSMON_RADIOS);
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_NULL) {
    row(StrId::STR_SYSMON_WIFI, tr(STR_STATE_OFF));
  } else if (WiFi.status() == WL_CONNECTED) {
    snprintf(buffer, sizeof(buffer), "%s, %d dBm", WiFi.SSID().c_str(), static_cast<int>(WiFi.RSSI()));
    row(StrId::STR_SYSMON_WIFI, buffer);
    row(StrId::STR_SYSMON_IP, WiFi.localIP().toString().c_str());
  } else {
    row(StrId::STR_SYSMON_WIFI, tr(STR_SYSMON_NOT_CONNECTED));
  }
  if (!BleHid.isRunning()) {
    row(StrId::STR_SYSMON_BLUETOOTH, tr(STR_STATE_OFF));
  } else if (BleHid.isConnected()) {
    row(StrId::STR_SYSMON_BLUETOOTH, BleHid.connectedName());
  } else {
    row(StrId::STR_SYSMON_BLUETOOTH, tr(STR_SYSMON_NOT_CONNECTED));
  }
}

void SystemMonitorActivity::sample() {
  // Every 5th sample (~10 s): SD space scan and sensor read.
  if (slowCounter_++ % 5 == 0) {
    sdKnown_ = Storage.getSpace(sdTotal_, sdFree_);
#if SYSMON_ENV_SENSOR
    static freeink::EnvironmentSensor env;
    static bool envStarted = false;
    if (!envStarted) envStarted = env.begin();
    envKnown_ = envStarted && env.read(tempC_, humidity_);
#endif
  }
  RenderLock lock(*this);
  sampleTasks();
  buildOverview();
  lastSampleMs_ = millis();
}

SystemMonitorActivity::Layout SystemMonitorActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int left = safe.x + metrics.contentSidePadding;
  const int width = std::max(0, safe.width - 2 * metrics.contentSidePadding);
  const int top = safe.y + metrics.topPadding;
  const int bottom = safe.y + safe.height - metrics.verticalSpacing;
  Layout l;
  l.header = Rect{left, top, width, metrics.headerHeight};
  const int gap = std::max(kMinGap, metrics.verticalSpacing);
  const int toggleH = std::max(44, renderer.getLineHeight(UI_10_FONT_ID) * 2 + 8);
  l.toggle = Rect{left, bottom - toggleH, width, toggleH};
  const int bodyTop = l.header.y + l.header.height + metrics.verticalSpacing / 2;
  l.body = Rect{left, bodyTop, width, std::max(0, l.toggle.y - gap - bodyTop)};
  return l;
}

int SystemMonitorActivity::rowCount() const {
  return page_ == Page::Overview ? static_cast<int>(overview_.size()) : static_cast<int>(tasks_.size()) + 1;
}

void SystemMonitorActivity::scrollBy(const int rows) {
  RenderLock lock(*this);
  const int next = std::max(0, std::min(scroll_ + rows, std::max(0, rowCount() - visibleRows_)));
  if (next == scroll_) return;
  scroll_ = next;
  lock.unlock();
  requestUpdate();
}

void SystemMonitorActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    activityManager.goToApps();
    return;
  }
  const int page = std::max(1, visibleRows_ - 1);
  if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack) ||
      mappedInput.wasReleased(Button::Left)) {
    scrollBy(-page);
  } else if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward) ||
             mappedInput.wasReleased(Button::Right)) {
    scrollBy(page);
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) scrollBy(page);
  if (swipe == MappedInputManager::SwipeDir::Down) scrollBy(-page);

  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y) && contains(layout().toggle, x, y)) {
    RenderLock lock(*this);
    page_ = page_ == Page::Overview ? Page::Tasks : Page::Overview;
    scroll_ = 0;
    lock.unlock();
    requestUpdate();
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    RenderLock lock(*this);
    page_ = page_ == Page::Overview ? Page::Tasks : Page::Overview;
    scroll_ = 0;
    lock.unlock();
    requestUpdate();
  }

  if (millis() - lastSampleMs_ >= kSampleMs) {
    sample();
    requestUpdate();
  }
}

void SystemMonitorActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Layout l = layout();
  GUI.drawHeader(renderer, l.header, tr(STR_SYSMON_TITLE),
                 page_ == Page::Overview ? tr(STR_SYSMON_OVERVIEW) : tr(STR_SYSMON_TASKS));
  const int lineH = renderer.getLineHeight(kLabelFont) + 4;
  visibleRows_ = std::max(1, l.body.height / lineH);
  scroll_ = std::min(scroll_, std::max(0, rowCount() - visibleRows_));

  int y = l.body.y;
  char buffer[96];
  if (page_ == Page::Overview) {
    for (int i = scroll_; i < static_cast<int>(overview_.size()) && y + lineH <= l.body.y + l.body.height; ++i) {
      const Row& r = overview_[static_cast<size_t>(i)];
      if (r.heading) {
        renderer.drawText(kLabelFont, l.body.x, y, r.label.c_str(), true, EpdFontFamily::BOLD);
        renderer.drawLine(l.body.x, y + lineH - 3, l.body.x + l.body.width, y + lineH - 3, 1, true);
      } else {
        renderer.drawText(kLabelFont, l.body.x + 8, y, r.label.c_str());
        const std::string value =
            renderer.truncatedText(kLabelFont, r.value.c_str(), l.body.width * 3 / 5, EpdFontFamily::REGULAR);
        const int w = renderer.getTextWidth(kLabelFont, value.c_str());
        renderer.drawText(kLabelFont, l.body.x + l.body.width - w, y, value.c_str());
      }
      y += lineH;
    }
  } else {
    // Columns: name | cpu | core/prio/state | free stack
    const int colCpu = l.body.x + l.body.width * 46 / 100;
    const int colInfo = l.body.x + l.body.width * 62 / 100;
    const int colStack = l.body.x + l.body.width;
    const auto rightText = [&](const int xRight, const char* text, const EpdFontFamily::Style style) {
      renderer.drawText(kLabelFont, xRight - renderer.getTextWidth(kLabelFont, text, style), y, text, true, style);
    };
    renderer.drawText(kLabelFont, l.body.x, y, tr(STR_SYSMON_COL_TASK), true, EpdFontFamily::BOLD);
    rightText(colInfo - 12, tr(STR_SYSMON_COL_CPU), EpdFontFamily::BOLD);
    renderer.drawText(kLabelFont, colInfo, y, tr(STR_SYSMON_COL_INFO), true, EpdFontFamily::BOLD);
    rightText(colStack, tr(STR_SYSMON_COL_STACK), EpdFontFamily::BOLD);
    renderer.drawLine(l.body.x, y + lineH - 3, l.body.x + l.body.width, y + lineH - 3, 1, true);
    y += lineH;
    for (int i = scroll_; i < static_cast<int>(tasks_.size()) && y + lineH <= l.body.y + l.body.height; ++i) {
      const TaskSample& t = tasks_[static_cast<size_t>(i)];
      const std::string name =
          renderer.truncatedText(kLabelFont, t.name.c_str(), colCpu - l.body.x - 8, EpdFontFamily::REGULAR);
      renderer.drawText(kLabelFont, l.body.x, y, name.c_str());
      snprintf(buffer, sizeof(buffer), "%u.%u%%", t.cpuPermille / 10, t.cpuPermille % 10);
      rightText(colInfo - 12, buffer, EpdFontFamily::REGULAR);
      if (t.core >= 0) {
        snprintf(buffer, sizeof(buffer), "c%d p%u %c", t.core, t.priority, t.state);
      } else {
        snprintf(buffer, sizeof(buffer), "-- p%u %c", t.priority, t.state);
      }
      renderer.drawText(kLabelFont, colInfo, y, buffer);
      rightText(colStack, bytes(t.stackFreeBytes).c_str(), EpdFontFamily::REGULAR);
      y += lineH;
    }
  }
  if (rowCount() > visibleRows_) {
    GUI.drawSideScrollBar(renderer, l.body, rowCount(), scroll_, visibleRows_);
  }

  GUI.drawActionButton(renderer, l.toggle, page_ == Page::Overview ? tr(STR_SYSMON_SHOW_TASKS)
                                                                    : tr(STR_SYSMON_SHOW_OVERVIEW),
                       false);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

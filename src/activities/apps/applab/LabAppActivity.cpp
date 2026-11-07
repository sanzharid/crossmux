#include "LabAppActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "AppLabFiles.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

// Lua VM budget: the Sticky has 8 MB PSRAM, so apps get the runtime's maximum.
constexpr size_t kVmBytes = crossstore::MAX_VM_BYTES;

fui::Color shade(const uint8_t gray) {
  if (gray <= 2) return fui::Color::Black;
  if (gray <= 7) return fui::Color::DarkGray;
  if (gray <= 12) return fui::Color::LightGray;
  return fui::Color::White;
}

}  // namespace

Rect LabAppActivity::canvasBoundsFor(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  const int padding = std::max(12, metrics.contentSidePadding);
  const int top = metrics.topPadding + metrics.headerHeight + 8;
  return Rect{safe.x + padding, safe.y + top, std::max(1, safe.width - padding * 2),
              std::max(1, safe.height - top - metrics.buttonHintsHeight - 8)};
}

void LabAppActivity::onEnter() {
  UiListActivity::onEnter();
  guardHeldButtons();
  app.on(ACTION_USER, &LabAppActivity::canvasAction, this);
  RenderLock lock;
  applab::AppInfo info;
  std::string source;
  if (!applab::readApp(id_, info, source)) {
    title(tr(STR_APPLAB_TITLE));
    showError(tr(STR_APPLAB_ERR_MISSING));
    return;
  }
  title(info.name.c_str());
  // Without BOARD_HAS_PSRAM (as in static analysis) the helper is constant true.
  // cppcheck-suppress knownConditionTrueFalse
  if (!memory::psramHasHeadroom(kVmBytes, kVmBytes, 0)) {
    showError(tr(STR_HERMES_ERR_MEMORY));
    return;
  }
  active_ = runtime_.load(source.data(), source.size(), kVmBytes);
  if (!active_) showError(runtime_.error());
  LOG_INF("APPLAB", "App %s: VM peak %u bytes", id_, static_cast<unsigned>(runtime_.peak()));
}

void LabAppActivity::onExit() {
  // Give a live app its final save callback before teardown (Home, sleep).
  if (active_ && !runtime_.event("on_stop")) LOG_ERR("APPLAB", "App %s final save failed: %s", id_, runtime_.error());
  runtime_.close();
  canvas_.reset();
  active_ = false;
  Activity::onExit();
}

void LabAppActivity::showError(const char* detail) {
  LOG_ERR("APPLAB", "App %s stopped: %s", id_, detail);
  active_ = false;
  inputPending_ = false;
  clear();
  row(tr(STR_APPLAB_APP_STOPPED), detail);
}

void LabAppActivity::dispatch(const char* event, const int index, const char* text, const int x, const int y) {
  if (!active_) return;
  RenderLock lock;
  if (!runtime_.event(event, index, text, x, y)) showError(runtime_.error());
}

void LabAppActivity::guardHeldButtons() {
  using Button = MappedInputManager::Button;
  keyReleasePending_ = false;
  for (const auto button : {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down,
                            Button::NavNext, Button::NavPrevious, Button::PageBack, Button::PageForward}) {
    keyReleasePending_ |= mappedInput.isPressed(button);
  }
}

bool LabAppActivity::handleKeyInput() {
  if (keyReleasePending_) {
    guardHeldButtons();
    return true;
  }
  if (!active_) return false;
  using Button = MappedInputManager::Button;
  constexpr std::array keys{Button::Up, Button::Down};
  return std::any_of(keys.begin(), keys.end(), [this](const auto button) {
    if (!mappedInput.wasPressed(button)) return false;
    dispatch("on_key", 0, button == Button::Up ? "up" : "down");
    if (!active_ || runtime_.handled()) {
      guardHeldButtons();
      return true;
    }
    return false;
  });
}

void LabAppActivity::loop() {
  if (exitPending_) {
    finish();
    return;
  }
  if (inputPending_) {
    inputPending_ = false;
    startActivityForResultWith<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          guardHeldButtons();
          if (!result.isCancelled) dispatch("on_input", 0, std::get<KeyboardResult>(result.data).text.c_str());
        },
        inputTitle_, inputText_, 191);
    return;
  }
  int width = 0;
  int height = 0;
  size(width, height);
  if (width != lastWidth_ || height != lastHeight_) {
    lastWidth_ = width;
    lastHeight_ = height;
    dispatch("on_resize");
  }
  if (millis() - tickAt_ >= 500) {
    tickAt_ = millis();
    dispatch("on_tick");
  }
  const bool buttonsConsumed = handleKeyInput();
  if (dirty_) {
    dirty_ = false;
    requestUpdate();
  }
  if (buttonsConsumed) {
    const auto touch = routeTouch(mappedInput, true);
    if (touch.routed && app.invalidated()) requestUpdate();
    if (!touch) {
      int x = 0;
      int y = 0;
      const auto swipe = mappedInput.wasSwipe(&x, &y);
      if (swipe != MappedInputManager::SwipeDir::None) handleListSwipe(swipe, x, y);
    }
    return;
  }
  UiListActivity::loop();
}

void LabAppActivity::buildScreen(UiScreen& screen) {
  if (canvasMode_ && canvas_) {
    buildCanvas(screen);
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  if (columns_) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
    const int x = safe.x + metrics.contentSidePadding;
    const int width = std::max(1, safe.width - 2 * metrics.contentSidePadding);
    int y = safe.y + metrics.topPadding + metrics.headerHeight + 8;
    const int bottom = safe.y + safe.height - metrics.buttonHintsHeight - 8;
    if (*caption_) {
      auto style = screen.theme().bodyText;
      style.align = fui::TextAlign::Center;
      screen.target().text({static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width),
                            static_cast<int16_t>(metrics.headerHeight)},
                           caption_, style);
      y += metrics.headerHeight + 12;
    }
    constexpr int gap = 6;
    const unsigned columns = std::min(columns_, static_cast<unsigned>(std::max(1, (width + gap) / 46)));
    const int visibleRows = std::max(1, (bottom - y + gap) / 46);
    const int capacity = std::min(20, visibleRows * static_cast<int>(columns));
    syncPagedViewport(capacity);
    fui::TileGridItem tiles[20]{};
    size_t visible = 0;
    for (size_t i = static_cast<size_t>(nav.top); i < count_ && visible < static_cast<size_t>(capacity); ++i) {
      tiles[visible].label = labels_[i];
      tiles[visible].value = static_cast<int16_t>(i);
      tiles[visible].state = static_cast<int>(i) == nav.selected ? fui::StateChecked : fui::StateNormal;
      ++visible;
    }
    fui::TileGridProps props;
    props.items = tiles;
    props.count = static_cast<uint16_t>(visible);
    props.action = ACTION_ROW;
    props.columns = columns;
    props.gap = gap;
    props.text = screen.theme().bodyText;
    fui::tileGrid(screen.frame(),
                  {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(width),
                   static_cast<int16_t>(std::max(1, bottom - y))},
                  props);
    return;
  }
  screen.setContentMargin({static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                           static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  fui::ListProps props;
  props.items = rows_;
  props.count = static_cast<uint16_t>(count_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void LabAppActivity::buildCanvas(UiScreen& screen) {
  syncPagedViewport(std::max(1, static_cast<int>(canvas_->controlCount)));
  auto& target = screen.target();
  const auto bounds = canvasBounds();
  const GfxRenderer::ClipScope clip(renderer, bounds.x, bounds.y, bounds.width, bounds.height);
  // A dedicated large-font slot leaves the theme's body/header fonts intact.
  uiTarget.setFont(fui::GfxRendererTarget::FONT_LABEL, CONTROL_18_FONT_ID);
  for (size_t i = 0; i < canvas_->count; ++i) {
    const auto& command = canvas_->commands[i];
    const fui::Rect rect{static_cast<int16_t>(bounds.x + command.x), static_cast<int16_t>(bounds.y + command.y),
                         command.width, command.height};
    const char* label = canvas_->text + command.text;
    const auto paint = fui::Paint::solid(shade(command.gray));
    switch (command.kind) {
      case crossstore::DrawKind::Box:
      case crossstore::DrawKind::Circle: {
        const int radius =
            command.kind == crossstore::DrawKind::Circle ? std::min(command.width, command.height) / 2 : command.radius;
        if (command.style) {
          target.stroke(rect, paint, command.style, radius);
        } else {
          target.fill(rect, paint, radius);
        }
        break;
      }
      case crossstore::DrawKind::Line:
        target.line({rect.x, static_cast<int16_t>(rect.y + (command.radius ? rect.height - 1 : 0))},
                    {static_cast<int16_t>(rect.x + rect.width - 1),
                     static_cast<int16_t>(rect.y + (command.radius ? 0 : rect.height - 1))},
                    std::max<uint8_t>(1, command.style), paint);
        break;
      case crossstore::DrawKind::Text: {
        auto style = screen.theme().bodyText;
        switch (command.font) {
          case 0:
            style = screen.theme().smallText;
            break;
          case 2:
            style = screen.theme().titleText;
            style.bold = true;
            break;
          case 3:
            style.font = fui::GfxRendererTarget::FONT_LABEL;
            style.bold = true;
            break;
          default:
            break;
        }
        style.align = static_cast<fui::TextAlign>(command.align);
        style.color = shade(command.gray);
        style.maxLines = std::max(1, rect.height / std::max(1, static_cast<int>(target.lineHeight(style.font))));
        target.text(rect, label, style);
        break;
      }
      case crossstore::DrawKind::Button: {
        const bool dark = command.style == 3;
        const auto background = dark                 ? fui::Color::Black
                                : command.style == 2 ? fui::Color::LightGray
                                                     : fui::Color::White;
        const auto foreground = dark ? fui::Color::White : fui::Color::Black;
        const int radius = std::min<int>(16, rect.height / 4);
        target.fill(rect, fui::Paint::solid(background), radius);
        if (command.style != 1) target.stroke(rect, fui::Paint::solid(foreground), 1, radius);
        auto style = screen.theme().bodyText;
        style.align = fui::TextAlign::Center;
        style.color = foreground;
        style.bold = command.style >= 2;
        if (command.style > 0 && std::strlen(label) <= 3 &&
            rect.height >= target.lineHeight(fui::GfxRendererTarget::FONT_LABEL) + 12) {
          style.font = fui::GfxRendererTarget::FONT_LABEL;
        }
        target.text(rect.inset({4, 6, 4, 6}), label, style);
        if (hasContentSelection() && nav.selected >= 0 && static_cast<size_t>(nav.selected) < canvas_->controlCount &&
            canvas_->controls[nav.selected] == i) {
          target.stroke(rect.inset({3, 3, 3, 3}), fui::Paint::solid(foreground), 2, std::max(0, radius - 3));
        }
        screen.frame().hit(rect, ACTION_USER, command.action, fui::InputTouch | fui::InputLongPress);
        break;
      }
      case crossstore::DrawKind::Hit:
        screen.frame().hit(rect, ACTION_USER, command.action, fui::InputTouch | fui::InputLongPress);
        break;
      case crossstore::DrawKind::Raster: {
        const int width = command.radius;
        const int height = command.font;
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width;) {
            const char value = label[y * width + x];
            int end = x + 1;
            while (end < width && label[y * width + end] == value) ++end;
            const auto color = shade(value <= '9' ? value - '0' : value - 'A' + 10);
            const int left = rect.x + x * rect.width / width;
            const int top = rect.y + y * rect.height / height;
            target.fill({static_cast<int16_t>(left), static_cast<int16_t>(top),
                         static_cast<int16_t>(rect.x + end * rect.width / width - left),
                         static_cast<int16_t>(rect.y + (y + 1) * rect.height / height - top)},
                        fui::Paint::solid(color));
            x = end;
          }
        }
        break;
      }
      case crossstore::DrawKind::Qr:
        break;  // QR codes are not offered to App Lab apps.
    }
  }
}

void LabAppActivity::activateIndex(const int index) {
  if (canvasMode_ && canvas_) {
    if (index >= 0 && static_cast<size_t>(index) < canvas_->controlCount) {
      dispatch("on_select", canvas_->commands[canvas_->controls[index]].action);
    }
    return;
  }
  if (index < 0 || static_cast<size_t>(index) >= count_) return;
  app.clearTapFlash();
  dispatch("on_select", index + 1);
}

void LabAppActivity::canvasAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<LabAppActivity*>(user);
  self.app.clearTapFlash();
  int x = -1;
  int y = -1;
  if (event.longPress ? self.mappedInput.wasScreenLongPress(x, y) : self.mappedInput.wasScreenTapped(x, y)) {
    const auto bounds = self.canvasBounds();
    x -= bounds.x;
    y -= bounds.y;
  }
  self.dispatch("on_select", event.value, event.longPress ? "long" : "", x, y);
}

void LabAppActivity::onBackButton() {
  if (!active_) {
    finish();
    return;
  }
  dispatch("on_back");
  if (!runtime_.handled()) finish();
}

bool LabAppActivity::handleListSwipe(const MappedInputManager::SwipeDir swipe, const int x, const int y) {
  if (!canvasMode_) return UiListActivity::handleListSwipe(swipe, x, y);
  const char* direction = nullptr;
  switch (swipe) {
    case MappedInputManager::SwipeDir::Left:
      direction = "left";
      break;
    case MappedInputManager::SwipeDir::Right:
      direction = "right";
      break;
    case MappedInputManager::SwipeDir::Up:
      direction = "up";
      break;
    case MappedInputManager::SwipeDir::Down:
      direction = "down";
      break;
    default:
      return false;
  }
  dispatch("on_swipe", 0, direction);
  return true;
}

void* LabAppActivity::resizeAllocation(void* ptr, const size_t bytes) {
  if (!bytes) {
    heap_caps_free(ptr);
    return nullptr;
  }
  return heap_caps_realloc(ptr, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void LabAppActivity::title(const char* text) {
  if (!std::strcmp(heading_, text)) return;
  std::snprintf(heading_, sizeof(heading_), "%s", text);
  dirty_ = true;
}

void LabAppActivity::clear() {
  closeRouting();
  if (canvas_) canvas_->clear();
  canvasMode_ = false;
  count_ = 0;
  columns_ = 0;
  caption_[0] = 0;
  dirty_ = true;
}

void LabAppActivity::caption(const char* text) {
  std::snprintf(caption_, sizeof(caption_), "%s", text);
  dirty_ = true;
}

bool LabAppActivity::row(const char* label, const char* value) {
  if (count_ == crossstore::MAX_ROWS) return false;
  std::snprintf(labels_[count_], sizeof(labels_[0]), "%s", label);
  std::snprintf(values_[count_], sizeof(values_[0]), "%s", value);
  rows_[count_] = {};
  rows_[count_].label = labels_[count_];
  rows_[count_].subtitle = values_[count_];
  rows_[count_].actionValue = static_cast<int16_t>(count_);
  ++count_;
  dirty_ = true;
  return true;
}

bool LabAppActivity::chinese() const { return I18N.getLanguage() == Language::ZH_CN; }
uint32_t LabAppActivity::milliseconds() const { return millis(); }

bool LabAppActivity::read(const char* key, char* out, const size_t capacity) {
  return applab::readData(id_, key, out, capacity);
}

bool LabAppActivity::write(const char* key, const char* value, const size_t size) {
  return applab::writeData(id_, key, value, size);
}

void LabAppActivity::input(const char* title, const char* initial) {
  std::snprintf(inputTitle_, sizeof(inputTitle_), "%s", title);
  std::snprintf(inputText_, sizeof(inputText_), "%s", initial);
  inputPending_ = true;
}

void LabAppActivity::size(int& width, int& height) const {
  const auto bounds = canvasBounds();
  width = bounds.width;
  height = bounds.height;
}

bool LabAppActivity::canvas() {
  if (!canvas_) {
    canvas_ = makeUniqueNoThrow<crossstore::Canvas>();
    if (!canvas_) {
      LOG_ERR("APPLAB", "OOM: canvas (%u bytes)", static_cast<unsigned>(sizeof(crossstore::Canvas)));
      return false;
    }
  }
  canvasMode_ = true;
  return true;
}

bool LabAppActivity::draw(const crossstore::DrawCommand& command, const char* text) {
  if (!canvasMode_ || !canvas_) return false;
  const auto bounds = canvasBounds();
  if ((command.kind == crossstore::DrawKind::Button || command.kind == crossstore::DrawKind::Hit) &&
      (command.x < 0 || command.y < 0 || command.x + command.width > bounds.width ||
       command.y + command.height > bounds.height)) {
    return false;
  }
  if (command.kind == crossstore::DrawKind::Qr) return false;
  dirty_ = true;
  return canvas_->add(command, text);
}

void LabAppActivity::measure(const char* text, const int maximum, const unsigned font, int& width, int& height) {
  static constexpr int kFonts[] = {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID, CONTROL_18_FONT_ID};
  const int fontId = kFonts[font < 4 ? font : 1];
  const EpdFontFamily::Style style = font >= 2 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int full = renderer.getTextWidth(fontId, text, style);
  const int lines = maximum > 0 ? std::max(1, (full + maximum - 1) / maximum) : 1;
  width = std::min(full, maximum);
  height = lines * renderer.getLineHeight(fontId);
}

bool LabAppActivity::system(const char* command, const char*, int64_t, crossstore::ServiceReply& reply) {
  return applab::systemCall(command, reply);
}

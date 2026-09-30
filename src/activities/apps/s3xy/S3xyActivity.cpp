#include "S3xyActivity.h"

#include <ArduinoJson.h>
#include <I18n.h>
#include <Logging.h>
#include <PersistableStore.h>

#include <algorithm>

#include "BleInput.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr char kLabelsPath[] = "/.crosspoint/s3xy.json";
constexpr size_t kMaxLabelBytes = 40;
constexpr int kMinGap = 10;  // AGENTS rule 14: >= 6 px between controls
constexpr s3xy::Press kPresses[] = {s3xy::Press::Single, s3xy::Press::Double, s3xy::Press::Long};

bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

StrId pressName(const int tile) {
  switch (tile) {
    case 0:
      return StrId::STR_S3XY_SINGLE;
    case 1:
      return StrId::STR_S3XY_DOUBLE;
    default:
      return StrId::STR_S3XY_LONG;
  }
}

}  // namespace

void S3xyActivity::onEnter() {
  Activity::onEnter();
  loadLabels();
  // The link owns NimBLE while this app is open; the keyboard host gets it
  // back afterwards (this activity does not keep Bluetooth alive for it).
  {
    RenderLock lock;
    bleinput::stop();
  }
  started_ = s3xy::begin();
  lastState_ = s3xy::state();
  requestUpdate();
}

void S3xyActivity::onExit() {
  // Runs under ActivityManager's render lock: no RenderLock here.
  s3xy::end();
  started_ = false;
  Activity::onExit();
}

bool S3xyActivity::preventAutoSleep() {
  const auto state = s3xy::state();
  return state == s3xy::LinkState::Connected || state == s3xy::LinkState::Ready;
}

void S3xyActivity::loadLabels() {
  for (int i = 0; i < kTileCount; ++i) labels_[i].clear();
  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(kLabelsPath, doc)) return;
  JsonArrayConst labels = doc["labels"].as<JsonArrayConst>();
  int i = 0;
  for (JsonVariantConst label : labels) {
    if (i >= kTileCount) break;
    labels_[i++] = label | "";
  }
}

void S3xyActivity::saveLabels() const {
  JsonDocument doc;
  JsonArray labels = doc["labels"].to<JsonArray>();
  for (const auto& label : labels_) labels.add(label);
  if (!PersistableStoreBase::writeDocToFile(kLabelsPath, doc)) LOG_ERR("S3XY", "Failed to save labels");
}

S3xyActivity::Layout S3xyActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int left = safe.x + metrics.contentSidePadding;
  const int width = std::max(0, safe.width - 2 * metrics.contentSidePadding);
  const int top = safe.y + metrics.topPadding;
  const int bottom = safe.y + safe.height - metrics.verticalSpacing;

  Layout l;
  l.header = Rect{left, top, width, metrics.headerHeight};
  const int statusH = renderer.getLineHeight(UI_12_FONT_ID) * 2 + 8;
  l.status = Rect{left, l.header.y + l.header.height + metrics.verticalSpacing / 2, width, statusH};
  const int gap = std::max(kMinGap, metrics.verticalSpacing);
  const int editH = std::max(44, renderer.getLineHeight(UI_10_FONT_ID) * 2 + 8);
  l.edit = Rect{left, bottom - editH, width, editH};
  const int tilesTop = l.status.y + l.status.height + gap;
  const int tileH = std::max(1, (l.edit.y - gap - tilesTop - gap * (kTileCount - 1)) / kTileCount);
  for (int i = 0; i < kTileCount; ++i) l.tiles[i] = Rect{left, tilesTop + i * (tileH + gap), width, tileH};
  return l;
}

void S3xyActivity::press(const int tile) {
  if (editing_) {
    editLabel(tile);
    return;
  }
  const bool sent = s3xy::send(kPresses[tile]);
  RenderLock lock(*this);
  flashTile_ = tile;
  flashUntilMs_ = millis() + kSentFlashMs;
  sendFailed_ = !sent;
  lock.unlock();
  requestUpdate();
}

void S3xyActivity::editLabel(const int tile) {
  startActivityForResultWith<KeyboardEntryActivity>(
      [this, tile](const ActivityResult& result) {
        if (result.isCancelled) return;
        {
          RenderLock lock(*this);
          labels_[tile] = std::get<KeyboardResult>(result.data).text;
        }
        saveLabels();
      },
      I18N.get(pressName(tile)), labels_[tile], kMaxLabelBytes, InputType::Text);
}

void S3xyActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    if (editing_) {
      editing_ = false;
      requestUpdate();
      return;
    }
    activityManager.goToApps();
    return;
  }

  const auto state = s3xy::state();
  if (state != lastState_) {
    lastState_ = state;
    requestUpdate();
  }
  if (flashTile_ >= 0 && static_cast<int32_t>(millis() - flashUntilMs_) >= 0) {
    RenderLock lock(*this);
    flashTile_ = -1;
    lock.unlock();
    requestUpdate();
  }

  if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::PageBack)) {
    press(0);
    return;
  }
  if (mappedInput.wasReleased(Button::Down) || mappedInput.wasReleased(Button::PageForward)) {
    press(1);
    return;
  }

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  const Layout l = layout();
  if (contains(l.edit, x, y)) {
    editing_ = !editing_;
    requestUpdate();
    return;
  }
  for (int i = 0; i < kTileCount; ++i) {
    if (contains(l.tiles[i], x, y)) {
      press(i);
      return;
    }
  }
}

void S3xyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Layout l = layout();
  GUI.drawHeader(renderer, l.header, tr(STR_S3XY_TITLE));

  // Status: link state, then a hint for the next step.
  const char* status = tr(STR_S3XY_UNAVAILABLE);
  const char* hint = "";
  if (started_) {
    switch (s3xy::state()) {
      case s3xy::LinkState::Advertising:
        status = tr(STR_S3XY_WAITING);
        hint = tr(STR_S3XY_PAIR_HINT);
        break;
      case s3xy::LinkState::Connected:
        status = tr(STR_S3XY_CONNECTING);
        break;
      case s3xy::LinkState::Ready:
        status = tr(STR_S3XY_READY);
        hint = editing_ ? tr(STR_S3XY_EDIT_HINT) : "";
        break;
      case s3xy::LinkState::Off:
        break;
    }
  }
  if (flashTile_ >= 0 && sendFailed_) hint = tr(STR_S3XY_NOT_CONNECTED);
  renderer.drawText(UI_12_FONT_ID, l.status.x, l.status.y, status, true, EpdFontFamily::BOLD);
  const auto hintLines = renderer.wrappedText(UI_10_FONT_ID, hint, l.status.width, 1);
  if (!hintLines.empty()) {
    renderer.drawText(UI_10_FONT_ID, l.status.x, l.status.y + renderer.getLineHeight(UI_12_FONT_ID) + 4,
                      hintLines[0].c_str());
  }

  for (int i = 0; i < kTileCount; ++i) {
    const Rect& tile = l.tiles[i];
    const bool active = flashTile_ == i && !sendFailed_;
    bool black = true;
    const int radius = std::min(metrics.optionPopupSelectionRadius, tile.height / 2);
    if (active) {
      black = GUI.drawSelectionBackground(renderer, tile);
    } else {
      renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, editing_ ? 3 : 2, radius, true);
    }
    const char* caption = I18N.get(pressName(i));
    const char* label = labels_[i].empty() ? caption : labels_[i].c_str();
    const std::string main = renderer.truncatedText(NOTOSANS_18_FONT_ID, label, tile.width - 24, EpdFontFamily::BOLD);
    const int mainH = renderer.getLineHeight(NOTOSANS_18_FONT_ID);
    const int capH = renderer.getLineHeight(UI_10_FONT_ID);
    const bool showCaption = !labels_[i].empty();
    const int blockH = mainH + (showCaption ? capH + 4 : 0);
    int y = tile.y + (tile.height - blockH) / 2;
    const int mainW = renderer.getTextWidth(NOTOSANS_18_FONT_ID, main.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_18_FONT_ID, tile.x + (tile.width - mainW) / 2, y, main.c_str(), black,
                      EpdFontFamily::BOLD);
    if (showCaption) {
      y += mainH + 4;
      const int capW = renderer.getTextWidth(UI_10_FONT_ID, caption);
      renderer.drawText(UI_10_FONT_ID, tile.x + (tile.width - capW) / 2, y, caption, black);
    }
  }

  GUI.drawActionButton(renderer, l.edit, editing_ ? tr(STR_S3XY_DONE) : tr(STR_S3XY_EDIT), editing_);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

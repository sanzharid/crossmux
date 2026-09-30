#include "LockScreenActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "ScreenLock.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Keypad slots, row-major: digits, then [action][0][backspace].
constexpr int kActionKey = 9;
constexpr int kZeroKey = 10;
constexpr int kBackspaceKey = 11;
constexpr int kMinKeyGap = 10;  // AGENTS rule 14: >= 6 px between controls

// Same backspace glyph as the calculator: fonts may lack U+232B.
void drawBackspaceIcon(const GfxRenderer& renderer, const Rect key) {
  const int cx = key.x + key.width / 2;
  const int cy = key.y + key.height / 2;
  const int left = cx - 13;
  const int right = cx + 13;
  const int top = cy - 9;
  const int bottom = cy + 9;
  renderer.drawLine(left, cy, left + 8, top, 2, true);
  renderer.drawLine(left + 8, top, right, top, 2, true);
  renderer.drawLine(right, top, right, bottom, 2, true);
  renderer.drawLine(right, bottom, left + 8, bottom, 2, true);
  renderer.drawLine(left + 8, bottom, left, cy, 2, true);
  renderer.drawLine(cx + 1, cy - 5, cx + 9, cy + 5, 2, true);
  renderer.drawLine(cx + 9, cy - 5, cx + 1, cy + 5, 2, true);
}

bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

}  // namespace

void LockScreenActivity::onEnter() {
  Activity::onEnter();
  pinRequired_ = mode_ != Mode::Unlock || ScreenLock::hasPin();
  if (mode_ == Mode::Unlock) ScreenLock::setLocked(true);
  entry_.clear();
  message_ = StrId::STR_NONE_OPT;
  requestUpdate();
}

LockScreenActivity::Layout LockScreenActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int left = safe.x + metrics.contentSidePadding;
  const int width = std::max(0, safe.width - 2 * metrics.contentSidePadding);
  const int top = safe.y + metrics.topPadding;
  const int bottom = safe.y + safe.height - metrics.verticalSpacing;

  Layout l;
  l.title = Rect{left, top, width, metrics.headerHeight};
  const int dotsHeight = renderer.getLineHeight(NOTOSANS_18_FONT_ID) + 8;
  l.dots = Rect{left, l.title.y + l.title.height + metrics.verticalSpacing, width, dotsHeight};
  const int messageHeight = renderer.getLineHeight(UI_12_FONT_ID) + 4;
  l.message = Rect{left, l.dots.y + l.dots.height, width, messageHeight};

  const int gridTop = l.message.y + l.message.height + metrics.verticalSpacing;
  const int gap = std::max(kMinKeyGap, metrics.verticalSpacing);
  // Keep keys roughly square so the pad stays thumb-sized in landscape too.
  const int maxKeyH = (bottom - gridTop - gap * (kRows - 1)) / kRows;
  const int maxKeyW = (width - gap * (kColumns - 1)) / kColumns;
  const int keyH = std::max(1, std::min(maxKeyH, maxKeyW));
  const int keyW = std::max(1, std::min(maxKeyW, keyH * 3 / 2));
  const int gridW = keyW * kColumns + gap * (kColumns - 1);
  const int gridX = left + (width - gridW) / 2;
  for (int i = 0; i < kKeyCount; ++i) {
    l.keys[i] = Rect{gridX + (i % kColumns) * (keyW + gap), gridTop + (i / kColumns) * (keyH + gap), keyW, keyH};
  }
  const int unlockH = std::max(56, renderer.getLineHeight(NOTOSANS_18_FONT_ID) * 2);
  l.unlock = Rect{gridX, gridTop + (bottom - gridTop - unlockH) / 2, gridW, unlockH};
  return l;
}

const char* LockScreenActivity::keyLabel(const int key) const {
  static const char* const kDigits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
  if (key < kActionKey) return kDigits[key];
  if (key == kZeroKey) return "0";
  if (key == kBackspaceKey) return "<";  // drawn as an icon in render()
  return offerRemove_ && mode_ == Mode::Enter ? tr(STR_LOCK_REMOVE_PIN) : "";
}

void LockScreenActivity::press(const int key) {
  if (key == kBackspaceKey) {
    if (!entry_.empty()) entry_.pop_back();
  } else if (key == kActionKey) {
    if (offerRemove_ && mode_ == Mode::Enter) {
      done(std::string());  // "remove PIN"
      return;
    }
  } else if (entry_.size() < ScreenLock::kPinLength) {
    entry_ += key == kZeroKey ? '0' : static_cast<char>('1' + key);
    message_ = StrId::STR_NONE_OPT;
  }
  if (entry_.size() == ScreenLock::kPinLength) submit();
  requestUpdate();
}

void LockScreenActivity::submit() {
  if (mode_ == Mode::Enter) {
    done(entry_);
    return;
  }
  if (ScreenLock::lockoutRemainingMs() > 0) {
    entry_.clear();
    return;
  }
  if (ScreenLock::verify(entry_)) {
    done(entry_);
    return;
  }
  LOG_INF("LOCK", "Wrong PIN (%u failures)", ScreenLock::failedAttempts());
  entry_.clear();
  message_ = StrId::STR_LOCK_WRONG_PIN;
}

void LockScreenActivity::done(std::string pin) {
  if (mode_ == Mode::Unlock) {
    ScreenLock::setLocked(false);
  } else {
    setResult(KeyboardResult{std::move(pin)});
  }
  finish();
}

void LockScreenActivity::loop() {
  // A chat underneath keeps BLE text mode on; drop anything typed while locked
  // so it cannot be replayed (and sent) after unlock.
  MappedInputManager::BleTextKey discarded;
  while (mappedInput.popBleTextKey(discarded)) {
  }

  // Settings flows can be abandoned with Back; the lock itself cannot.
  if (mode_ != Mode::Unlock && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // Count down a running lockout once a second.
  const uint32_t lockoutS = (ScreenLock::lockoutRemainingMs() + 999) / 1000;
  if (lockoutS != lastCountdownS_) {
    lastCountdownS_ = lockoutS;
    requestUpdate();
  }

  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return;
  const Layout l = layout();
  if (!pinRequired_) {
    if (contains(l.unlock, x, y)) done(std::string());
    return;
  }
  if (lockoutS > 0) return;
  for (int i = 0; i < kKeyCount; ++i) {
    if (contains(l.keys[i], x, y)) {
      press(i);
      return;
    }
  }
}

void LockScreenActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Layout l = layout();
  const auto centered = [&](const int fontId, const Rect& r, const char* text, const EpdFontFamily::Style style) {
    const int w = renderer.getTextWidth(fontId, text, style);
    renderer.drawText(fontId, r.x + (r.width - w) / 2, r.y + (r.height - renderer.getLineHeight(fontId)) / 2, text,
                      true, style);
  };

  const char* title = mode_ == Mode::Unlock ? tr(STR_LOCK_LOCKED) : I18N.get(prompt_);
  centered(NOTOSANS_18_FONT_ID, l.title, title, EpdFontFamily::BOLD);

  if (!pinRequired_) {
    GUI.drawActionButton(renderer, l.unlock, tr(STR_LOCK_UNLOCK), true);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // PIN progress: filled dots for entered digits, rings for the rest.
  const int dot = std::max(12, renderer.getLineHeight(NOTOSANS_18_FONT_ID) / 2);
  const int spacing = dot * 2;
  const int rowW = static_cast<int>(ScreenLock::kPinLength) * dot + (ScreenLock::kPinLength - 1) * spacing;
  int dx = l.dots.x + (l.dots.width - rowW) / 2;
  const int dy = l.dots.y + (l.dots.height - dot) / 2;
  for (size_t i = 0; i < ScreenLock::kPinLength; ++i, dx += dot + spacing) {
    if (i < entry_.size()) {
      renderer.fillRoundedRect(dx, dy, dot, dot, dot / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(dx, dy, dot, dot, 2, dot / 2, true);
    }
  }

  char buffer[64];
  const uint32_t lockoutS = (ScreenLock::lockoutRemainingMs() + 999) / 1000;
  if (lockoutS > 0) {
    snprintf(buffer, sizeof(buffer), tr(STR_LOCK_TRY_AGAIN_IN), static_cast<unsigned>(lockoutS));
    centered(UI_12_FONT_ID, l.message, buffer, EpdFontFamily::REGULAR);
  } else if (message_ != StrId::STR_NONE_OPT) {
    centered(UI_12_FONT_ID, l.message, I18N.get(message_), EpdFontFamily::BOLD);
  }

  const EpdFontFamily::Style keyStyle =
      metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  for (int i = 0; i < kKeyCount; ++i) {
    const char* label = keyLabel(i);
    if (!*label) continue;
    const Rect& key = l.keys[i];
    const int radius = std::min(metrics.optionPopupSelectionRadius, std::min(key.width, key.height) / 2);
    renderer.drawRoundedRect(key.x, key.y, key.width, key.height, 1, radius, true);
    if (i == kBackspaceKey) {
      drawBackspaceIcon(renderer, key);
      continue;
    }
    const int font = i == kActionKey ? UI_10_FONT_ID : NOTOSANS_18_FONT_ID;
    centered(font, key, label, keyStyle);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#pragma once

#include <cstdint>

namespace appVisibility {

// Persisted bit positions: never reorder or reuse IDs, including uncompiled apps.
enum class AppId : uint8_t {
  ReadingStats = 0,
  Retired1 = 1,  // retired app (WeRead); never reuse
  Sudoku = 2,
  Gomoku = 3,
  Retired4 = 4,  // retired app (ChineseChess); never reuse
  Minesweeper = 5,
  Game2048 = 6,
  Retired7 = 7,  // retired app (UglyAvatar); never reuse
  Standby = 8,
  AirPage = 9,
  Retired10 = 10,  // retired app (Buddy); never reuse
  Retired11 = 11,  // retired app (Sokoban); never reuse
  Retired12 = 12,  // retired app (PixelSwitch); never reuse
  FileTransfer = 13,
  OpdsBrowser = 14,
  Calculator = 15,
  Retired16 = 16,  // retired app (Woodfish); never reuse
  Hermes = 17,
  S3xy = 18,
  SystemMonitor = 19,
  Count = 20,
};

constexpr uint32_t appBit(const AppId id) { return uint32_t{1} << static_cast<uint8_t>(id); }

constexpr uint32_t DEFAULT_HIDDEN_APPS_MASK = appBit(AppId::Minesweeper) | appBit(AppId::Game2048);

static_assert(static_cast<uint8_t>(AppId::Count) <= 32, "app IDs must fit the persisted mask");

}  // namespace appVisibility

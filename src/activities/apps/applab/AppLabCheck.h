#pragma once

#include <string>
#include <vector>

namespace applab {

// Runs a generated app headless in the same sandbox and canvas limits as
// LabAppActivity, exercising start, resize, ticks, every control and list row,
// swipes and the page keys. Returns the problems found (empty when it ran clean)
// in plain English for the model to fix. Reads and writes stay in memory.
std::vector<std::string> checkApp(const std::string& lua, int width, int height);

}  // namespace applab

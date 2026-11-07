// Runs the real Lua 5.4 runtime and App Lab's sandbox check on sample apps.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "AppLabCheck.h"
#include "AppLabFiles.h"

// The device implementation reads the RTC and battery; the check only needs a
// well-formed answer.
bool applab::systemCall(const char* command, crossstore::ServiceReply& reply) {
  if (std::string(command) != "device.battery") return false;
  reply.a = 80;
  return true;
}

namespace {

constexpr int kWidth = 432;
constexpr int kHeight = 600;

std::vector<std::string> check(const std::string& lua) { return applab::checkApp(lua, kWidth, kHeight); }

bool mentions(const std::vector<std::string>& problems, const std::string& text) {
  for (const auto& problem : problems) {
    if (problem.find(text) != std::string::npos) return true;
  }
  return false;
}

std::string joined(const std::vector<std::string>& problems) {
  std::string all;
  for (const auto& problem : problems) all += problem + "\n";
  return all;
}

}  // namespace

TEST(AppLabRuntime, CleanCanvasAppPasses) {
  const auto problems = check(R"LUA(
local count = 0
local function draw()
  app.clear(); app.title("Counter"); app.canvas()
  local w, h = app.size()
  app.text(0, 20, w, 80, tostring(count), 3, 1)
  app.button(1, 0, h - 120, w, 52, "Add", 3)
  app.button(2, 0, h - 60, w, 52, "Reset", 0)
end
return {
  on_start = function() count = tonumber(app.read("count") or "0") or 0; draw() end,
  on_resize = draw,
  on_select = function(action)
    if action == 1 then count = count + 1 elseif action == 2 then count = 0 end
    app.write("count", tostring(count)); draw()
  end,
  on_stop = function() app.write("count", tostring(count)) end,
}
)LUA");
  EXPECT_TRUE(problems.empty()) << joined(problems);
}

TEST(AppLabRuntime, CleanListAppPasses) {
  const auto problems = check(R"LUA(
return {
  on_start = function()
    app.clear(); app.title("List")
    for i = 1, 5 do app.row("Item " .. i, "value") end
  end,
  on_select = function(index) app.caption("Picked " .. index) end,
}
)LUA");
  EXPECT_TRUE(problems.empty()) << joined(problems);
}

TEST(AppLabRuntime, SyntaxErrorIsReported) {
  const auto problems = check("return { on_start = function( end }");
  ASSERT_FALSE(problems.empty());
  EXPECT_TRUE(mentions(problems, "during loading")) << joined(problems);
}

TEST(AppLabRuntime, RuntimeErrorIsReported) {
  const auto problems = check("return { on_start = function() local t = nil; t.x = 1 end }");
  ASSERT_FALSE(problems.empty());
  EXPECT_TRUE(mentions(problems, "during on_start")) << joined(problems);
}

TEST(AppLabRuntime, RemovedLibrariesAreUnavailable) {
  for (const char* call : {"os.time()", "io.write('x')", "pcall(print)", "string.find('a', 'a')", "load('x=1')",
                           "require('x')"}) {
    const auto problems =
        check(std::string("return { on_start = function() app.clear(); app.row('a', 'b'); ") + call + " end }");
    EXPECT_TRUE(mentions(problems, "during on_start")) << call << "\n" << joined(problems);
  }
}

TEST(AppLabRuntime, InfiniteLoopHitsTheInstructionBudget) {
  const auto problems = check("return { on_start = function() app.clear(); app.row('a', 'b'); while true do end end }");
  ASSERT_FALSE(problems.empty());
  EXPECT_TRUE(mentions(problems, "during on_start")) << joined(problems);
}

TEST(AppLabRuntime, OverlappingControlsAreReported) {
  const auto problems = check(R"LUA(
return { on_start = function()
  app.clear(); app.canvas()
  app.button(1, 0, 0, 200, 60, "A", 0)
  app.button(2, 100, 30, 200, 60, "B", 0)
end }
)LUA");
  EXPECT_TRUE(mentions(problems, "rejected")) << joined(problems);
}

TEST(AppLabRuntime, ControlOutsideCanvasIsReported) {
  const auto problems = check(R"LUA(
return { on_start = function()
  app.clear(); app.canvas()
  app.button(1, 0, 590, 200, 60, "Off the edge", 0)
end }
)LUA");
  EXPECT_TRUE(mentions(problems, "outside the 432x600 canvas")) << joined(problems);
}

TEST(AppLabRuntime, DrawingNothingIsReported) {
  const auto problems = check("return { on_start = function() app.clear() end }");
  EXPECT_TRUE(mentions(problems, "drew nothing")) << joined(problems);
}

TEST(AppLabRuntime, ErrorsInTappedControlsAreFound) {
  const auto problems = check(R"LUA(
return {
  on_start = function() app.clear(); app.canvas(); app.button(7, 0, 0, 200, 60, "Go", 0) end,
  on_select = function(action) if action == 7 then error("tap handler broke") end end,
}
)LUA");
  EXPECT_TRUE(mentions(problems, "tap handler broke")) << joined(problems);
}

TEST(AppLabRuntime, OversizedProgramIsRejected) {
  const auto problems = check("return {}" + std::string(40000, ' '));
  EXPECT_TRUE(mentions(problems, "over 32768 bytes")) << joined(problems);
}

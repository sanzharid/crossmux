#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "AppLabProtocol.h"

using applab::ParseResult;
using applab::RequestKind;

TEST(AppLabProtocol, EndpointUsesModel) {
  EXPECT_EQ(applab::endpointFor("gemini-3.5-flash"),
            "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent");
}

TEST(AppLabProtocol, ModelIdsAreUrlSafeTokens) {
  EXPECT_TRUE(applab::isValidModel("gemini-3.5-flash"));
  EXPECT_FALSE(applab::isValidModel(""));
  EXPECT_FALSE(applab::isValidModel("gemini/../../x"));
  EXPECT_FALSE(applab::isValidModel("Gemini Flash"));
  EXPECT_FALSE(applab::isValidModel("models?key=1"));
  EXPECT_FALSE(applab::isValidModel(std::string(65, 'a')));
}

TEST(AppLabProtocol, RequestCarriesSystemUserAndSchema) {
  const std::string body = applab::buildRequestBody("SYSTEM \"rules\"", "Make a timer", 8192);
  EXPECT_NE(body.find("\"systemInstruction\":{\"parts\":[{\"text\":\"SYSTEM \\\"rules\\\"\"}]}"), std::string::npos);
  EXPECT_NE(body.find("\"text\":\"Make a timer\""), std::string::npos);
  EXPECT_NE(body.find("\"responseMimeType\":\"application/json\""), std::string::npos);
  EXPECT_NE(body.find("\"main_lua\":{\"type\":\"STRING\"}"), std::string::npos);
  EXPECT_NE(body.find("\"maxOutputTokens\":8192"), std::string::npos);
}

TEST(AppLabProtocol, UserMessagesIncludeCodeAndProblems) {
  const std::string create = applab::userMessage(RequestKind::Create, "a timer", "", {});
  EXPECT_NE(create.find("a timer"), std::string::npos);
  const std::string change = applab::userMessage(RequestKind::Change, "bigger digits", "return {}", {});
  EXPECT_NE(change.find("return {}"), std::string::npos);
  EXPECT_NE(change.find("bigger digits"), std::string::npos);
  const std::string fix = applab::userMessage(RequestKind::Fix, "a timer", "return {}", {"line 3: nil", "overlap"});
  EXPECT_NE(fix.find("- line 3: nil\n- overlap\n"), std::string::npos);
  EXPECT_NE(fix.find("a timer"), std::string::npos);
}

TEST(AppLabProtocol, ExtractsTextAndSkipsThoughts) {
  const std::string reply =
      R"({"candidates":[{"content":{"parts":[{"text":"thinking...","thought":true},{"text":"{\"name\":"},)"
      R"({"text":"\"Timer\",\"main_lua\":\"return {}\"}"}]},"finishReason":"STOP"}]})";
  std::string text, message;
  ASSERT_EQ(applab::extractText(reply, text, message), ParseResult::Ok);
  EXPECT_EQ(text, R"({"name":"Timer","main_lua":"return {}"})");
  std::string name, lua;
  ASSERT_EQ(applab::parseApp(text, name, lua), ParseResult::Ok);
  EXPECT_EQ(name, "Timer");
  EXPECT_EQ(lua, "return {}");
}

TEST(AppLabProtocol, ReportsApiErrorsAndBlocks) {
  std::string text, message;
  EXPECT_EQ(applab::extractText(R"({"error":{"code":400,"message":"API key not valid."}})", text, message),
            ParseResult::ApiError);
  EXPECT_EQ(message, "API key not valid.");
  EXPECT_EQ(applab::extractText(R"({"promptFeedback":{"blockReason":"SAFETY"}})", text, message), ParseResult::Blocked);
  EXPECT_EQ(applab::extractText(R"({"candidates":[{"content":{"parts":[{"text":"x"}]},"finishReason":"MAX_TOKENS"}]})",
                                text, message),
            ParseResult::TooLong);
  EXPECT_EQ(applab::extractText("not json", text, message), ParseResult::BadJson);
  EXPECT_EQ(applab::extractText(R"({"candidates":[{"content":{"parts":[]}}]})", text, message), ParseResult::NoApp);
}

TEST(AppLabProtocol, FallsBackToFencedLua) {
  std::string name, lua;
  ASSERT_EQ(applab::parseApp("Here you go:\n```lua\nreturn { on_start = function() end }\n```\n", name, lua),
            ParseResult::Ok);
  EXPECT_EQ(lua, "return { on_start = function() end }");
  EXPECT_TRUE(name.empty());
  EXPECT_EQ(applab::parseApp("no code here", name, lua), ParseResult::NoApp);
  EXPECT_EQ(applab::parseApp(R"({"name":"x","main_lua":""})", name, lua), ParseResult::NoApp);
}

TEST(AppLabProtocol, IdsAreSafeFolderNames) {
  EXPECT_EQ(applab::idFromName("Pomodoro Timer!"), "pomodoro_timer");
  EXPECT_EQ(applab::idFromName("  2048 game "), "app_2048_game");
  EXPECT_EQ(applab::idFromName("../../etc"), "etc");
  EXPECT_EQ(applab::idFromName(""), "app");
  EXPECT_EQ(applab::idFromName("计时器"), "app");
  EXPECT_LE(applab::idFromName(std::string(80, 'a')).size(), 31u);
  EXPECT_TRUE(applab::isValidId(applab::idFromName("Weird / Name ..")));
  EXPECT_FALSE(applab::isValidId("Bad"));
  EXPECT_FALSE(applab::isValidId("1abc"));
  EXPECT_FALSE(applab::isValidId("a/b"));
  EXPECT_FALSE(applab::isValidId(""));
}

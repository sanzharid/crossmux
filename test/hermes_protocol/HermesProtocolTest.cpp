#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "HermesProtocol.h"

using hermes::ApiStyle;
using hermes::ParseResult;

TEST(HermesProtocol, JoinUrlAppendsPath) {
  EXPECT_EQ(hermes::joinUrl("http://hermes.local:8642", "/v1/responses"), "http://hermes.local:8642/v1/responses");
  EXPECT_EQ(hermes::joinUrl("http://h:8642/", "/v1/responses"), "http://h:8642/v1/responses");
  EXPECT_EQ(hermes::joinUrl("http://h:8642/v1", "/v1/responses"), "http://h:8642/v1/responses");
  EXPECT_EQ(hermes::joinUrl("http://h/p/work/", "/v1/responses"), "http://h/p/work/v1/responses");
}

TEST(HermesProtocol, JoinUrlKeepsFullEndpoint) {
  EXPECT_EQ(hermes::joinUrl("http://h:8642/v1/chat/completions", "/v1/responses"), "http://h:8642/v1/chat/completions");
}

TEST(HermesProtocol, ResponsesBodyCarriesConversation) {
  const std::string body = hermes::buildResponsesBody("hermes-agent", "sticky", "Hi \"there\"");
  EXPECT_NE(body.find("\"model\":\"hermes-agent\""), std::string::npos);
  EXPECT_NE(body.find("\"conversation\":\"sticky\""), std::string::npos);
  EXPECT_NE(body.find("\"input\":\"Hi \\\"there\\\"\""), std::string::npos);
}

TEST(HermesProtocol, ChatCompletionsBodyHasSingleUserMessage) {
  const std::string body = hermes::buildChatCompletionsBody("hermes-agent", "hello");
  EXPECT_NE(body.find("\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]"), std::string::npos);
}

TEST(HermesProtocol, ParsesResponsesOutputSkippingToolCalls) {
  const std::string body = R"({
    "id": "resp_1", "object": "response", "status": "completed",
    "output": [
      {"type": "function_call", "name": "terminal", "arguments": "{\"command\":\"ls\"}", "call_id": "c1"},
      {"type": "function_call_output", "call_id": "c1", "output": "README.md src/"},
      {"type": "message", "role": "assistant",
       "content": [{"type": "output_text", "text": "Your project has a README."}]}
    ]})";
  std::string text;
  std::string error;
  EXPECT_EQ(hermes::parseReply(body, ApiStyle::Responses, text, error), ParseResult::Ok);
  EXPECT_EQ(text, "Your project has a README.");
  EXPECT_TRUE(error.empty());
}

TEST(HermesProtocol, PrefersFlatOutputText) {
  std::string text;
  std::string error;
  EXPECT_EQ(hermes::parseReply(R"({"output_text":"flat","output":[]})", ApiStyle::Responses, text, error),
            ParseResult::Ok);
  EXPECT_EQ(text, "flat");
}

TEST(HermesProtocol, ParsesChatCompletions) {
  std::string text;
  std::string error;
  EXPECT_EQ(hermes::parseReply(R"({"choices":[{"message":{"role":"assistant","content":"Hi!"}}]})",
                               ApiStyle::ChatCompletions, text, error),
            ParseResult::Ok);
  EXPECT_EQ(text, "Hi!");
}

TEST(HermesProtocol, ReportsServerErrors) {
  std::string text;
  std::string error;
  EXPECT_EQ(hermes::parseReply(R"({"error":{"message":"Invalid API key","type":"auth"}})", ApiStyle::Responses, text,
                               error),
            ParseResult::NoText);
  EXPECT_EQ(error, "Invalid API key");
  EXPECT_EQ(hermes::parseReply("<html>502</html>", ApiStyle::Responses, text, error), ParseResult::BadJson);
}

TEST(HermesProtocol, ParsesTranscripts) {
  std::string text;
  std::string error;
  EXPECT_EQ(hermes::parseTranscript(R"({"text":"  Turn on the lights. "})", text, error), ParseResult::Ok);
  EXPECT_EQ(text, "Turn on the lights.");
  EXPECT_EQ(hermes::parseTranscript("plain words\n", text, error), ParseResult::Ok);
  EXPECT_EQ(text, "plain words");
  EXPECT_EQ(hermes::parseTranscript(R"({"text":"   "})", text, error), ParseResult::NoText);
  EXPECT_EQ(hermes::parseTranscript("", text, error), ParseResult::NoText);
}

TEST(HermesProtocol, WavHeaderIsCanonical) {
  uint8_t header[hermes::kWavHeaderBytes] = {};
  hermes::writeWavHeader(header, 32000, 16000);
  EXPECT_EQ(std::memcmp(header, "RIFF", 4), 0);
  EXPECT_EQ(header[4] | header[5] << 8 | header[6] << 16, 36 + 32000);
  EXPECT_EQ(std::memcmp(header + 8, "WAVEfmt ", 8), 0);
  EXPECT_EQ(header[20], 1);   // PCM
  EXPECT_EQ(header[22], 1);   // mono
  EXPECT_EQ(header[24] | header[25] << 8, 16000);
  EXPECT_EQ(header[28] | header[29] << 8, 32000 & 0xFFFF);  // byte rate low half
  EXPECT_EQ(header[34], 16);  // bits per sample
  EXPECT_EQ(std::memcmp(header + 36, "data", 4), 0);
  EXPECT_EQ(header[40] | header[41] << 8, 32000 & 0xFFFF);
}

TEST(HermesProtocol, MultipartFramesFilePart) {
  const std::string head = hermes::multipartHead("B", "whisper-1", "en");
  const std::string tail = hermes::multipartTail("B");
  EXPECT_EQ(head.rfind("--B\r\n", 0), 0u);
  EXPECT_NE(head.find("name=\"model\"\r\n\r\nwhisper-1\r\n"), std::string::npos);
  EXPECT_NE(head.find("name=\"language\"\r\n\r\nen\r\n"), std::string::npos);
  EXPECT_NE(head.find("name=\"file\"; filename=\"sticky.wav\""), std::string::npos);
  EXPECT_EQ(head.substr(head.size() - 4), "\r\n\r\n");
  EXPECT_EQ(tail, "\r\n--B--\r\n");
  EXPECT_EQ(hermes::multipartHead("B", "", "").find("language"), std::string::npos);
}

TEST(HermesProtocol, PlainTextFlattensMarkdown) {
  const std::string md =
      "# Plan\n\n**Bold** and `code` with [a link](http://x).\n\n- one\n* two\n\n```\nls -la\n```\n\n---\n> quoted\n\n\n\nend";
  EXPECT_EQ(hermes::plainText(md),
            "Plan\n\nBold and code with a link.\n\n\xE2\x80\xA2 one\n\xE2\x80\xA2 two\n\nls -la\n\nquoted\n\nend");
}

TEST(HermesProtocol, PlainTextKeepsSnakeCaseAndUtf8) {
  EXPECT_EQ(hermes::plainText("use my_var — café"), "use my_var — café");
}

#pragma once

// Hardware-free request/response helpers for the Hermes chat app, kept apart
// from the network client so they can be unit-tested on the host.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hermes {

enum class ApiStyle : uint8_t { Responses, ChatCompletions };

// Joins a configured base URL with an API path. A base that already names an
// endpoint (contains "/v1/") is used as-is so users may paste a full URL.
std::string joinUrl(const std::string& base, const char* path);

// JSON bodies for POST /v1/responses (server-side conversation chaining) and
// POST /v1/chat/completions (continuity via the X-Hermes-Session-Id header).
std::string buildResponsesBody(const std::string& model, const std::string& conversation, const std::string& text);
std::string buildChatCompletionsBody(const std::string& model, const std::string& text);

enum class ParseResult : uint8_t { Ok, BadJson, NoText };

// Extracts the assistant text from a Hermes reply. `serverError` receives the
// server's own error message when the body carries one (untranslated detail).
ParseResult parseReply(const std::string& body, ApiStyle style, std::string& text, std::string& serverError);

// Extracts {"text": "..."} from an OpenAI-compatible transcription reply; a
// non-JSON body is accepted as plain text (response_format=text servers).
ParseResult parseTranscript(const std::string& body, std::string& text, std::string& serverError);

// Canonical 44-byte PCM WAV header for 16-bit mono audio.
constexpr size_t kWavHeaderBytes = 44;
void writeWavHeader(uint8_t* out, uint32_t pcmBytes, uint32_t sampleRate);

// multipart/form-data framing around a single "file" part holding a WAV.
std::string multipartHead(const char* boundary, const std::string& model, const std::string& language);
std::string multipartTail(const char* boundary);

// http(s) links in a reply whose path ends in a book extension the reader
// opens (.epub .txt .md .xtc .xtch), in order, de-duplicated, at most maxLinks.
// Run on the raw reply: plainText() drops Markdown link targets.
std::vector<std::string> findBookLinks(const std::string& text, size_t maxLinks = 3);

// SD-safe file name for a book URL: last path segment, percent-decoded, with
// unsafe characters replaced and length capped; keeps the extension.
std::string bookFileName(const std::string& url);

// Flattens common Markdown (headings, emphasis, code fences, bullets, links)
// into plain text that reads well on e-paper.
std::string plainText(const std::string& markdown);

}  // namespace hermes

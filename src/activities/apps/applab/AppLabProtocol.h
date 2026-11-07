#pragma once

#include <string>
#include <vector>

// Gemini request/response handling for App Lab. Free of Arduino and device
// types so host tests cover it (test/applab_protocol).
namespace applab {

// Gemini's generateContent endpoint for a model id such as "gemini-3.5-flash".
std::string endpointFor(const std::string& model);

// A model id is a short, URL-safe token: lowercase letters, digits, '.', '-'.
bool isValidModel(const std::string& model);

// What to ask Gemini. `code` is the current app for Change/Fix requests;
// `problems` are the sandbox findings for a Fix request.
enum class RequestKind { Create, Change, Fix };
std::string userMessage(RequestKind kind, const std::string& request, const std::string& code,
                        const std::vector<std::string>& problems);

// Full generateContent body: system instruction, one user turn, and a JSON
// response schema {name, main_lua} so the reply needs no free-text parsing.
std::string buildRequestBody(const char* systemInstruction, const std::string& userText, int maxOutputTokens);

enum class ParseResult { Ok, ApiError, Blocked, TooLong, NoApp, BadJson };

// Pulls the model's text out of a generateContent reply, skipping thought
// parts. On ApiError/Blocked, `message` holds Google's explanation.
ParseResult extractText(const std::string& body, std::string& text, std::string& message);

// Reads {name, main_lua} from the model text. Falls back to a ```lua fenced
// block when the model ignored the schema; then the name is left empty.
ParseResult parseApp(const std::string& text, std::string& name, std::string& lua);

// App ids are folder names: lowercase letters, digits and '_', starting with a
// letter, at most 31 characters. Built from the app's name; "app" when empty.
bool isValidId(const std::string& id);
std::string idFromName(const std::string& name);

}  // namespace applab

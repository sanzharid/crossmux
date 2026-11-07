#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <utility>

// Validation for the phone setup page (/hermes on the file-transfer web server).
// Header-only and free of Arduino types so it can be covered by host tests.
//
// The web server has no login and answers with wildcard CORS, so these rules are
// what keep a saved key from being redirected or read by another page:
//  * isSameOrigin() turns away requests made by other websites in the phone's browser.
//  * applyUrlKeyUpdate() refuses to point a saved key at a new URL unless the key is
//    sent again, because the next chat would otherwise hand the key to that URL.
namespace hermesweb {

// Same limits as the on-device keyboard in HermesSettingsActivity.
constexpr size_t kMaxUrlLength = 160;
constexpr size_t kMaxKeyLength = 200;
constexpr size_t kMaxBodyLength = 1024;

enum class UpdateResult { Ok, InvalidUrl, InvalidKey, KeyRequired };

// One optional text field of the request: absent means "keep the saved value".
struct OptionalText {
  bool present = false;
  std::string value;
};

// Pasting from a phone often brings along a trailing space or newline.
inline std::string trimmed(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

// Empty is allowed (it clears the URL); otherwise http(s), a host, and no spaces or control characters.
inline bool isValidUrl(const std::string& url) {
  if (url.empty()) return true;
  if (url.size() > kMaxUrlLength) return false;
  size_t schemeLength = 0;
  if (url.rfind("http://", 0) == 0) {
    schemeLength = 7;
  } else if (url.rfind("https://", 0) == 0) {
    schemeLength = 8;
  } else {
    return false;
  }
  if (url.size() == schemeLength || url[schemeLength] == '/') return false;
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte == 0x7F) return false;
  }
  return true;
}

// Empty is allowed (it clears the key). Keys are sent in an Authorization header,
// so only visible ASCII is accepted; that also rules out header injection.
inline bool isValidKey(const std::string& key) {
  if (key.size() > kMaxKeyLength) return false;
  for (const char c : key) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte >= 0x7F) return false;
  }
  return true;
}

// Applies one URL + key pair. Nothing is changed unless the whole pair is acceptable.
inline UpdateResult applyUrlKeyUpdate(std::string& url, std::string& key, const OptionalText& newUrl,
                                      const OptionalText& newKey) {
  std::string nextUrl = newUrl.present ? trimmed(newUrl.value) : url;
  std::string nextKey = newKey.present ? trimmed(newKey.value) : key;
  if (!isValidUrl(nextUrl)) return UpdateResult::InvalidUrl;
  if (!isValidKey(nextKey)) return UpdateResult::InvalidKey;
  if (nextUrl != url && !nextUrl.empty() && !key.empty() && !newKey.present) return UpdateResult::KeyRequired;
  url = std::move(nextUrl);
  key = std::move(nextKey);
  return UpdateResult::Ok;
}

// True when the request came from the page the device itself served, or from a
// non-browser client (no Origin header). Browsers set Origin on cross-site requests.
inline bool isSameOrigin(const std::string& origin, const std::string& host) {
  if (origin.empty()) return true;
  const size_t scheme = origin.find("://");
  if (scheme == std::string::npos) return false;  // includes the literal "null" origin
  const std::string originHost = origin.substr(scheme + 3);
  if (originHost.size() != host.size()) return false;
  for (size_t i = 0; i < host.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(originHost[i])) != std::tolower(static_cast<unsigned char>(host[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace hermesweb

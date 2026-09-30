#include "ScreenLock.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

namespace ScreenLock {

namespace {

constexpr char kNamespace[] = "cmx-lock";
constexpr char kSaltKey[] = "salt";
constexpr char kHashKey[] = "hash";
constexpr char kFailsKey[] = "fails";
constexpr char kLockedKey[] = "locked";
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;

// Lockout clock is RAM-only: a reboot restarts the current wait, but the
// persisted failure count keeps later waits long.
uint32_t lockoutUntilMs = 0;
bool lockoutRestored = false;

bool hashPin(const uint8_t* salt, const std::string& pin, uint8_t* out) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  const bool ok = mbedtls_sha256_starts(&context, 0) == 0 && mbedtls_sha256_update(&context, salt, kSaltBytes) == 0 &&
                  mbedtls_sha256_update(&context, reinterpret_cast<const uint8_t*>(pin.data()), pin.size()) == 0 &&
                  mbedtls_sha256_finish(&context, out) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}

// Constant-time compare so timing does not leak how many bytes matched.
bool equalDigest(const uint8_t* a, const uint8_t* b) {
  uint8_t diff = 0;
  for (size_t i = 0; i < kHashBytes; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

void setFails(Preferences& prefs, const uint8_t fails) {
  prefs.putUChar(kFailsKey, fails);
  if (fails < kFreeAttempts) {
    lockoutUntilMs = 0;
    return;
  }
  const uint8_t doublings = std::min<uint8_t>(fails - kFreeAttempts, 5);
  const uint32_t wait = std::min<uint32_t>(kFirstLockoutMs << doublings, kMaxLockoutMs);
  lockoutUntilMs = millis() + wait;
}

}  // namespace

void setLocked(const bool locked) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  if (prefs.getBool(kLockedKey, false) != locked) prefs.putBool(kLockedKey, locked);
  prefs.end();
}

bool wasLocked() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;
  const bool locked = prefs.getBool(kLockedKey, false);
  prefs.end();
  return locked;
}

bool isValidPin(const std::string& pin) {
  return pin.size() == kPinLength && std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool hasPin() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return false;
  const bool present = prefs.getBytesLength(kHashKey) == kHashBytes;
  prefs.end();
  return present;
}

bool setPin(const std::string& pin) {
  if (!isValidPin(pin)) return false;
  uint8_t salt[kSaltBytes];
  uint8_t digest[kHashBytes];
  esp_fill_random(salt, sizeof(salt));
  if (!hashPin(salt, pin, digest)) return false;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const bool ok =
      prefs.putBytes(kSaltKey, salt, sizeof(salt)) == sizeof(salt) && prefs.putBytes(kHashKey, digest, sizeof(digest)) == sizeof(digest);
  setFails(prefs, 0);
  prefs.end();
  lockoutRestored = true;
  if (!ok) LOG_ERR("LOCK", "Failed to store PIN");
  return ok;
}

void clearPin() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return;
  prefs.clear();
  prefs.end();
  lockoutUntilMs = 0;
  lockoutRestored = true;
}

uint32_t lockoutRemainingMs() {
  if (!lockoutRestored) {
    // After a reboot, re-arm the wait the stored failure count calls for, so
    // power-cycling does not skip a lockout.
    lockoutRestored = true;
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
      const uint8_t fails = prefs.getUChar(kFailsKey, 0);
      if (fails >= kFreeAttempts) setFails(prefs, fails);
      prefs.end();
    }
  }
  if (lockoutUntilMs == 0) return 0;
  const int32_t left = static_cast<int32_t>(lockoutUntilMs - millis());
  if (left <= 0) {
    lockoutUntilMs = 0;
    return 0;
  }
  return static_cast<uint32_t>(left);
}

uint8_t failedAttempts() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, true)) return 0;
  const uint8_t fails = prefs.getUChar(kFailsKey, 0);
  prefs.end();
  return fails;
}

bool verify(const std::string& pin) {
  if (lockoutRemainingMs() > 0) return false;
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  uint8_t salt[kSaltBytes];
  uint8_t stored[kHashBytes];
  uint8_t digest[kHashBytes];
  const bool loaded = prefs.getBytes(kSaltKey, salt, sizeof(salt)) == sizeof(salt) &&
                      prefs.getBytes(kHashKey, stored, sizeof(stored)) == sizeof(stored);
  if (!loaded) {
    prefs.end();
    return true;  // no PIN configured: nothing to check
  }
  const bool ok = isValidPin(pin) && hashPin(salt, pin, digest) && equalDigest(digest, stored);
  const uint8_t fails = prefs.getUChar(kFailsKey, 0);
  setFails(prefs, ok ? 0 : static_cast<uint8_t>(std::min(fails + 1, 250)));
  prefs.end();
  return ok;
}

}  // namespace ScreenLock

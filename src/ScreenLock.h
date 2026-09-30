#pragma once

#include <cstdint>
#include <string>

// 4-digit screen-lock PIN, kept in NVS (internal flash) rather than on the SD
// card so swapping or editing the card cannot remove it. Stored as
// SHA-256(salt || pin) with a per-device random salt. This is a casual privacy
// lock: 10^4 PINs are trivially brute-forced by anyone who can dump flash.
namespace ScreenLock {

constexpr size_t kPinLength = 4;
// Wrong attempts allowed before lockouts start; each further miss doubles the
// wait from kFirstLockoutMs up to kMaxLockoutMs.
constexpr uint8_t kFreeAttempts = 5;
constexpr uint32_t kFirstLockoutMs = 30 * 1000;
constexpr uint32_t kMaxLockoutMs = 15 * 60 * 1000;

bool hasPin();
bool setPin(const std::string& pin);  // exactly kPinLength digits
void clearPin();

// Checks a PIN, updating the persisted failure count. Returns false without
// checking while a lockout is running.
bool verify(const std::string& pin);
// Milliseconds left in the current lockout (0 when attempts are allowed).
uint32_t lockoutRemainingMs();
uint8_t failedAttempts();

bool isValidPin(const std::string& pin);

// Set while the lock screen is up, so sleeping from the lock (page-button
// combo, auto-sleep) comes back locked even when "Lock when waking" is off.
void setLocked(bool locked);
bool wasLocked();

}  // namespace ScreenLock

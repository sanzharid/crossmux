#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace applab {

// One generateContent request on its own task, shared by reference count
// between the task and the activity (either may finish first).
struct GeminiJob {
  enum class State : uint8_t { Running, Done, Failed };
  enum class Error : uint8_t { None, Unreachable, Cancelled, NoMemory, TooLarge };

  // Inputs (written before start, read-only afterwards).
  std::string url;
  std::string apiKey;  // sent as the x-goog-api-key header; never logged
  std::string body;
  uint32_t timeoutMs = 120000;

  // Outputs (valid once state != Running).
  std::string reply;
  int httpStatus = 0;
  Error error = Error::None;

  std::atomic<State> state{State::Running};
  std::atomic<bool> cancel{false};
  std::atomic<uint8_t> refs{1};

  static GeminiJob* create();
  void release();
};

// Starts the job; on success the task holds its own reference.
bool startGeminiJob(GeminiJob* job);

}  // namespace applab

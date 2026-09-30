#pragma once

// Background work for the Hermes chat app: HTTP requests (chat, transcription)
// and microphone capture run on short-lived FreeRTOS tasks so the UI keeps
// rendering and the user can cancel.
//
// Ownership: a job is shared by the activity and its task through an atomic
// reference count. Either side may drop its reference first (the activity can
// exit mid-request); the last release frees the job. The task always deletes
// itself, so no task outlives its job and none needs vTaskDelete() from onExit().

#include <Memory.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "HermesProtocol.h"

namespace hermes {

enum class JobState : uint8_t { Running, Done, Failed };

// Failure reasons; the activity maps them to translated strings.
enum class JobError : uint8_t {
  None,
  InvalidUrl,
  Unreachable,
  Cancelled,
  BadReply,
  NoText,
  HttpStatus,
  NoMemory,
  MicUnavailable,
  MicFailed,
};

struct NetJob {
  enum class Kind : uint8_t { Chat, Transcribe };

  // Inputs (written before start, read-only afterwards).
  Kind kind = Kind::Chat;
  std::string url;
  std::string apiKey;
  std::string body;       // chat JSON
  std::string sessionId;  // X-Hermes-Session-Id for chat completions
  ApiStyle style = ApiStyle::Responses;
  std::string sttModel;
  std::string sttLanguage;
  memory::ByteBuffer audio;  // PSRAM: 44-byte WAV header + PCM
  size_t audioBytes = 0;
  uint32_t timeoutMs = 180000;

  // Outputs (valid once state != Running).
  std::string text;
  JobError error = JobError::None;
  std::string serverMessage;  // untranslated detail from the server, if any
  int httpStatus = 0;

  std::atomic<JobState> state{JobState::Running};
  std::atomic<bool> cancel{false};
  std::atomic<uint8_t> refs{1};

  static NetJob* create();
  void release();
};

// Starts the job on its own task; on success the task holds a reference.
bool startNetJob(NetJob* job);

struct RecordJob {
  memory::ByteBuffer buffer;  // PSRAM: WAV header space + PCM samples
  size_t capacitySamples = 0;
  uint32_t sampleRate = 16000;
  std::atomic<size_t> samples{0};
  std::atomic<uint16_t> level{0};  // recent peak, 0..32767, for the meter
  std::atomic<JobState> state{JobState::Running};
  std::atomic<bool> stop{false};
  std::atomic<uint8_t> refs{1};
  JobError error = JobError::None;

  static RecordJob* create(uint8_t maxSeconds);
  void release();
  // After the task finishes: DC-removes and normalizes the PCM, writes the WAV
  // header in place, and returns the total WAV size (0 if nothing recorded).
  size_t finalizeWav();
};

// True when this build has microphone support for the active board.
bool micAvailable();
// True while a recording task still owns the microphone (e.g. just cancelled).
bool micBusy();
bool startRecordJob(RecordJob* job);

}  // namespace hermes

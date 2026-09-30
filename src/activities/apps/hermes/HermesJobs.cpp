#include "HermesJobs.h"

#include "network/HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <HalStorage.h>
#include <SecureHttpClient.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#if HERMES_HAS_MIC
#include <Buzzer.h>
#include <Microphone.h>
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <BoardConfig.h>  // BoardConfig::ACTIVE.mic — the mic pins to release
// Direct PAD control for the USB-Serial-JTAG release in recordTask() below.
#include <driver/gpio.h>
#include <soc/usb_serial_jtag_struct.h>
#endif
#endif

namespace hermes {

namespace {

// TLS (https endpoints) runs wolfSSL on this stack; plain LAN http needs far less.
constexpr uint32_t kNetTaskStackBytes = 12 * 1024;
// HttpDownloader keeps its TLS client and a 1 KB copy buffer on the stack.
constexpr uint32_t kDownloadTaskStackBytes = 16 * 1024;
constexpr uint32_t kRecordTaskStackBytes = 4 * 1024;
constexpr char kBoundary[] = "----CrossMuxHermesBoundary7f3a";
constexpr size_t kMaxReplyBytes = 256 * 1024;

std::atomic<uint8_t> activeRecorders{0};

constexpr uint32_t kStartBeepHz = 2000;
constexpr uint32_t kStopBeepHz = 1200;
constexpr uint32_t kBeepMs = 70;

template <typename Job>
void releaseJob(Job* job) {
  if (job->refs.fetch_sub(1) == 1) delete job;
}

// Maps a finished HTTP exchange onto the job's error fields.
void classify(NetJob& job, const int status, const ParseResult parsed) {
  job.httpStatus = status;
  if (status < 0) {
    job.error = job.cancel ? JobError::Cancelled : JobError::Unreachable;
  } else if (status >= 400) {
    job.error = JobError::HttpStatus;
  } else if (parsed == ParseResult::BadJson) {
    job.error = JobError::BadReply;
  } else if (parsed == ParseResult::NoText) {
    job.error = JobError::NoText;
  }
  if (job.error != JobError::None) job.text.clear();
}

void runChat(NetJob& job, freeink::SecureHttpClient& http) {
  http.addHeader("Content-Type", "application/json");
  if (job.style == ApiStyle::ChatCompletions && !job.sessionId.empty()) {
    http.addHeader("X-Hermes-Session-Id", job.sessionId);
  }
  std::string reply;
  const int status = http.sendRequest(
      "POST", reinterpret_cast<const uint8_t*>(job.body.data()), job.body.size(),
      [&reply](const uint8_t* data, const size_t len) {
        if (reply.size() + len > kMaxReplyBytes) return false;
        reply.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      [&job] { return job.cancel.load(); });
  const ParseResult parsed =
      status < 0 ? ParseResult::NoText : parseReply(reply, job.style, job.text, job.serverMessage);
  classify(job, status, parsed);
}

void runTranscribe(NetJob& job, freeink::SecureHttpClient& http) {
  const std::string head = multipartHead(kBoundary, job.sttModel, job.sttLanguage);
  const std::string tail = multipartTail(kBoundary);
  const size_t total = head.size() + job.audioBytes + tail.size();
  // Up to ~3.8 MB for a 120 s clip: only PSRAM can hold it; the WAV buffer is
  // released right after the copy so the peak is two clips, not three.
  memory::ByteBuffer body = memory::makePsramByteBufferUninitializedNoThrow(total);
  if (!body) {
    LOG_ERR("HERMES", "OOM: upload body (%u bytes)", static_cast<unsigned>(total));
    job.error = JobError::NoMemory;
    return;
  }
  memcpy(body.get(), head.data(), head.size());
  memcpy(body.get() + head.size(), job.audio.get(), job.audioBytes);
  memcpy(body.get() + head.size() + job.audioBytes, tail.data(), tail.size());
  job.audio.reset();

  http.addHeader("Content-Type", std::string("multipart/form-data; boundary=") + kBoundary);
  std::string reply;
  const int status = http.sendRequest(
      "POST", body.get(), total,
      [&reply](const uint8_t* data, const size_t len) {
        if (reply.size() + len > kMaxReplyBytes) return false;
        reply.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      [&job] { return job.cancel.load(); });
  const ParseResult parsed = status < 0 ? ParseResult::NoText : parseTranscript(reply, job.text, job.serverMessage);
  classify(job, status, parsed);
}

// Streams a book to destPath.part, then renames it into place so a half
// download never shows up in the library.
void runDownload(NetJob& job) {
  const std::string part = job.destPath + ".part";
  const auto result = HttpDownloader::downloadToFile(job.url, part, nullptr, &job.downloadCancel);
  if (result == HttpDownloader::OK) {
    // Never replace an existing book: if the name got taken meanwhile, the
    // rename fails and the download is reported as a file error.
    if (!Storage.exists(job.destPath.c_str()) && Storage.rename(part.c_str(), job.destPath.c_str())) {
      job.text = job.destPath;
      return;
    }
    Storage.remove(part.c_str());
    job.error = JobError::FileError;
    return;
  }
  if (Storage.exists(part.c_str())) Storage.remove(part.c_str());
  job.error = result == HttpDownloader::ABORTED      ? JobError::Cancelled
              : result == HttpDownloader::FILE_ERROR ? JobError::FileError
                                                     : JobError::Unreachable;
}

void netTask(void* param) {
  auto* job = static_cast<NetJob*>(param);
  if (job->kind == NetJob::Kind::Download) {
    runDownload(*job);
  } else {
    freeink::SecureHttpClient http;
    http.setInsecure();  // LAN servers commonly use self-signed certs
    http.setTimeout(job->timeoutMs);
    http.setFollowRedirects(2);
    if (!http.begin(job->url)) {
      job->error = JobError::InvalidUrl;
    } else {
      if (!job->apiKey.empty()) http.addHeader("Authorization", "Bearer " + job->apiKey);
      http.addHeader("Accept", "application/json");
      if (job->kind == NetJob::Kind::Chat) {
        runChat(*job, http);
      } else {
        runTranscribe(*job, http);
      }
      http.end();
    }
  }
  LOG_INF("HERMES", "job %u done: http=%d err=%u", static_cast<unsigned>(job->kind), job->httpStatus,
          static_cast<unsigned>(job->error));
  job->state = job->error == JobError::None ? JobState::Done : JobState::Failed;
  job->release();
  vTaskDelete(nullptr);
}

#if HERMES_HAS_MIC
void recordTask(void* param) {
  auto* job = static_cast<RecordJob*>(param);
  activeRecorders.fetch_add(1);
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  // On the ESP32-S3 the PDM mic CLK/DATA pads are the *same physical pads* as
  // USB-Serial-JTAG, and that peripheral is routed to the pad directly, not through
  // the GPIO matrix. While the USB PHY holds them, the PDM RX channel reads a full
  // block of zeros -- a well-formed WAV of pure silence that transcribes to "" and
  // surfaces as "didn't catch that". Disabling the pad hands the pins back to I2S.
  // Board-safe: on the Sticky, flashing and serial both use the CH343P bridge on
  // UART0, so the native USB-JTAG is unused.
  const BoardConfig::MicConfig& micCfg = BoardConfig::ACTIVE.mic;
  if (micCfg.input == BoardConfig::MicInput::Pdm) {
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
    if (micCfg.clk != BoardConfig::PIN_UNASSIGNED) gpio_reset_pin(static_cast<gpio_num_t>(micCfg.clk));
    if (micCfg.data != BoardConfig::PIN_UNASSIGNED) gpio_reset_pin(static_cast<gpio_num_t>(micCfg.data));
  }
#endif
  freeink::Microphone mic;
  if (!mic.begin(job->sampleRate)) {
    job->error = JobError::MicUnavailable;
  } else {
    // Discard ~100 ms while the PDM decimation filter settles (start-up pop).
    int16_t scratch[256];
    for (int i = 0; i < 6; ++i) mic.read(scratch, 256, 50);

    // "Speak now" prompt, like the stock firmware; the samples captured while
    // it plays are discarded so the beep isn't sent for transcription.
    freeink::Buzzer buzzer;
    if (buzzer.begin()) {
      buzzer.tone(kStartBeepHz, kBeepMs);
      for (int i = 0; i < 4; ++i) mic.read(scratch, 256, 50);
    }

    auto* pcm = reinterpret_cast<int16_t*>(job->buffer.get() + kWavHeaderBytes);
    size_t count = 0;
    while (!job->stop && count < job->capacitySamples) {
      const size_t want = std::min<size_t>(512, job->capacitySamples - count);
      const int got = mic.read(pcm + count, want, 100);
      if (got < 0) {
        job->error = JobError::MicFailed;
        break;
      }
      int peak = 0;
      for (int i = 0; i < got; ++i) peak = std::max(peak, std::abs(static_cast<int>(pcm[count + i])));
      count += static_cast<size_t>(got);
      job->samples = count;
      job->level = static_cast<uint16_t>(std::min(peak, 32767));
    }
    mic.end();
    if (buzzer.present()) {
      buzzer.tone(kStopBeepHz, kBeepMs);  // "got it"
      buzzer.end();
    }
  }
  if (job->error != JobError::None) LOG_ERR("HERMES", "mic error %u", static_cast<unsigned>(job->error));
  LOG_INF("HERMES", "recorded %u samples", static_cast<unsigned>(job->samples.load()));
  job->state = job->error == JobError::None ? JobState::Done : JobState::Failed;
  job->release();
  activeRecorders.fetch_sub(1);
  vTaskDelete(nullptr);
}
#endif

}  // namespace

NetJob* NetJob::create() { return new (std::nothrow) NetJob(); }

void NetJob::release() { releaseJob(this); }

bool startNetJob(NetJob* job) {
  job->refs.fetch_add(1);
  const uint32_t stack = job->kind == NetJob::Kind::Download ? kDownloadTaskStackBytes : kNetTaskStackBytes;
  if (xTaskCreatePinnedToCore(&netTask, "HermesNet", stack, job, 1, nullptr, 0) != pdPASS) {
    job->refs.fetch_sub(1);
    LOG_ERR("HERMES", "Failed to start network task");
    return false;
  }
  return true;
}

RecordJob* RecordJob::create(const uint8_t maxSeconds) {
  auto* job = new (std::nothrow) RecordJob();
  if (!job) return nullptr;
  job->capacitySamples = static_cast<size_t>(job->sampleRate) * maxSeconds;
  const size_t bytes = kWavHeaderBytes + job->capacitySamples * sizeof(int16_t);
  job->buffer = memory::makePsramByteBufferUninitializedNoThrow(bytes);
  if (!job->buffer) {
    LOG_ERR("HERMES", "OOM: recording buffer (%u bytes)", static_cast<unsigned>(bytes));
    delete job;
    return nullptr;
  }
  return job;
}

void RecordJob::release() { releaseJob(this); }

size_t RecordJob::finalizeWav() {
  const size_t count = samples.load();
  if (count == 0) return 0;
  auto* pcm = reinterpret_cast<int16_t*>(buffer.get() + kWavHeaderBytes);

  // Remove DC offset, then normalize to ~-3 dBFS (capped at 16x) — the
  // Sticky's PDM mic is quiet at arm's length and STT does better with level.
  int64_t sum = 0;
  for (size_t i = 0; i < count; ++i) sum += pcm[i];
  const int32_t dc = static_cast<int32_t>(sum / static_cast<int64_t>(count));
  int32_t peak = 1;
  for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(static_cast<int32_t>(pcm[i]) - dc));
  const int32_t gainQ8 = std::min<int32_t>(16 * 256, (23000 * 256) / peak);
  for (size_t i = 0; i < count; ++i) {
    const int32_t v = ((static_cast<int32_t>(pcm[i]) - dc) * gainQ8) >> 8;
    pcm[i] = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, v)));
  }

  const auto pcmBytes = static_cast<uint32_t>(count * sizeof(int16_t));
  writeWavHeader(buffer.get(), pcmBytes, sampleRate);
  return kWavHeaderBytes + pcmBytes;
}

bool micAvailable() {
#if HERMES_HAS_MIC
  return true;
#else
  return false;
#endif
}

bool micBusy() { return activeRecorders.load() != 0; }

bool startRecordJob(RecordJob* job) {
#if HERMES_HAS_MIC
  job->refs.fetch_add(1);
  if (xTaskCreatePinnedToCore(&recordTask, "HermesMic", kRecordTaskStackBytes, job, 2, nullptr, 0) != pdPASS) {
    job->refs.fetch_sub(1);
    LOG_ERR("HERMES", "Failed to start recording task");
    return false;
  }
  return true;
#else
  (void)job;
  return false;
#endif
}

}  // namespace hermes

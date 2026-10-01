#include "HermesJobs.h"

#include "network/HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <HalStorage.h>
#include <SecureHttpClient.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#if HERMES_HAS_MIC
#include <BoardConfig.h>  // BoardConfig::ACTIVE.mic — pins, enable rail
#include <Buzzer.h>
#include <driver/i2s_pdm.h>
#include <driver/gpio.h>
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
// PDM capture that finds the live data phase itself. The Sticky's mic answers
// on one clock edge / slot, and which one reads back as audio was observed to
// differ between a clean boot (left slot, normal clock) and later recordings
// in the running app, where the same configuration read one constant value
// (-30935: the idle line on the wrong phase; issue #1). Instead of trusting a
// fixed configuration, each recording tries the combinations for ~160 ms and
// keeps the first whose samples actually vary.
class PdmCapture {
 public:
  bool open(const uint32_t sampleRate) {
    const auto& cfg = BoardConfig::ACTIVE.mic;
    if (cfg.input != BoardConfig::MicInput::Pdm || cfg.clk == BoardConfig::PIN_UNASSIGNED ||
        cfg.data == BoardConfig::PIN_UNASSIGNED) {
      return false;
    }
    // Deep sleep isolates and holds every pad (esp_sleep_config_gpio_isolate +
    // gpio_deep_sleep_hold_en), and the hold survives the wake reset: until it is
    // released, the enable pin stays LOW (mic unpowered) and the clock pin ignores
    // I2S, so every sample reads the same idle value (issue #1). EpdBus and the
    // SD rail release their own pins the same way.
    for (const int8_t pin : {cfg.enable, cfg.clk, cfg.data}) {
      if (pin != BoardConfig::PIN_UNASSIGNED) gpio_hold_dis(static_cast<gpio_num_t>(pin));
    }
    power(true);
    delay(30);  // mic start-up after its rail comes up
    struct Phase {
      bool rightSlot;
      bool clkInvert;
    };
    static constexpr Phase kPhases[] = {{false, false}, {true, false}, {false, true}, {true, true}};
    int16_t probe[256];
    for (const Phase& phase : kPhases) {
      if (!start(sampleRate, phase.rightSlot, phase.clkInvert)) continue;
      for (int i = 0; i < 6; ++i) read(probe, 256, 50);  // ~100 ms: filter settle / start-up pop
      int lo = 32767;
      int hi = -32768;
      for (int i = 0; i < 4; ++i) {
        const int got = read(probe, 256, 50);
        for (int k = 0; k < got; ++k) {
          lo = std::min(lo, static_cast<int>(probe[k]));
          hi = std::max(hi, static_cast<int>(probe[k]));
        }
      }
      if (hi - lo > 8) {  // a live mic is never flat, even in a quiet room
        LOG_INF("HERMES", "mic live on %s slot, clock %s (probe range %d..%d)", phase.rightSlot ? "right" : "left",
                phase.clkInvert ? "inverted" : "normal", lo, hi);
        return true;
      }
      LOG_INF("HERMES", "mic flat on %s slot, clock %s (value %d)", phase.rightSlot ? "right" : "left",
              phase.clkInvert ? "inverted" : "normal", lo);
      stop();
    }
    LOG_ERR("HERMES", "mic flat in every PDM phase");
    power(false);
    return false;
  }

  int read(int16_t* dst, const size_t maxSamples, const uint32_t timeoutMs) {
    if (!rx_) return -1;
    size_t bytes = 0;
    const esp_err_t err = i2s_channel_read(rx_, dst, maxSamples * sizeof(int16_t), &bytes, pdMS_TO_TICKS(timeoutMs));
    if (err == ESP_ERR_TIMEOUT) return 0;
    if (err != ESP_OK) return -1;
    return static_cast<int>(bytes / sizeof(int16_t));
  }

  void close() {
    stop();
    power(false);
  }

 private:
  i2s_chan_handle_t rx_ = nullptr;

  static void power(const bool on) {
    const auto& cfg = BoardConfig::ACTIVE.mic;
    if (cfg.enable == BoardConfig::PIN_UNASSIGNED) return;
    pinMode(cfg.enable, OUTPUT);
    digitalWrite(cfg.enable, on == cfg.enableActiveHigh ? HIGH : LOW);
  }

  bool start(const uint32_t sampleRate, const bool rightSlot, const bool clkInvert) {
    const auto& cfg = BoardConfig::ACTIVE.mic;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan, nullptr, &rx_) != ESP_OK) {
      rx_ = nullptr;
      return false;
    }
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = static_cast<gpio_num_t>(cfg.clk),
                     .din = static_cast<gpio_num_t>(cfg.data),
                     .invert_flags = {.clk_inv = clkInvert}},
    };
    pdm.slot_cfg.slot_mask = rightSlot ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
    if (i2s_channel_init_pdm_rx_mode(rx_, &pdm) != ESP_OK || i2s_channel_enable(rx_) != ESP_OK) {
      i2s_del_channel(rx_);
      rx_ = nullptr;
      return false;
    }
    return true;
  }

  void stop() {
    if (!rx_) return;
    i2s_channel_disable(rx_);
    i2s_del_channel(rx_);
    rx_ = nullptr;
  }
};

void recordTask(void* param) {
  auto* job = static_cast<RecordJob*>(param);
  activeRecorders.fetch_add(1);
  PdmCapture mic;
  if (!mic.open(job->sampleRate)) {
    job->error = JobError::MicFailed;
  } else {
    int16_t scratch[256];

    // "Speak now" prompt, like the stock firmware; the samples captured while
    // it plays are discarded so the beep isn't sent for transcription.
    // Same deep-sleep pad hold as the mic pins (see PdmCapture::open).
    if (BoardConfig::ACTIVE.audio.buzzer != BoardConfig::PIN_UNASSIGNED) {
      gpio_hold_dis(static_cast<gpio_num_t>(BoardConfig::ACTIVE.audio.buzzer));
    }
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
    mic.close();
    if (buzzer.present()) {
      buzzer.tone(kStopBeepHz, kBeepMs);  // "got it"
      buzzer.end();
    }
  }
  if (job->error != JobError::None) LOG_ERR("HERMES", "mic error %u", static_cast<unsigned>(job->error));
  // Raw clip statistics, to tell speech from silence (peak 0, issue #1) or from
  // an unconverted PDM bitstream (near full-scale noise: zero-crossing rate ~0.5).
  {
    const auto* pcm = reinterpret_cast<const int16_t*>(job->buffer.get() + kWavHeaderBytes);
    const size_t count = job->samples.load();
    int peak = 0;
    int64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
      peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
      sum += pcm[i];
    }
    const int32_t mean = count ? static_cast<int32_t>(sum / static_cast<int64_t>(count)) : 0;
    double energy = 0;
    size_t crossings = 0;
    for (size_t i = 0; i < count; ++i) {
      const int32_t v = pcm[i] - mean;
      energy += static_cast<double>(v) * v;
      if (i > 0 && ((pcm[i - 1] - mean) < 0) != (v < 0)) ++crossings;
    }
    const int rms = count ? static_cast<int>(sqrt(energy / static_cast<double>(count))) : 0;
    const int zcrPermille = count > 1 ? static_cast<int>(crossings * 1000 / (count - 1)) : 0;
    LOG_INF("HERMES", "recorded %u samples, raw peak %d, mean %ld, rms %d, zcr %d/1000", static_cast<unsigned>(count),
            peak, static_cast<long>(mean), rms, zcrPermille);
  }
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

#include "GeminiJob.h"

#include <Logging.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <new>

namespace applab {

namespace {

// mbedTLS handshakes need more stack than plain HTTP.
constexpr uint32_t kTaskStackBytes = 12 * 1024;
// A complete app is under 32 KiB of Lua; JSON escaping and thought parts add
// more, so allow generous headroom and stop beyond it.
constexpr size_t kMaxReplyBytes = 384 * 1024;
constexpr int kReadChunk = 2048;

// ESP-IDF's HTTP client over mbedTLS, not the SDK's wolfSSL client: it checks
// the server certificate against the built-in root bundle *and* the hostname,
// so the API key only ever reaches generativelanguage.googleapis.com.
void runRequest(GeminiJob& job) {
  esp_http_client_config_t config = {};
  config.url = job.url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = static_cast<int>(job.timeoutMs);
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.buffer_size = 4096;
  config.buffer_size_tx = 2048;
  config.disable_auto_redirect = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    job.error = GeminiJob::Error::NoMemory;
    return;
  }
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "x-goog-api-key", job.apiKey.c_str());

  if (esp_http_client_open(client, static_cast<int>(job.body.size())) != ESP_OK) {
    job.error = job.cancel ? GeminiJob::Error::Cancelled : GeminiJob::Error::Unreachable;
    esp_http_client_cleanup(client);
    return;
  }
  size_t sent = 0;
  while (sent < job.body.size() && !job.cancel) {
    const int wrote = esp_http_client_write(client, job.body.data() + sent, static_cast<int>(job.body.size() - sent));
    if (wrote <= 0) break;
    sent += static_cast<size_t>(wrote);
  }
  if (sent != job.body.size()) {
    job.error = job.cancel ? GeminiJob::Error::Cancelled : GeminiJob::Error::Unreachable;
    esp_http_client_cleanup(client);
    return;
  }

  esp_http_client_fetch_headers(client);
  job.httpStatus = esp_http_client_get_status_code(client);
  char chunk[kReadChunk];
  while (!job.cancel) {
    const int got = esp_http_client_read(client, chunk, sizeof(chunk));
    if (got < 0) {
      job.error = GeminiJob::Error::Unreachable;
      break;
    }
    if (got == 0) break;
    if (job.reply.size() + static_cast<size_t>(got) > kMaxReplyBytes) {
      job.error = GeminiJob::Error::TooLarge;
      break;
    }
    job.reply.append(chunk, static_cast<size_t>(got));
  }
  if (job.cancel) job.error = GeminiJob::Error::Cancelled;
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
}

void geminiTask(void* param) {
  auto* job = static_cast<GeminiJob*>(param);
  runRequest(*job);
  LOG_INF("APPLAB", "Gemini request done: http=%d err=%u bytes=%u", job->httpStatus, static_cast<unsigned>(job->error),
          static_cast<unsigned>(job->reply.size()));
  job->state = job->error == GeminiJob::Error::None ? GeminiJob::State::Done : GeminiJob::State::Failed;
  job->release();
  vTaskDelete(nullptr);
}

}  // namespace

GeminiJob* GeminiJob::create() { return new (std::nothrow) GeminiJob(); }

void GeminiJob::release() {
  if (refs.fetch_sub(1) == 1) delete this;
}

bool startGeminiJob(GeminiJob* job) {
  job->refs.fetch_add(1);
  if (xTaskCreatePinnedToCore(&geminiTask, "AppLabNet", kTaskStackBytes, job, 1, nullptr, 0) != pdPASS) {
    job->refs.fetch_sub(1);
    LOG_ERR("APPLAB", "Failed to start the Gemini task");
    return false;
  }
  return true;
}

}  // namespace applab

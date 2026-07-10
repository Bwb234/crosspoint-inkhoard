#ifdef ARDUINO

#include "InkHoardEspTransport.h"

#include <Arduino.h>
#include <Logging.h>
#include <algorithm>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "InkHoardModels.h"

namespace {
constexpr int HTTP_BUF_SIZE = 2048;
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;
constexpr int TIMEOUT_MS = 15000;

struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;
  bool oversize = false;

  ~ResponseBuffer() { free(data); }

  bool append(const char* chunk, int chunkLen) {
    if (oversize) return false;
    if (len + chunkLen > static_cast<int>(inkhoard::MAX_JSON_RESPONSE_BYTES)) {
      oversize = true;
      return false;
    }
    const int need = len + chunkLen + 1;
    if (need > capacity) {
      int newCap = capacity == 0 ? 1024 : capacity * 2;
      while (newCap < need) newCap *= 2;
      if (newCap > static_cast<int>(inkhoard::MAX_JSON_RESPONSE_BYTES) + 1) {
        newCap = static_cast<int>(inkhoard::MAX_JSON_RESPONSE_BYTES) + 1;
      }
      char* n = static_cast<char*>(realloc(data, newCap));
      if (!n) return false;
      data = n;
      capacity = newCap;
    }
    memcpy(data + len, chunk, chunkLen);
    len += chunkLen;
    data[len] = '\0';
    return true;
  }
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data && evt->data_len > 0) {
    buf->append(static_cast<const char*>(evt->data), evt->data_len);
  }
  return ESP_OK;
}

void wipe(std::string& s) {
  for (char& c : s) c = '\0';
  s.clear();
}
}  // namespace

inkhoard::TransportResponse InkHoardEspTransport::getJson(const std::string& url, const std::string& bearerToken) {
  inkhoard::TransportResponse out;

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("INKH", "Insufficient heap for TLS: %u", (unsigned)freeHeap);
    out.lowMemory = true;
    return out;
  }

  LOG_DBG("INKH", "GET %s (heap %u)", url.c_str(), (unsigned)freeHeap);

  ResponseBuffer buf;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.event_handler = httpEventHandler;
  config.user_data = &buf;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = TIMEOUT_MS;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    out.transportFailure = true;
    return out;
  }

  std::string auth = "Bearer ";
  auth += bearerToken;
  const esp_err_t hdrErr = esp_http_client_set_header(client, "Authorization", auth.c_str());
  wipe(auth);
  if (hdrErr != ESP_OK || esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
    esp_http_client_cleanup(client);
    out.transportFailure = true;
    return out;
  }

  const bool expectTls = url.rfind("https://", 0) == 0;
  const esp_err_t err = esp_http_client_perform(client);
  out.httpCode = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (buf.oversize) {
    out.ok = true;
    out.oversize = true;
    out.body.clear();
    return out;
  }

  if (err != ESP_OK) {
    LOG_DBG("INKH", "transport error: %s", esp_err_to_name(err));
    if (expectTls && out.httpCode == 0) {
      out.tlsFailure = true;
    } else {
      out.transportFailure = true;
    }
    return out;
  }

  out.ok = true;
  if (buf.data && buf.len > 0) {
    out.body.assign(buf.data, buf.len);
  }
  LOG_DBG("INKH", "HTTP %d body %u bytes heap %u", out.httpCode, (unsigned)out.body.size(),
          (unsigned)ESP.getFreeHeap());
  return out;
}

#endif  // ARDUINO

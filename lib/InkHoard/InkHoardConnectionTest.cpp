#include "InkHoardConnectionTest.h"

#include <Arduino.h>
#include <Logging.h>
#include <algorithm>
#include <cstring>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include "InkHoardCredentialStore.h"

namespace {
// Match KOReaderSyncClient TLS budgeting (plan 006 Wi-Fi floor ~105KB; TLS needs headroom).
constexpr int HTTP_BUF_SIZE = 2048;
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;
constexpr int RESPONSE_CAP = 512;  // only need status; discard body beyond this

struct ResponseBuffer {
  char* data = nullptr;
  int len = 0;
  int capacity = 0;

  ~ResponseBuffer() { free(data); }

  bool ensure(int size) {
    if (size > RESPONSE_CAP) size = RESPONSE_CAP;
    if (size <= capacity) return true;
    char* newData = static_cast<char*>(realloc(data, size));
    if (!newData) return false;
    data = newData;
    capacity = size;
    return true;
  }
};

esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  auto* buf = static_cast<ResponseBuffer*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf && buf->len < RESPONSE_CAP - 1) {
    const int copy = std::min(evt->data_len, RESPONSE_CAP - 1 - buf->len);
    if (copy > 0 && buf->ensure(buf->len + copy + 1)) {
      memcpy(buf->data + buf->len, evt->data, copy);
      buf->len += copy;
      buf->data[buf->len] = '\0';
    }
  }
  return ESP_OK;
}

void wipe(std::string& s) {
  for (char& c : s) c = '\0';
  s.clear();
}
}  // namespace

const char* InkHoardConnectionTest::resultLabel(Result r) {
  switch (r) {
    case Result::Ok:
      return "ok";
    case Result::NoCredentials:
      return "no_credentials";
    case Result::LowMemory:
      return "low_memory";
    case Result::Unreachable:
      return "unreachable";
    case Result::TlsFailure:
      return "tls_failure";
    case Result::Unauthorized:
      return "unauthorized";
    case Result::Forbidden:
      return "forbidden";
    case Result::HttpError:
      return "http_error";
  }
  return "unknown";
}

InkHoardConnectionTest::Outcome InkHoardConnectionTest::run() {
  Outcome out;
  if (!INKHOARD_STORE.hasCredentials()) {
    out.result = Result::NoCredentials;
    out.detail = "Set server URL and device token first";
    return out;
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_TLS) {
    LOG_ERR("INKH", "Insufficient heap for TLS: %u (need %u)", (unsigned)freeHeap, (unsigned)MIN_HEAP_FOR_TLS);
    out.result = Result::LowMemory;
    out.detail = "Not enough free memory for TLS";
    return out;
  }

  const std::string base = INKHOARD_STORE.getBaseUrl();
  const std::string url = base + "/api/device/v1/library?limit=1";
  // Log URL only — never Authorization or token
  LOG_DBG("INKH", "Connection test GET %s (heap %u)", url.c_str(), (unsigned)freeHeap);

  // Heuristic: cleartext http is rejected by store in release; if somehow present, treat as TLS N/A
  const bool expectTls = base.rfind("https://", 0) == 0;

  ResponseBuffer buf;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.event_handler = httpEventHandler;
  config.user_data = &buf;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 15000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    out.result = Result::Unreachable;
    out.detail = "Failed to create HTTP client";
    return out;
  }

  std::string auth = "Bearer ";
  auth += INKHOARD_STORE.getToken();
  const esp_err_t hdrErr = esp_http_client_set_header(client, "Authorization", auth.c_str());
  wipe(auth);
  if (hdrErr != ESP_OK || esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
    esp_http_client_cleanup(client);
    out.result = Result::Unreachable;
    out.detail = "Failed to set request headers";
    return out;
  }

  const esp_err_t err = esp_http_client_perform(client);
  const int httpCode = esp_http_client_get_status_code(client);
  out.httpCode = httpCode;
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    LOG_DBG("INKH", "Connection test transport error: %s (%d)", esp_err_to_name(err), (int)err);
    // Without a reliable ESP-IDF TLS error enum across IDF versions, map https failures
    // that never got an HTTP status to TlsFailure; plain network failures to Unreachable.
    if (expectTls && httpCode == 0) {
      out.result = Result::TlsFailure;
      out.detail = "TLS or connection failed";
    } else {
      out.result = Result::Unreachable;
      out.detail = "Host unreachable";
    }
    return out;
  }

  LOG_DBG("INKH", "Connection test HTTP %d", httpCode);

  if (httpCode == 200) {
    out.result = Result::Ok;
    out.detail = "Connected";
    return out;
  }
  if (httpCode == 401) {
    out.result = Result::Unauthorized;
    out.detail = "Token rejected — re-provision";
    return out;
  }
  if (httpCode == 403) {
    out.result = Result::Forbidden;
    out.detail = "Token lacks library access";
    return out;
  }

  out.result = Result::HttpError;
  out.detail = "Server error " + std::to_string(httpCode);
  return out;
}

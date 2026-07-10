// INKHOARD: plan 008 — EPUB download lives in src/ so HttpDownloader is visible.
#include "InkHoardClient.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "InkHoardCredentialStore.h"
#include "network/HttpDownloader.h"

namespace {
void copyEtag(char* dest, size_t cap, const char* src) {
  if (!dest || cap == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  std::strncpy(dest, src, cap - 1);
  dest[cap - 1] = '\0';
}

void delayRetry(uint8_t attemptIndex) { delay(attemptIndex == 0 ? 1000 : 3000); }
}  // namespace

InkHoardClient::Outcome InkHoardClient::downloadEpub(const char* id, const char* destPath, const char* ifNoneMatch,
                                                     char* etagOut, size_t etagOutCap, ProgressCallback progress) {
  Outcome outcome;
  if (!INKHOARD_STORE.hasCredentials()) {
    outcome.result = inkhoard::ClientResult::NoCredentials;
    return outcome;
  }
  if (!id || !id[0] || !destPath || !destPath[0]) {
    outcome.result = inkhoard::ClientResult::BadRequest;
    return outcome;
  }

  char path[160];
  std::snprintf(path, sizeof(path), "/api/device/v1/items/%s/content.epub", id);
  const std::string url = buildUrl(path);
  if (url.empty()) {
    outcome.result = inkhoard::ClientResult::NoCredentials;
    return outcome;
  }

  const std::string& tok = INKHOARD_STORE.getToken();

  constexpr uint8_t MAX_ATTEMPTS = 3;
  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    outcome.attempts = static_cast<uint8_t>(attempt + 1);
    int status = 0;
    char etagBuf[256] = {};
    HttpDownloader::RequestOptions opts;
    opts.outStatusCode = &status;
    opts.acceptStatus = [](int s) { return s == 200 || s == 304; };
    opts.setRequestHeaders = [&](esp_http_client_handle_t client) {
      std::string auth = "Bearer ";
      auth += tok;
      esp_http_client_set_header(client, "Authorization", auth.c_str());
      for (char& c : auth) c = '\0';
      if (ifNoneMatch && ifNoneMatch[0]) {
        esp_http_client_set_header(client, "If-None-Match", ifNoneMatch);
      }
    };
    opts.onResponseHeaders = [&](esp_http_client_handle_t client) {
      char* value = nullptr;
      if (esp_http_client_get_header(client, "ETag", &value) == ESP_OK && value) {
        std::strncpy(etagBuf, value, sizeof(etagBuf) - 1);
      }
    };

    LOG_DBG("INKH", "download EPUB attempt %u heap %u", (unsigned)outcome.attempts, (unsigned)ESP.getFreeHeap());
    const auto dl = HttpDownloader::downloadToFile(url, destPath, opts, progress, nullptr);
    outcome.httpCode = status;

    if (status == 304) {
      outcome.result = inkhoard::ClientResult::NotModified;
      copyEtag(etagOut, etagOutCap, etagBuf);
      return outcome;
    }
    if (status == 200 && dl == HttpDownloader::OK) {
      outcome.result = inkhoard::ClientResult::Ok;
      copyEtag(etagOut, etagOutCap, etagBuf);
      return outcome;
    }
    if (dl == HttpDownloader::ABORTED) {
      outcome.result = inkhoard::ClientResult::Aborted;
      return outcome;
    }
    if (dl == HttpDownloader::FILE_ERROR) {
      outcome.result = inkhoard::ClientResult::FileError;
      return outcome;
    }

    if (status == 401 || status == 403 || status == 404 || status == 409 || status == 422 || status == 400) {
      outcome.result = inkhoard::mapHttpStatus(status, nullptr);
      return outcome;
    }
    if (status >= 500 || status == 0 || dl == HttpDownloader::HTTP_ERROR) {
      outcome.result = (status == 0) ? inkhoard::ClientResult::TransportError : inkhoard::ClientResult::ServerError;
      if (attempt + 1 < MAX_ATTEMPTS) {
        Storage.remove(destPath);
        delayRetry(attempt);
        continue;
      }
      return outcome;
    }
    outcome.result = inkhoard::mapHttpStatus(status, nullptr);
    return outcome;
  }
  return outcome;
}

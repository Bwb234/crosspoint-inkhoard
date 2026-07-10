#include "InkHoardClient.h"

#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Logging.h>

#include "HttpDownloader.h"
#include "InkHoardCredentialStore.h"
#include "InkHoardEspTransport.h"
#endif

namespace {
#ifdef ARDUINO
InkHoardEspTransport& defaultTransport() {
  static InkHoardEspTransport t;
  return t;
}

bool hasCreds() { return INKHOARD_STORE.hasCredentials(); }
std::string baseUrl() { return INKHOARD_STORE.getBaseUrl(); }
const std::string& token() { return INKHOARD_STORE.getToken(); }
#else
// Host unit tests inject transport and must also inject credentials via a test double
// by calling methods that take explicit URL/token — see performJsonGetUrl.
bool hasCreds() { return true; }
std::string baseUrl() { return "https://test.invalid"; }
std::string tokenStorage;
const std::string& token() { return tokenStorage; }
#endif

void delayRetry(uint8_t attemptIndex) {
#ifdef ARDUINO
  delay(attemptIndex == 0 ? 1000 : 3000);
#else
  (void)attemptIndex;
#endif
}

#ifdef ARDUINO
void copyEtag(char* dest, size_t cap, const char* src) {
  if (!dest || cap == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  std::strncpy(dest, src, cap - 1);
  dest[cap - 1] = '\0';
}
#endif
}  // namespace

InkHoardClient::InkHoardClient(inkhoard::InkHoardTransport* transport) : transport_(transport) {
#ifdef ARDUINO
  if (!transport_) transport_ = &defaultTransport();
#endif
}

std::string InkHoardClient::buildUrl(const std::string& pathAndQuery) const {
  std::string base = baseUrl();
  if (base.empty()) return {};
  if (!pathAndQuery.empty() && pathAndQuery[0] != '/') {
    return base + "/" + pathAndQuery;
  }
  return base + pathAndQuery;
}

InkHoardClient::Outcome InkHoardClient::performJsonGet(const std::string& pathAndQuery,
                                                       InkHoardJsonParser::Kind kind, void* outStruct) {
  Outcome outcome;
  if (!hasCreds()) {
    outcome.result = inkhoard::ClientResult::NoCredentials;
    return outcome;
  }
  if (!transport_) {
    outcome.result = inkhoard::ClientResult::TransportError;
    return outcome;
  }

  const std::string url = buildUrl(pathAndQuery);
  if (url.empty()) {
    outcome.result = inkhoard::ClientResult::NoCredentials;
    return outcome;
  }

  constexpr uint8_t MAX_ATTEMPTS = 3;  // initial + 2 retries
  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    outcome.attempts = static_cast<uint8_t>(attempt + 1);
    const auto resp = transport_->getJson(url, token());
    outcome.httpCode = resp.httpCode;

    if (resp.lowMemory) {
      outcome.result = inkhoard::ClientResult::LowMemory;
      return outcome;
    }
    if (resp.oversize) {
      outcome.result = inkhoard::ClientResult::OversizeResponse;
      return outcome;
    }
    if (resp.tlsFailure) {
      outcome.result = inkhoard::ClientResult::TlsError;
      if (attempt + 1 < MAX_ATTEMPTS) {
        delayRetry(attempt);
        continue;
      }
      return outcome;
    }
    if (resp.transportFailure || !resp.ok) {
      outcome.result = inkhoard::ClientResult::TransportError;
      if (attempt + 1 < MAX_ATTEMPTS) {
        delayRetry(attempt);
        continue;
      }
      return outcome;
    }

    if (resp.httpCode == 200) {
      InkHoardJsonParser parser;
      parser.reset(kind);
      parser.feed(resp.body.data(), resp.body.size());
      if (parser.isOversize()) {
        outcome.result = inkhoard::ClientResult::OversizeResponse;
        return outcome;
      }
      if (!parser.ok()) {
        outcome.result = inkhoard::ClientResult::ParseError;
        return outcome;
      }
      switch (kind) {
        case InkHoardJsonParser::Kind::LibraryPage:
          *static_cast<inkhoard::LibraryPage*>(outStruct) = parser.libraryPage();
          break;
        case InkHoardJsonParser::Kind::SearchPage:
          *static_cast<inkhoard::SearchPage*>(outStruct) = parser.searchPage();
          break;
        case InkHoardJsonParser::Kind::CompactItem:
          *static_cast<inkhoard::CompactItem*>(outStruct) = parser.item();
          break;
        case InkHoardJsonParser::Kind::ApiError:
          break;
      }
      outcome.result = inkhoard::ClientResult::Ok;
      return outcome;
    }

    if (!resp.body.empty()) {
      InkHoardJsonParser errParser;
      errParser.reset(InkHoardJsonParser::Kind::ApiError);
      errParser.feed(resp.body.data(), resp.body.size());
      if (errParser.ok()) {
        outcome.error = errParser.apiError();
      }
    }
    outcome.result = inkhoard::mapHttpStatus(resp.httpCode, outcome.error.code);

    if (inkhoard::shouldRetry(outcome.result) && attempt + 1 < MAX_ATTEMPTS) {
      delayRetry(attempt);
      continue;
    }
    return outcome;
  }
  return outcome;
}

InkHoardClient::Outcome InkHoardClient::testConnection() {
  inkhoard::LibraryPage page;
  return performJsonGet("/api/device/v1/library?limit=1", InkHoardJsonParser::Kind::LibraryPage, &page);
}

InkHoardClient::Outcome InkHoardClient::fetchLibraryPage(inkhoard::LibraryPage& out, const char* cursor, int limit) {
  if (limit < 1) limit = 1;
  if (limit > static_cast<int>(inkhoard::MAX_PAGE_ITEMS)) limit = static_cast<int>(inkhoard::MAX_PAGE_ITEMS);
  char path[384];
  if (cursor && cursor[0]) {
    std::snprintf(path, sizeof(path), "/api/device/v1/library?limit=%d&cursor=%s", limit, cursor);
  } else {
    std::snprintf(path, sizeof(path), "/api/device/v1/library?limit=%d", limit);
  }
  return performJsonGet(path, InkHoardJsonParser::Kind::LibraryPage, &out);
}

InkHoardClient::Outcome InkHoardClient::search(inkhoard::SearchPage& out, const char* query, int offset, int limit) {
  if (!query || !query[0]) {
    Outcome o;
    o.result = inkhoard::ClientResult::BadRequest;
    return o;
  }
  if (limit < 1) limit = 1;
  if (limit > static_cast<int>(inkhoard::MAX_PAGE_ITEMS)) limit = static_cast<int>(inkhoard::MAX_PAGE_ITEMS);
  if (offset < 0) offset = 0;
  if (offset > 500) offset = 500;
  char path[512];
  std::string q;
  for (const char* p = query; *p; ++p) {
    if (*p == ' ')
      q += "%20";
    else if (*p == '&' || *p == '?' || *p == '#')
      continue;
    else
      q += *p;
  }
  std::snprintf(path, sizeof(path), "/api/device/v1/search?q=%s&limit=%d&offset=%d", q.c_str(), limit, offset);
  return performJsonGet(path, InkHoardJsonParser::Kind::SearchPage, &out);
}

InkHoardClient::Outcome InkHoardClient::fetchItemDetail(inkhoard::CompactItem& out, const char* id) {
  if (!id || !id[0]) {
    Outcome o;
    o.result = inkhoard::ClientResult::BadRequest;
    return o;
  }
  char path[160];
  std::snprintf(path, sizeof(path), "/api/device/v1/items/%s", id);
  return performJsonGet(path, InkHoardJsonParser::Kind::CompactItem, &out);
}

InkHoardClient::Outcome InkHoardClient::downloadEpub(const char* id, const char* destPath, const char* ifNoneMatch,
                                                     char* etagOut, size_t etagOutCap, ProgressCallback progress) {
  Outcome outcome;
#ifndef ARDUINO
  (void)id;
  (void)destPath;
  (void)ifNoneMatch;
  (void)etagOut;
  (void)etagOutCap;
  (void)progress;
  outcome.result = inkhoard::ClientResult::TransportError;
  return outcome;
#else
  if (!hasCreds()) {
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

  constexpr uint8_t MAX_ATTEMPTS = 3;
  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    outcome.attempts = static_cast<uint8_t>(attempt + 1);
    int status = 0;
    char etagBuf[256] = {};
    HttpDownloader::RequestOptions opts;
    opts.outStatusCode = &status;
    // Only stream body for success / not-modified; still capture status via outStatusCode.
    opts.acceptStatus = [](int s) { return s == 200 || s == 304; };
    opts.setRequestHeaders = [&](esp_http_client_handle_t client) {
      std::string auth = "Bearer ";
      auth += token();
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
#endif
}

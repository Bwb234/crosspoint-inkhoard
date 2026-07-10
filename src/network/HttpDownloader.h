#pragma once
#include <HalStorage.h>
#include <esp_http_client.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Optional request/response hooks (generic; upstreamable).
   * INKHOARD: plan 008 — Bearer, If-None-Match, ETag, non-200 status.
   */
  struct RequestOptions {
    /** Set custom request headers after client init (e.g. Authorization). */
    std::function<void(esp_http_client_handle_t)> setRequestHeaders;
    /** Called after headers are fetched (read ETag, etc.). */
    std::function<void(esp_http_client_handle_t)> onResponseHeaders;
    /** If non-null, receives the final HTTP status code. */
    int* outStatusCode = nullptr;
    /** Accept status codes other than 200 (default: 200 only). Return true to continue body read. */
    std::function<bool(int status)> acceptStatus;
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");

  /** Same as downloadToFile with RequestOptions (Bearer / 304 / ETag). INKHOARD: plan 008 */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath, const RequestOptions& opts,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");
};

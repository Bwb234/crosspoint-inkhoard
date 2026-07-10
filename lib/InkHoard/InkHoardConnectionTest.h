#pragma once

#include <string>

/**
 * One-shot connection test: GET /api/device/v1/library?limit=1 with Bearer.
 * Uses esp_http_client directly (HttpDownloader has no Bearer support).
 * INKHOARD: plan 007 — plan 008 will replace transport with InkHoardClient.
 */
class InkHoardConnectionTest {
 public:
  enum class Result {
    Ok,
    NoCredentials,
    LowMemory,
    Unreachable,
    TlsFailure,
    Unauthorized,  // HTTP 401
    Forbidden,     // HTTP 403
    HttpError,     // other HTTP
  };

  struct Outcome {
    Result result = Result::NoCredentials;
    int httpCode = 0;
    /** Short user-facing detail (never contains the token). */
    std::string detail;
  };

  /** Run the test using credentials currently in INKHOARD_STORE. */
  static Outcome run();

  static const char* resultLabel(Result r);
};

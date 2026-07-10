#pragma once

#include <string>

/**
 * One-shot connection test via InkHoardClient::testConnection().
 * INKHOARD: plan 007 UI surface; plan 008 transport.
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

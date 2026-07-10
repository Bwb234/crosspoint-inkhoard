#include "InkHoardConnectionTest.h"

#include <string>

#include "InkHoardClient.h"
#include "InkHoardModels.h"

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
  InkHoardClient client;
  const auto o = client.testConnection();
  out.httpCode = o.httpCode;

  switch (o.result) {
    case inkhoard::ClientResult::Ok:
      out.result = Result::Ok;
      out.detail = "Connected";
      break;
    case inkhoard::ClientResult::NoCredentials:
      out.result = Result::NoCredentials;
      out.detail = "Set server URL and device token first";
      break;
    case inkhoard::ClientResult::LowMemory:
      out.result = Result::LowMemory;
      out.detail = "Not enough free memory for TLS";
      break;
    case inkhoard::ClientResult::TlsError:
      out.result = Result::TlsFailure;
      out.detail = "TLS certificate or handshake failed";
      break;
    case inkhoard::ClientResult::TransportError:
      out.result = Result::Unreachable;
      out.detail = "Host unreachable";
      break;
    case inkhoard::ClientResult::AuthInvalid:
      out.result = Result::Unauthorized;
      out.detail = "Token rejected — re-provision";
      break;
    case inkhoard::ClientResult::AuthForbidden:
      out.result = Result::Forbidden;
      out.detail = "Token lacks library access";
      break;
    default:
      out.result = Result::HttpError;
      out.detail = "Server error " + std::to_string(o.httpCode);
      break;
  }
  return out;
}

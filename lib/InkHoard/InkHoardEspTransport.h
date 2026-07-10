#pragma once

#ifdef ARDUINO

#include "InkHoardTransport.h"

/**
 * Device transport: esp_http_client GET with Bearer, 32 KB body cap.
 * INKHOARD: plan 008
 */
class InkHoardEspTransport : public inkhoard::InkHoardTransport {
 public:
  inkhoard::TransportResponse getJson(const std::string& url, const std::string& bearerToken) override;
};

#endif  // ARDUINO

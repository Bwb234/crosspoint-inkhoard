#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

#include "InkHoardCredentialLogic.h"

/**
 * InkHoard server URL + device token on the SD card.
 * Token is XOR-obfuscated with the device MAC (concealment, not encryption).
 * INKHOARD: plan 007
 */
class InkHoardCredentialStore : public PersistableStore<InkHoardCredentialStore> {
 private:
  std::string serverUrl;
  std::string token;
  std::string displayName;

  InkHoardCredentialStore() = default;
  ~InkHoardCredentialStore() = default;

  friend class PersistableStore<InkHoardCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/inkhoard.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  /** Set URL (normalized). Returns false if rejected (e.g. plain http in release). */
  bool setServerUrl(const std::string& url, bool allowHttp = false);
  const std::string& getServerUrl() const { return serverUrl; }

  /** Set token; enforces ≤128 UTF-8 bytes. Never logs the value. */
  bool setToken(const std::string& value);
  const std::string& getToken() const { return token; }
  std::string getRedactedToken() const { return inkhoard::redactedToken(token); }

  bool setDisplayName(const std::string& name);
  const std::string& getDisplayName() const { return displayName; }

  bool hasCredentials() const { return !serverUrl.empty() && !token.empty(); }

  void clear();

  /** Base URL for API calls (no trailing slash). Empty if unset. */
  std::string getBaseUrl() const { return serverUrl; }
};

#define INKHOARD_STORE InkHoardCredentialStore::getInstance()

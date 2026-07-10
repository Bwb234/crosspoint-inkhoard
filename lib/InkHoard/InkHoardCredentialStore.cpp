#include "InkHoardCredentialStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

void InkHoardCredentialStore::toJson(JsonDocument& doc) const {
  doc["serverUrl"] = serverUrl.c_str();
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["displayName"] = displayName.c_str();
}

bool InkHoardCredentialStore::fromJson(JsonVariantConst doc) {
  serverUrl = doc["serverUrl"] | "";
  displayName = doc["displayName"] | "";

  bool ok = false;
  token = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", &ok);
  if (!ok) {
    // Legacy plaintext fallback (should not appear in new writes)
    const char* plain = doc["token"] | "";
    token = plain ? plain : "";
    if (!token.empty()) {
      LOG_DBG("INKH", "Migrating plaintext token to obfuscated form");
      saveToFile();
    }
  }

  // Enforce bounds after load without logging secrets
  serverUrl = inkhoard::truncateUtf8(serverUrl, inkhoard::MAX_URL_BYTES);
  token = inkhoard::truncateUtf8(token, inkhoard::MAX_TOKEN_BYTES);
  displayName = inkhoard::truncateUtf8(displayName, inkhoard::MAX_DISPLAY_NAME_BYTES);
  return true;
}

bool InkHoardCredentialStore::setServerUrl(const std::string& url, bool allowHttp) {
  std::string normalized;
  if (!inkhoard::normalizeServerUrl(url, allowHttp, normalized)) {
    LOG_DBG("INKH", "Rejected server URL (scheme or length)");
    return false;
  }
  serverUrl = normalized;
  LOG_DBG("INKH", "Set server URL (%u bytes)", (unsigned)serverUrl.size());
  return true;
}

bool InkHoardCredentialStore::setToken(const std::string& value) {
  if (inkhoard::utf8ByteLength(value) > inkhoard::MAX_TOKEN_BYTES) {
    LOG_DBG("INKH", "Rejected token (too long)");
    return false;
  }
  token = value;
  LOG_DBG("INKH", "Set device token (%s)", getRedactedToken().c_str());
  return true;
}

bool InkHoardCredentialStore::setDisplayName(const std::string& name) {
  displayName = inkhoard::truncateUtf8(name, inkhoard::MAX_DISPLAY_NAME_BYTES);
  return true;
}

void InkHoardCredentialStore::clear() {
  serverUrl.clear();
  token.clear();
  displayName.clear();
  saveToFile();
  LOG_DBG("INKH", "Cleared InkHoard credentials");
}

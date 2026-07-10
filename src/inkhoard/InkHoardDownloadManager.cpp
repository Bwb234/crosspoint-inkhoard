#include "InkHoardDownloadManager.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ctime>
#include <cstring>

namespace {
bool endsWith(const char* name, const char* suffix) {
  if (!name || !suffix) return false;
  const size_t n = std::strlen(name);
  const size_t s = std::strlen(suffix);
  return n >= s && std::strcmp(name + (n - s), suffix) == 0;
}

void copyField(char* dest, size_t cap, const char* src) {
  if (!dest || cap == 0) return;
  if (!src) {
    dest[0] = '\0';
    return;
  }
  std::strncpy(dest, src, cap - 1);
  dest[cap - 1] = '\0';
}

void nowIso(char* buf, size_t cap) {
  if (!buf || cap < 20) return;
  const time_t t = time(nullptr);
  struct tm tmBuf {};
#if defined(_WIN32) && !defined(ARDUINO)
  gmtime_s(&tmBuf, &t);
#else
  gmtime_r(&t, &tmBuf);
#endif
  std::strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tmBuf);
}
}  // namespace

InkHoardDownloadManager::InkHoardDownloadManager(InkHoardClient* client)
    : client_(client), ownedClient_() {
  if (!client_) client_ = &ownedClient_;
}

const char* InkHoardDownloadManager::resultLabel(Result r) {
  switch (r) {
    case Result::Ok:
      return "ok";
    case Result::NotModified:
      return "not_modified";
    case Result::NoCredentials:
      return "no_credentials";
    case Result::BadId:
      return "bad_id";
    case Result::ClientError:
      return "client_error";
    case Result::StorageError:
      return "storage_error";
    case Result::StorageFull:
      return "storage_full";
    case Result::Incomplete:
      return "incomplete";
    case Result::Aborted:
      return "aborted";
  }
  return "unknown";
}

InkHoardDownloadManager::Result InkHoardDownloadManager::mapClient(inkhoard::ClientResult r) const {
  switch (r) {
    case inkhoard::ClientResult::Ok:
      return Result::Ok;
    case inkhoard::ClientResult::NotModified:
      return Result::NotModified;
    case inkhoard::ClientResult::NoCredentials:
      return Result::NoCredentials;
    case inkhoard::ClientResult::Aborted:
      return Result::Aborted;
    case inkhoard::ClientResult::FileError:
      return Result::StorageError;
    default:
      return Result::ClientError;
  }
}

bool InkHoardDownloadManager::prepareStorage() {
  if (!Storage.ensureDirectoryExists(inkhoard::ROOT_DIR)) return false;
  if (!Storage.ensureDirectoryExists(inkhoard::ITEMS_DIR)) return false;
  if (!Storage.ensureDirectoryExists(inkhoard::TMP_DIR)) return false;

  HalFile dir = Storage.open(inkhoard::TMP_DIR);
  if (!dir || !dir.isDirectory()) return true;
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    char name[96] = {};
    entry.getName(name, sizeof(name));
    if (endsWith(name, ".part") || endsWith(name, ".json.part")) {
      std::string path = std::string(inkhoard::TMP_DIR) + "/" + name;
      LOG_DBG("INKH", "cleanup stale part %s", path.c_str());
      Storage.remove(path.c_str());
    }
  }
  return true;
}

void InkHoardDownloadManager::cleanupPart(const char* id) const {
  const auto part = inkhoard::itemPartPath(id);
  const auto sidePart = inkhoard::itemSidecarPartPath(id);
  if (!part.empty()) Storage.remove(part.c_str());
  if (!sidePart.empty()) Storage.remove(sidePart.c_str());
}

bool InkHoardDownloadManager::isDownloaded(const char* id) const {
  inkhoard::Sidecar side;
  return readSidecar(id, side);
}

std::string InkHoardDownloadManager::localPath(const char* id) const {
  if (!isDownloaded(id)) return {};
  return inkhoard::itemEpubPath(id);
}

bool InkHoardDownloadManager::readSidecar(const char* id, inkhoard::Sidecar& out) const {
  out = inkhoard::Sidecar{};
  const auto sidePath = inkhoard::itemSidecarPath(id);
  const auto epubPath = inkhoard::itemEpubPath(id);
  if (sidePath.empty() || epubPath.empty()) return false;
  if (!Storage.exists(sidePath.c_str()) || !Storage.exists(epubPath.c_str())) return false;

  char buf[inkhoard::MAX_SIDECAR_BYTES] = {};
  const size_t n = Storage.readFileToBuffer(sidePath.c_str(), buf, sizeof(buf));
  if (n == 0) return false;
  if (!inkhoard::parseSidecarJson(buf, n, out) || !out.valid) {
    out = inkhoard::Sidecar{};
    return false;
  }
  if (std::strcmp(out.id, id) != 0) {
    out = inkhoard::Sidecar{};
    return false;
  }
  return true;
}

bool InkHoardDownloadManager::writeSidecarPartOnly(const inkhoard::Sidecar& side, std::string& partPathOut) const {
  char json[inkhoard::MAX_SIDECAR_BYTES] = {};
  const size_t n = inkhoard::formatSidecarJson(json, sizeof(json), side);
  if (n == 0) return false;
  partPathOut = inkhoard::itemSidecarPartPath(side.id);
  if (partPathOut.empty()) return false;
  if (!Storage.writeFile(partPathOut.c_str(), String(json))) {
    Storage.remove(partPathOut.c_str());
    partPathOut.clear();
    return false;
  }
  return true;
}

InkHoardDownloadManager::Result InkHoardDownloadManager::download(const DownloadRequest& item,
                                                                  ProgressCallback progress,
                                                                  inkhoard::ClientResult* clientDetail) {
  if (clientDetail) *clientDetail = inkhoard::ClientResult::Ok;
  if (!inkhoard::isSafeItemId(item.id)) return Result::BadId;
  if (!prepareStorage()) return Result::StorageError;
  if (!client_) return Result::ClientError;

  const auto partPath = inkhoard::itemPartPath(item.id);
  const auto epubPath = inkhoard::itemEpubPath(item.id);
  if (partPath.empty() || epubPath.empty()) return Result::BadId;

  cleanupPart(item.id);

  char ifNoneMatch[256] = {};
  inkhoard::Sidecar existing;
  if (readSidecar(item.id, existing) && existing.etag[0]) {
    copyField(ifNoneMatch, sizeof(ifNoneMatch), existing.etag);
  }

  char etagOut[256] = {};
  const auto outcome =
      client_->downloadEpub(item.id, partPath.c_str(), ifNoneMatch[0] ? ifNoneMatch : nullptr, etagOut,
                            sizeof(etagOut), progress);
  if (clientDetail) *clientDetail = outcome.result;

  if (outcome.result == inkhoard::ClientResult::NotModified) {
    cleanupPart(item.id);
    return Result::NotModified;
  }
  if (outcome.result != inkhoard::ClientResult::Ok) {
    cleanupPart(item.id);
    return mapClient(outcome.result);
  }

  HalFile partFile;
  if (!Storage.openFileForRead("INKH", partPath, partFile) || !partFile) {
    cleanupPart(item.id);
    return Result::StorageError;
  }
  const size_t partSize = partFile.size();
  partFile.close();
  if (partSize == 0) {
    cleanupPart(item.id);
    return Result::Incomplete;
  }

  inkhoard::Sidecar side{};
  copyField(side.id, sizeof(side.id), item.id);
  copyField(side.title, sizeof(side.title), item.title);
  copyField(side.contentVersion, sizeof(side.contentVersion), item.contentVersion);
  copyField(side.etag, sizeof(side.etag), etagOut);
  side.size = partSize;
  nowIso(side.downloadedAt, sizeof(side.downloadedAt));
  side.valid = true;

  // Plan order: sidecar → tmp name, rename .part → items/<id>.epub, then sidecar into place.
  // Existing items/<id>.epub + sidecar stay untouched until both renames succeed.
  std::string sidePart;
  if (!writeSidecarPartOnly(side, sidePart)) {
    cleanupPart(item.id);
    return Result::StorageFull;
  }

  const auto finalSide = inkhoard::itemSidecarPath(item.id);
  const bool hadPrior = Storage.exists(epubPath.c_str());
  std::string priorBackup;
  if (hadPrior) {
    priorBackup = std::string(inkhoard::TMP_DIR) + "/" + item.id + ".epub.bak";
    Storage.remove(priorBackup.c_str());
    if (!Storage.rename(epubPath.c_str(), priorBackup.c_str())) {
      cleanupPart(item.id);
      Storage.remove(sidePart.c_str());
      return Result::StorageError;
    }
  }

  if (!Storage.rename(partPath.c_str(), epubPath.c_str())) {
    if (hadPrior && !priorBackup.empty()) {
      Storage.rename(priorBackup.c_str(), epubPath.c_str());
    }
    cleanupPart(item.id);
    Storage.remove(sidePart.c_str());
    return Result::StorageError;
  }

  if (Storage.exists(finalSide.c_str())) {
    Storage.remove(finalSide.c_str());
  }
  if (!Storage.rename(sidePart.c_str(), finalSide.c_str())) {
    Storage.remove(epubPath.c_str());
    if (hadPrior && !priorBackup.empty()) {
      Storage.rename(priorBackup.c_str(), epubPath.c_str());
    }
    cleanupPart(item.id);
    Storage.remove(sidePart.c_str());
    return Result::StorageError;
  }

  if (!priorBackup.empty()) {
    Storage.remove(priorBackup.c_str());
  }

  LOG_DBG("INKH", "downloaded %s (%u bytes)", item.id, (unsigned)partSize);
  return Result::Ok;
}

size_t InkHoardDownloadManager::listDownloaded(std::vector<inkhoard::Sidecar>& out, size_t maxCount) const {
  out.clear();
  if (maxCount == 0) return 0;
  HalFile dir = Storage.open(inkhoard::ITEMS_DIR);
  if (!dir || !dir.isDirectory()) return 0;

  for (HalFile entry = dir.openNextFile(); entry && out.size() < maxCount; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    char name[96] = {};
    entry.getName(name, sizeof(name));
    if (!endsWith(name, ".json")) continue;
    const size_t nameLen = std::strlen(name);
    if (nameLen <= 5) continue;
    char id[inkhoard::MAX_ID_PATH_BYTES + 1] = {};
    const size_t idLen = nameLen - 5;
    if (idLen > inkhoard::MAX_ID_PATH_BYTES) continue;
    std::memcpy(id, name, idLen);
    id[idLen] = '\0';
    inkhoard::Sidecar side;
    if (readSidecar(id, side)) {
      out.push_back(side);
    }
  }
  return out.size();
}

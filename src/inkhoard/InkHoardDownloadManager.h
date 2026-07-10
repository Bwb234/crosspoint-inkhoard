#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "InkHoardClient.h"
#include "InkHoardDownloadPaths.h"

/**
 * Atomic EPUB download + sidecar for /InkHoard/items.
 * INKHOARD: plan 009
 */
class InkHoardDownloadManager {
 public:
  enum class Result : uint8_t {
    Ok = 0,
    NotModified,
    NoCredentials,
    BadId,
    ClientError,
    StorageError,
    StorageFull,
    Incomplete,
    Aborted,
  };

  struct DownloadRequest {
    char id[inkhoard::MAX_ID_PATH_BYTES + 1] = {};
    char title[inkhoard::MAX_TITLE_BYTES + 1] = {};
    char contentVersion[inkhoard::MAX_ISO_BYTES + 1] = {};
  };

  using ProgressCallback = InkHoardClient::ProgressCallback;

  explicit InkHoardDownloadManager(InkHoardClient* client = nullptr);

  void setClient(InkHoardClient* client) { client_ = client; }

  /** Ensure dirs exist; delete stale tmp/*.part (and *.json.part). */
  bool prepareStorage();

  Result download(const DownloadRequest& item, ProgressCallback progress = nullptr,
                  inkhoard::ClientResult* clientDetail = nullptr);

  bool isDownloaded(const char* id) const;
  std::string localPath(const char* id) const;
  bool readSidecar(const char* id, inkhoard::Sidecar& out) const;

  /** Scan items/*.json (bounded). Corrupt sidecars skipped. */
  size_t listDownloaded(std::vector<inkhoard::Sidecar>& out, size_t maxCount = inkhoard::MAX_SCAN_SIDECARS) const;

  static const char* resultLabel(Result r);

 private:
  InkHoardClient* client_ = nullptr;
  InkHoardClient ownedClient_;

  Result mapClient(inkhoard::ClientResult r) const;
  bool writeSidecarPartOnly(const inkhoard::Sidecar& side, std::string& partPathOut) const;
  void cleanupPart(const char* id) const;
};

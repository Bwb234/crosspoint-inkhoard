#pragma once

// FreeInkBook storage adapters — bind the engine's BookSource/CacheStorage
// interfaces to HalStorage (all SD access must go through the HAL mutex).
// Pattern follows freeink-books' BookStorageAdapters.h with CrossPoint's
// torn-write-safe temp+rename commit.

#include <BookStorage.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

// Random access over one book file on the SD card. The file stays open for
// the book's lifetime; every readAt takes the storage mutex via HalFile.
class SdBookSource : public freeink::book::BookSource {
 public:
  bool open(const char* path) {
    if (!Storage.openFileForRead("FIBSRC", path, file_)) return false;
    size_ = file_.fileSize64();
    return size_ > 0;
  }
  void close() { file_.close(); }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (!file_.isOpen() || !file_.seek64(offset)) return -1;
    return file_.read(dst, len);
  }
  uint64_t size() const override { return size_; }

 private:
  HalFile file_;
  uint64_t size_ = 0;
};

// Layout-cache files inside one book's cache directory. Writes stream to a
// temp name and commit via rename, so an interrupted write leaves the old
// file or none — never a torn one (the engine additionally verifies a footer
// magic). Reads keep the last-touched file open: page turns are then one
// seek+read instead of a directory walk per call.
class SdCacheStorage : public freeink::book::CacheStorage {
 public:
  // `dir` is the per-book cache directory (e.g. ".crosspoint/epub_<hash>").
  void setDir(const char* dir) {
    snprintf(dir_, sizeof(dir_), "%s", dir);
    Storage.ensureDirectoryExists(dir_);
    closeRead();
  }

  bool exists(const char* name) override { return Storage.exists(path(name)); }

  bool remove(const char* name) override {
    invalidateRead(name);
    return Storage.remove(path(name));
  }

  int64_t fileSize(const char* name) override {
    if (!ensureReadOpen(name)) return -1;
    return static_cast<int64_t>(readFile_.fileSize64());
  }

  int32_t readAt(const char* name, uint32_t offset, void* dst, uint32_t len) override {
    if (!ensureReadOpen(name) || !readFile_.seekSet(offset)) return -1;
    return readFile_.read(dst, len);
  }

  bool beginWrite(const char* name) override {
    snprintf(commitPath_, sizeof(commitPath_), "%s/%s", dir_, name);
    invalidateRead(name);
    if (!Storage.openFileForWrite("FIBCACHE", path(kTempName), write_)) {
      LOG_ERR("FIBCACHE", "beginWrite failed: %s", commitPath_);
      return false;
    }
    return true;
  }

  bool write(const void* data, uint32_t len) override {
    return write_.isOpen() && write_.write(data, len) == len;
  }

  bool endWrite() override {
    if (!write_.isOpen()) return false;
    write_.close();  // must close before rename (DESTRUCTOR_CLOSES_FILE covers scope exit only)
    Storage.remove(commitPath_);  // may not exist; rename below is the commit point
    if (!Storage.rename(path(kTempName), commitPath_)) {
      LOG_ERR("FIBCACHE", "commit rename failed: %s", commitPath_);
      return false;
    }
    return true;
  }

 private:
  static constexpr const char* kTempName = "_tmp.fibp";

  const char* path(const char* name) {
    snprintf(pathBuf_, sizeof(pathBuf_), "%s/%s", dir_, name);
    return pathBuf_;
  }

  bool ensureReadOpen(const char* name) {
    if (readFile_.isOpen() && strncmp(readName_, name, sizeof(readName_)) == 0) return true;
    closeRead();
    if (!Storage.openFileForRead("FIBCACHE", path(name), readFile_)) return false;
    snprintf(readName_, sizeof(readName_), "%s", name);
    return true;
  }

  void invalidateRead(const char* name) {
    if (readFile_.isOpen() && strncmp(readName_, name, sizeof(readName_)) == 0) closeRead();
  }

  void closeRead() {
    if (readFile_.isOpen()) readFile_.close();
    readName_[0] = '\0';
  }

  char dir_[96] = "";
  char pathBuf_[192];
  char commitPath_[192];
  char readName_[80] = "";
  HalFile readFile_;
  HalFile write_;
};

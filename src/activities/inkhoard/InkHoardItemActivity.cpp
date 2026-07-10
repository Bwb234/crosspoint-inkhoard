#include "InkHoardItemActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <cstring>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

void InkHoardItemActivity::refreshDownloaded() { downloaded = downloads.isDownloaded(item_.id); }

int InkHoardItemActivity::actionCount() const {
  // Download/Retry always; Open when downloaded
  return downloaded ? 2 : 1;
}

void InkHoardItemActivity::onEnter() {
  Activity::onEnter();
  downloads.prepareStorage();
  refreshDownloaded();
  actionIndex = downloaded ? 1 : 0;
  state = State::READY;
  errorMessage.clear();
  failure = inkhoard::UiFailureKind::None;
  downloadProgress = downloadTotal = 0;
  openPending = false;
  wifiActivated = WiFi.status() == WL_CONNECTED;
  requestUpdate();
}

void InkHoardItemActivity::onExit() {
  Activity::onExit();
  if (openPending) {
    // Reader hand-off: tear down Wi-Fi then silent restart into reader.
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(false);
      delay(30);
    }
    silentRestartToReader();
    return;
  }
  // Returning to library: leave Wi-Fi up; library onExit handles teardown.
}

void InkHoardItemActivity::startDownload() {
  state = State::DOWNLOADING;
  downloadProgress = downloadTotal = 0;
  errorMessage.clear();
  requestUpdate(true);

  InkHoardDownloadManager::DownloadRequest req{};
  std::strncpy(req.id, item_.id, sizeof(req.id) - 1);
  if (!item_.titleIsNull) std::strncpy(req.title, item_.title, sizeof(req.title) - 1);
  std::strncpy(req.contentVersion, item_.contentVersion, sizeof(req.contentVersion) - 1);

  inkhoard::ClientResult detail = inkhoard::ClientResult::Ok;
  const auto result = downloads.download(
      req,
      [this](size_t downloadedBytes, size_t total) {
        downloadProgress = downloadedBytes;
        downloadTotal = total;
        requestUpdate(true);
      },
      &detail);

  refreshDownloaded();
  if (result == InkHoardDownloadManager::Result::Ok || result == InkHoardDownloadManager::Result::NotModified) {
    state = State::READY;
    actionIndex = 1;
    requestUpdate();
    return;
  }

  state = State::ERROR;
  failure = inkhoard::mapClientToUiFailure(detail);
  switch (result) {
    case InkHoardDownloadManager::Result::NoCredentials:
      errorMessage = tr(STR_INKHOARD_NO_CREDENTIALS);
      failure = inkhoard::UiFailureKind::NoCredentials;
      break;
    case InkHoardDownloadManager::Result::StorageFull:
      errorMessage = tr(STR_INKHOARD_STORAGE_FULL);
      failure = inkhoard::UiFailureKind::SdError;
      break;
    case InkHoardDownloadManager::Result::StorageError:
    case InkHoardDownloadManager::Result::Incomplete:
      errorMessage = tr(STR_INKHOARD_STORAGE_ERROR);
      failure = inkhoard::UiFailureKind::SdError;
      break;
    case InkHoardDownloadManager::Result::Aborted:
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      break;
    default:
      if (detail == inkhoard::ClientResult::AuthInvalid) {
        errorMessage = tr(STR_INKHOARD_TOKEN_REJECTED);
      } else if (detail == inkhoard::ClientResult::AuthForbidden) {
        errorMessage = tr(STR_INKHOARD_TOKEN_NO_ACCESS);
      } else if (detail == inkhoard::ClientResult::ContentNotReady) {
        errorMessage = tr(STR_INKHOARD_CONTENT_NOT_READY);
      } else if (detail == inkhoard::ClientResult::ContentFailed) {
        errorMessage = tr(STR_INKHOARD_CONTENT_FAILED);
      } else if (detail == inkhoard::ClientResult::NotFound) {
        errorMessage = tr(STR_INKHOARD_CONTENT_UNAVAILABLE);
      } else {
        errorMessage = tr(STR_DOWNLOAD_FAILED);
      }
      break;
  }
  requestUpdate();
}

void InkHoardItemActivity::openLocal() {
  const auto path = downloads.localPath(item_.id);
  if (path.empty()) {
    state = State::ERROR;
    errorMessage = tr(STR_INKHOARD_STORAGE_ERROR);
    requestUpdate();
    return;
  }
  state = State::OPENING;
  requestUpdate(true);

  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  openPending = true;
  finish();  // onExit performs Wi-Fi teardown + silentRestartToReader
}

void InkHoardItemActivity::loop() {
  if (state == State::DOWNLOADING || state == State::OPENING) return;

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (failure == inkhoard::UiFailureKind::Auth || failure == inkhoard::UiFailureKind::Forbidden ||
          failure == inkhoard::UiFailureKind::NoCredentials) {
        activityManager.goToSettings();
      } else {
        startDownload();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  // READY
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (actionIndex == 0 || !downloaded) {
      startDownload();
    } else {
      openLocal();
    }
    return;
  }

  const int n = actionCount();
  buttonNavigator.onNextRelease([this, n] {
    actionIndex = ButtonNavigator::nextIndex(actionIndex, n);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, n] {
    actionIndex = ButtonNavigator::previousIndex(actionIndex, n);
    requestUpdate();
  });
}

void InkHoardItemActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const char* title = (!item_.titleIsNull && item_.title[0]) ? item_.title : item_.id;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_INKHOARD));

  auto trunc = renderer.truncatedText(UI_10_FONT_ID, title, pageWidth - 40);
  renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + 20, trunc.c_str(), true,
                            EpdFontFamily::BOLD);

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_DOWNLOADING));
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 10, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == State::OPENING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, errorMessage.c_str());
    const char* confirm =
        (failure == inkhoard::UiFailureKind::Auth || failure == inkhoard::UiFailureKind::Forbidden ||
         failure == inkhoard::UiFailureKind::NoCredentials)
            ? tr(STR_SETTINGS_TITLE)
            : tr(STR_RETRY);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Actions
  const int top = pageHeight / 2 - 20;
  const char* dlLabel = downloaded ? tr(STR_INKHOARD_REDOWNLOAD) : tr(STR_DOWNLOAD);
  renderer.fillRect(20, top + actionIndex * 36 - 4, pageWidth - 40, 32);
  renderer.drawText(UI_10_FONT_ID, 40, top, dlLabel, actionIndex != 0);
  if (downloaded) {
    renderer.drawText(UI_10_FONT_ID, 40, top + 36, tr(STR_OPEN), actionIndex != 1);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

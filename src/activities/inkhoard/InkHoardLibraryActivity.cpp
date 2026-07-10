#include "InkHoardLibraryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "InkHoardCredentialStore.h"
#include "InkHoardItemActivity.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int PAGE_ITEMS = 20;
}

void InkHoardLibraryActivity::mapError(inkhoard::ClientResult r) {
  failure = inkhoard::mapClientToUiFailure(r);
  switch (failure) {
    case inkhoard::UiFailureKind::NoCredentials:
      errorMessage = tr(STR_INKHOARD_NO_CREDENTIALS);
      break;
    case inkhoard::UiFailureKind::Auth:
      errorMessage = tr(STR_INKHOARD_TOKEN_REJECTED);
      break;
    case inkhoard::UiFailureKind::Forbidden:
      errorMessage = tr(STR_INKHOARD_TOKEN_NO_ACCESS);
      break;
    case inkhoard::UiFailureKind::Transport:
      errorMessage = tr(STR_INKHOARD_UNREACHABLE);
      break;
    case inkhoard::UiFailureKind::Server:
      errorMessage = tr(STR_INKHOARD_SERVER_ERROR);
      break;
    case inkhoard::UiFailureKind::Content:
      errorMessage = tr(STR_INKHOARD_CONTENT_UNAVAILABLE);
      break;
    case inkhoard::UiFailureKind::SdError:
      errorMessage = tr(STR_INKHOARD_STORAGE_ERROR);
      break;
    default:
      errorMessage = inkhoard::clientResultLabel(r);
      break;
  }
}

void InkHoardLibraryActivity::onEnter() {
  Activity::onEnter();
  downloads.prepareStorage();
  cursors.reset();
  page.reset();
  offlineItems.clear();
  selectorIndex = 0;
  errorMessage.clear();
  failure = inkhoard::UiFailureKind::None;
  wifiActivated = false;
  checkCredentialsAndWifi();
}

void InkHoardLibraryActivity::onExit() {
  Activity::onExit();
  page.reset();
  offlineItems.clear();
  if (wifiActivated && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void InkHoardLibraryActivity::checkCredentialsAndWifi() {
  if (!INKHOARD_STORE.hasCredentials()) {
    state = State::NO_CREDENTIALS;
    statusMessage = tr(STR_INKHOARD_NO_CREDENTIALS);
    requestUpdate();
    return;
  }

  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    wifiActivated = true;
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchPage();
    return;
  }
  launchWifiSelection();
}

void InkHoardLibraryActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void InkHoardLibraryActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    wifiActivated = true;
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchPage();
  } else {
    // Offline path: show downloaded items if any
    loadOfflineList();
  }
}

void InkHoardLibraryActivity::fetchPage() {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);

  if (!page) page = std::make_unique<inkhoard::LibraryPage>();
  if (!page) {
    mapError(inkhoard::ClientResult::LowMemory);
    state = State::ERROR;
    requestUpdate();
    return;
  }
  *page = {};
  const auto outcome = client.fetchLibraryPage(*page, cursors.currentCursor(), PAGE_ITEMS);
  if (outcome.result != inkhoard::ClientResult::Ok || !page->valid) {
    mapError(outcome.result);
    // Offer offline list when transport fails
    if (failure == inkhoard::UiFailureKind::Transport || failure == inkhoard::UiFailureKind::Server) {
      loadOfflineList();
      if (state == State::OFFLINE_LIST) return;
    }
    state = State::ERROR;
    requestUpdate();
    return;
  }

  selectorIndex = 0;
  state = State::BROWSING;
  requestUpdate();
}

void InkHoardLibraryActivity::loadOfflineList() {
  offlineItems.clear();
  downloads.listDownloaded(offlineItems);
  if (offlineItems.empty()) {
    state = State::ERROR;
    if (errorMessage.empty()) errorMessage = tr(STR_INKHOARD_NO_DOWNLOADS);
    failure = inkhoard::UiFailureKind::Transport;
    requestUpdate();
    return;
  }
  selectorIndex = 0;
  state = State::OFFLINE_LIST;
  requestUpdate();
}

void InkHoardLibraryActivity::openSelected() {
  inkhoard::CompactItem item{};
  if (state == State::BROWSING) {
    if (!page || page->itemCount == 0 || selectorIndex < 0 || selectorIndex >= page->itemCount) return;
    item = page->items[selectorIndex];
  } else if (state == State::OFFLINE_LIST) {
    if (offlineItems.empty() || selectorIndex < 0 ||
        static_cast<size_t>(selectorIndex) >= offlineItems.size()) {
      return;
    }
    const auto& side = offlineItems[static_cast<size_t>(selectorIndex)];
    std::strncpy(item.id, side.id, sizeof(item.id) - 1);
    std::strncpy(item.title, side.title, sizeof(item.title) - 1);
    item.titleIsNull = side.title[0] == '\0';
    std::strncpy(item.contentVersion, side.contentVersion, sizeof(item.contentVersion) - 1);
    item.epubAvailable = true;
    item.valid = true;
  } else {
    return;
  }

  startActivityForResult(std::make_unique<InkHoardItemActivity>(renderer, mappedInput, item),
                         [this](const ActivityResult&) {
                           // Refresh download markers after return
                           if (state == State::OFFLINE_LIST) loadOfflineList();
                           requestUpdate();
                         });
}

void InkHoardLibraryActivity::goNextPage() {
  if (!page || !page->hasNextCursor || !page->nextCursor[0]) return;
  cursors.pushNext(page->nextCursor);
  fetchPage();
}

void InkHoardLibraryActivity::goPrevPage() {
  if (!cursors.popPrev()) return;
  fetchPage();
}

void InkHoardLibraryActivity::loop() {
  if (state == State::WIFI_SELECTION) return;

  if (state == State::NO_CREDENTIALS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activityManager.goToSettings();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome(HomeMenuItem::INKHOARD);
    }
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (failure == inkhoard::UiFailureKind::Auth || failure == inkhoard::UiFailureKind::Forbidden ||
          failure == inkhoard::UiFailureKind::NoCredentials) {
        activityManager.goToSettings();
      } else if (WiFi.status() == WL_CONNECTED) {
        cursors.reset();
        fetchPage();
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      loadOfflineList();
      if (state == State::ERROR) onGoHome(HomeMenuItem::INKHOARD);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      loadOfflineList();
    }
    return;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome(HomeMenuItem::INKHOARD);
    }
    return;
  }

  if (state == State::BROWSING || state == State::OFFLINE_LIST) {
    const int count = state == State::BROWSING ? (page ? static_cast<int>(page->itemCount) : 0)
                                               : static_cast<int>(offlineItems.size());

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openSelected();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome(HomeMenuItem::INKHOARD);
      return;
    }
    if (state == State::BROWSING) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Left) && cursors.canGoPrev()) {
        goPrevPage();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Right) && page && page->hasNextCursor) {
        goNextPage();
        return;
      }
    }

    if (count > 0) {
      buttonNavigator.onNextRelease([this, count] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this, count] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this, count] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, count, PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this, count] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, count, PAGE_ITEMS);
        requestUpdate();
      });
    }
  }
}

void InkHoardLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_INKHOARD));

  if (state == State::NO_CREDENTIALS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, statusMessage.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, tr(STR_INKHOARD_GO_SETTINGS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SETTINGS_TITLE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING || state == State::WIFI_SELECTION) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const char* confirm =
        (failure == inkhoard::UiFailureKind::Auth || failure == inkhoard::UiFailureKind::Forbidden ||
         failure == inkhoard::UiFailureKind::NoCredentials)
            ? tr(STR_SETTINGS_TITLE)
            : tr(STR_RETRY);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_INKHOARD_DOWNLOADED), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const bool offline = state == State::OFFLINE_LIST;
  const int count = offline ? static_cast<int>(offlineItems.size()) : (page ? static_cast<int>(page->itemCount) : 0);
  const char* confirmLabel = tr(STR_OPEN);
  const char* pageLabel = "";
  if (!offline) {
    if (cursors.canGoPrev())
      pageLabel = tr(STR_PREV_PAGE);
    else if (page && page->hasNextCursor)
      pageLabel = tr(STR_NEXT_PAGE);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + 4,
                              tr(STR_INKHOARD_DOWNLOADED));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, pageLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (count == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const int listTop = metrics.topPadding + metrics.headerHeight + (offline ? 24 : 8);
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, listTop + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    for (int i = pageStartIndex; i < count && i < pageStartIndex + PAGE_ITEMS; i++) {
      std::string displayText;
      bool isDl = false;
      if (offline) {
        displayText = offlineItems[static_cast<size_t>(i)].title;
        if (displayText.empty()) displayText = offlineItems[static_cast<size_t>(i)].id;
        isDl = true;
      } else {
        const auto& it = page->items[i];
        displayText = (!it.titleIsNull && it.title[0]) ? it.title : it.id;
        isDl = downloads.isDownloaded(it.id);
      }
      if (isDl) displayText = std::string("* ") + displayText;
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, listTop + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
    }
  }
  renderer.displayBuffer();
}

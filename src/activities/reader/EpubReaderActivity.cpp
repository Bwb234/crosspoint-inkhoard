#include "EpubReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <limits>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "FreeInkPageRenderer.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

std::string cacheDirForBook(const std::string& path) {
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path));
}

// Display fallback when the OPF carries no title: the bare filename.
std::string filenameTitle(const std::string& path) {
  const size_t slash = path.rfind('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.rfind('.');
  const size_t end = (dot == std::string::npos || dot <= start) ? path.size() : dot;
  return path.substr(start, end - start);
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path, so it must be re-keyed.
  const std::string newCachePath = cacheDirForBook(dstPath);
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

// Synthetic KOReader-style xpath for a chapter (1-based DocFragment index).
// Percentage is the primary sync mechanism; the fragment index carries the
// chapter for display and same-book sanity checks.
std::string syntheticXPath(const int spineIndex) {
  return "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body/p[1]/text().0";
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  cacheDir_ = cacheDirForBook(path_);
  Storage.ensureDirectoryExists("/.crosspoint");

  if (!paginator.open(path_, cacheDir_, renderer)) {
    LOG_ERR("ERS", "Failed to open book: %s", path_.c_str());
    activityManager.goToFullScreenMessage(tr(STR_PAGE_LOAD_ERROR), EpdFontFamily::BOLD);
    return;
  }

  const auto progress = EpubReaderUtils::loadProgress(cacheDir_);
  if (progress.valid) {
    currentSpineIndex = progress.spineIndex;
    if (progress.charStart != EpubReaderUtils::kNoCharStart) {
      pendingCharStart = progress.charStart;
    } else {
      pendingChapterFraction = static_cast<float>(progress.fractionQ16) / 65536.0f;
    }
  }

  // Save current epub as last opened epub and add to recent books.
  APP_STATE.openEpubPath = path_;
  APP_STATE.saveToFile();
  std::string title = paginator.title();
  if (title.empty()) {
    title = filenameTitle(path_);
  }
  RECENT_BOOKS.addBook(path_, title, paginator.author(), cacheDir_ + "/thumb_[HEIGHT].bmp");

  loadCachedBookmarks();
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0) {
    const SavedPosition& origin = savedPositions[0];
    EpubReaderUtils::saveProgress(cacheDir_, static_cast<uint16_t>(origin.spineIndex), origin.charStart);
  }

  paginator.close();
  if (pendingReadFolderMove) {
    const std::string dstPath = buildReadFolderDestination(path_);
    moveFinishedBookToReadFolder(path_, dstPath, cacheDir_);
  }
}

float EpubReaderActivity::currentBookFraction() const {
  if (!paginator.chapterReady() || paginator.totalChars() == 0) {
    return paginator.bookProgress(currentSpineIndex, 0.0f);
  }
  const float chapterFraction = static_cast<float>(lastCharStart) / static_cast<float>(paginator.totalChars());
  return paginator.bookProgress(currentSpineIndex, chapterFraction);
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPageDisplay = paginator.chapterReady() ? static_cast<int>(currentPage) + 1 : 0;
  const int totalPages = paginator.chapterReady() ? static_cast<int>(paginator.pageCount()) : 0;
  const int bookProgressPercent = clampPercent(static_cast<int>(currentBookFraction() * 100.0f + 0.5f));
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, paginator.title(), currentPageDisplay, totalPages,
                                               bookProgressPercent, SETTINGS.orientation, !currentPageFootnotes.empty(),
                                               !cachedBookmarks.empty()),
      [this](const ActivityResult& result) {
        // Always apply orientation change even if the menu was cancelled
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

void EpubReaderActivity::loop() {
  if (!paginator.isOpen()) {
    finish();
    return;
  }

  const int spineCount = static_cast<int>(paginator.spineCount());
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= spineCount;

  // Drop this book from the Recent Books list at End-of-Book; re-add if the
  // reader pages back in. Acts only on transitions — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(path_);
    } else if (!atEndOfBook && recentsEntryRemoved) {
      std::string title = paginator.title();
      if (title.empty()) {
        title = filenameTitle(path_);
      }
      RECENT_BOOKS.addBook(path_, title, paginator.author(), cacheDir_ + "/thumb_[HEIGHT].bmp");
      recentsEntryRemoved = false;
    }
  }

  // Arm the move so ANY exit path relocates the finished book into /Read/;
  // paging back off the end screen disarms it.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(path_);
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      requestUpdate();  // updates chapter title space to indicate page turn disabled
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation input.
  if (atEndOfBook && endOfBookOptions.menuActive() &&
      !(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(spineCount - 1, 0);
        pendingLastPage = true;
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Enter reader menu activity on short-press Confirm.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      openReaderMenu();
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(path_);
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book with no suggestion menu, forward goes home; back returns to last page.
  if (atEndOfBook) {
    if (endOfBookOptions.menuActive()) {
      return;  // selection movement handled above; absorb leftover page-turn triggers
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = spineCount - 1;
      pendingLastPage = true;
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && currentPage > 0) {
      currentPage = 0;
      requestUpdate();
      return;
    }
    if (nextTriggered) {
      currentSpineIndex++;
    } else if (currentSpineIndex > 0) {
      currentSpineIndex--;
    }
    currentPage = 0;
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  pageTurn(nextTriggered);
}

void EpubReaderActivity::jumpToPercent(int percent) {
  percent = clampPercent(percent);
  float chapterFraction = 0.0f;
  const int spine = paginator.spineForBookFraction(static_cast<float>(percent) / 100.0f, &chapterFraction);
  currentSpineIndex = spine;
  pendingCharStart.reset();
  pendingChapterFraction = chapterFraction;
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      currentSpineIndex = sync.spineIndex;
      pendingChapterFraction.reset();
      pendingCharStart.reset();
      if (sync.hasCharStart) {
        pendingCharStart = sync.charStart;  // exact locator
      } else if (sync.hasSavedProgress) {
        // Legacy bookmark: percentage is whole-book; land inside its chapter.
        float chapterFraction = 0.0f;
        currentSpineIndex = paginator.spineForBookFraction(sync.percentage, &chapterFraction);
        pendingChapterFraction = chapterFraction;
      } else if (sync.totalPages > 0) {
        pendingChapterFraction = static_cast<float>(sync.page) / static_cast<float>(sync.totalPages);
      }
      requestUpdate();
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, paginator, currentSpineIndex),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              currentSpineIndex = chapterResult.spineIndex;
              pendingAnchor = chapterResult.anchor;
              pendingCharStart.reset();
              pendingChapterFraction.reset();
              currentPage = 0;
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = clampPercent(static_cast<int>(currentBookFraction() * 100.0f + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      std::string fullText = currentPageText();
      if (!fullText.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                               [](const ActivityResult&) {});
      } else {
        requestUpdate();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        chapterOpen = false;
        Storage.removeDir(cacheDir_.c_str());
        Storage.ensureDirectoryExists(cacheDir_.c_str());
        // Preserve the reading position across the wipe.
        EpubReaderUtils::saveProgress(cacheDir_, static_cast<uint16_t>(currentSpineIndex), lastCharStart);
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, paginator, path_),
                             progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int totalPages = paginator.chapterReady() ? static_cast<int>(paginator.pageCount()) : 0;

  // Pre-compute the local KOReader position and chapter name while the book
  // is still open. The synthetic xpath carries the chapter; the whole-book
  // percentage is the primary sync mechanism.
  SavedProgressPosition localKoPos{syntheticXPath(currentSpineIndex), currentBookFraction()};
  const int tocIdx = paginator.tocIndexForSpine(currentSpineIndex);
  std::string localChapterName = tocIdx >= 0 ? paginator.tocItem(tocIdx).title : "";

  // Persist current position so the reader resumes at the right page on return.
  if (!EpubReaderUtils::saveProgress(cacheDir_, static_cast<uint16_t>(currentSpineIndex), lastCharStart)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release the book to free RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing book for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    chapterOpen = false;
    paginator.close();
  }
  LOG_DBG("KOSync", "Book released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, path_, currentSpineIndex, static_cast<int>(currentPage), totalPages, std::move(localKoPos),
      std::move(localChapterName), std::nullopt));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) {
    return;
  }
  {
    RenderLock lock(*this);
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    // The generation changes with the page dimensions; render() reanchors on
    // lastCharStart automatically.
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;
  // The auto-turn indicator reserves status bar space; the viewport change
  // shows up in the generation hash and re-paginates on the next render.
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (paginator.chapterReady() && currentPage + 1 < paginator.pageCount()) {
      currentPage++;
    } else {
      currentSpineIndex++;  // spineCount == end-of-book screen
      currentPage = 0;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
    } else if (currentSpineIndex > 0) {
      currentSpineIndex--;
      pendingLastPage = true;
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

bool EpubReaderActivity::ensureChapterAndPosition() {
  const uint32_t gen = paginator.generation();
  if (!chapterOpen || paginator.currentSpine() != currentSpineIndex || openGeneration != gen) {
    // Settings/orientation changed under the same chapter: reanchor on the
    // page we were showing unless an explicit jump is already pending.
    if (chapterOpen && paginator.currentSpine() == currentSpineIndex && openGeneration != gen &&
        !pendingCharStart.has_value() && !pendingChapterFraction.has_value() && pendingAnchor.empty() &&
        !pendingLastPage) {
      pendingCharStart = lastCharStart;
    }
    chapterOpen = false;
    buildPopupShown = false;

    BookPaginator::BuildProgress progressCb;
    progressCb.ctx = this;
    progressCb.fn = [](void* ctx, uint32_t) {
      auto* self = static_cast<EpubReaderActivity*>(ctx);
      if (!self->buildPopupShown) {
        GUI.drawPopup(self->renderer, tr(STR_INDEXING));
        // HALF-clear the popup when the page replaces it, else it ghosts.
        self->pagesUntilFullRefresh = 1;
        self->buildPopupShown = true;
      }
    };

    const auto status = paginator.ensureChapter(static_cast<uint16_t>(currentSpineIndex), progressCb);
    if (status != freeink::book::BookStatus::Ok) {
      LOG_ERR("ERS", "ensureChapter(%d) failed: %d", currentSpineIndex, static_cast<int>(status));
      return false;
    }
    chapterOpen = true;
    openGeneration = gen;
  }

  // Resolve the landing page for any pending jump.
  if (!pendingAnchor.empty()) {
    uint32_t anchorChar = 0;
    if (paginator.charForAnchor(pendingAnchor.c_str(), &anchorChar)) {
      currentPage = paginator.pageForChar(anchorChar);
      LOG_DBG("ERS", "Resolved anchor '%s' to page %u", pendingAnchor.c_str(), currentPage);
    } else {
      LOG_DBG("ERS", "Anchor '%s' not found in spine %d", pendingAnchor.c_str(), currentSpineIndex);
      currentPage = 0;
    }
    pendingAnchor.clear();
    pendingCharStart.reset();
    pendingChapterFraction.reset();
    pendingLastPage = false;
  } else if (pendingCharStart.has_value()) {
    currentPage = paginator.pageForChar(*pendingCharStart);
    pendingCharStart.reset();
    pendingChapterFraction.reset();
    pendingLastPage = false;
  } else if (pendingChapterFraction.has_value()) {
    const uint32_t targetChar =
        static_cast<uint32_t>(*pendingChapterFraction * static_cast<float>(paginator.totalChars()));
    currentPage = paginator.pageForChar(targetChar);
    pendingChapterFraction.reset();
    pendingLastPage = false;
  } else if (pendingLastPage) {
    currentPage = paginator.pageCount() > 0 ? paginator.pageCount() - 1 : 0;
    pendingLastPage = false;
  }

  if (paginator.pageCount() > 0 && currentPage >= paginator.pageCount()) {
    currentPage = paginator.pageCount() - 1;
  }
  return true;
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!paginator.isOpen()) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const int spineCount = static_cast<int>(paginator.spineCount());
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  if (currentSpineIndex > spineCount) {
    currentSpineIndex = spineCount;
  }

  // Show end of book screen
  if (currentSpineIndex == spineCount) {
    endOfBookOptions.loadOnce(path_);
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  // The engine lays out inside [margin..page-margin]; run coordinates arrive
  // in absolute screen space, so no offset is applied at draw time.
  paginator.configureLayout(static_cast<int16_t>(renderer.getScreenWidth()),
                            static_cast<int16_t>(renderer.getScreenHeight()), static_cast<int16_t>(orientedMarginLeft),
                            static_cast<int16_t>(orientedMarginRight), static_cast<int16_t>(orientedMarginTop),
                            static_cast<int16_t>(orientedMarginBottom));

  if (!ensureChapterAndPosition()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  renderer.clearScreen();

  if (paginator.pageCount() == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  freeink::book::Page page{};
  const auto readStatus = paginator.readPage(currentPage, &page);
  if (readStatus != freeink::book::BookStatus::Ok) {
    LOG_ERR("ERS", "Failed to read page %u (%d) - clearing chapter cache", currentPage, static_cast<int>(readStatus));
    chapterOpen = false;
    requestUpdate();  // rebuild on the next pass
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  lastCharStart = page.charStart;
  currentPageFootnotes = FreeInkPageRenderer::collectFootnotes(page);
  updateBookmarkFlag();

  const auto start = millis();
  renderPage(page, 0);
  LOG_DBG("ERS", "Rendered page in %dms", millis() - start);

  EpubReaderUtils::saveProgress(cacheDir_, static_cast<uint16_t>(currentSpineIndex), lastCharStart);

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
}

void EpubReaderActivity::renderPage(const freeink::book::Page& page, int) {
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  FreeInkPageRenderer::drawPage(renderer, paginator, page, cacheDir_);  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages =
      FreeInkPageRenderer::hasImages(page) && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  auto renderGrayscalePass = [&]() { FreeInkPageRenderer::drawPage(renderer, paginator, page, cacheDir_); };

  FreeInkPageRenderer::drawPage(renderer, paginator, page, cacheDir_);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // blank only the image area, fast refresh, re-render, fast refresh again.
    int16_t imgX, imgY, imgW, imgH;
    if (FreeInkPageRenderer::imageBoundingBox(page, &imgX, &imgY, &imgW, &imgH)) {
      renderer.fillRect(imgX, imgY, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      FreeInkPageRenderer::drawPage(renderer, paginator, page, cacheDir_);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Grayscale leaves residue a plain fast diff can't clear (#2190): force
    // the next ordinary page onto the HALF ghost-cleanup path.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    // Tiled grayscale: render each plane band-by-band into a small scratch.
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      renderer.cleanupGrayscaleWithFrameBuffer();
      LOG_DBG("ERS", "Page render (tiled): prewarm=%lums bw=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, millis() - t0);
    }
  } else if (needsAnyGrayscale) {
    // Fallback for controllers without strip support: full-frame plane swaps.
    if (!renderer.storeBwBuffer()) {
      LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
      return;
    }
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderGrayscalePass();
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderGrayscalePass();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.restoreBwBuffer();
    LOG_DBG("ERS", "Page render: prewarm=%lums bw=%lums display=%lums total=%lums", tPrewarm - t0, tBwRender - tPrewarm,
            tDisplay - tBwRender, millis() - t0);
  } else {
    LOG_DBG("ERS", "Page render: prewarm=%lums bw=%lums display=%lums total=%lums", tPrewarm - t0, tBwRender - tPrewarm,
            tDisplay - tBwRender, millis() - t0);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPageDisplay = static_cast<int>(currentPage) + 1;
  const float pageCount = paginator.chapterReady() ? static_cast<float>(paginator.pageCount()) : 0.0f;
  const float bookProgress = currentBookFraction() * 100.0f;

  std::string title;
  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = paginator.tocIndexForSpine(currentSpineIndex);
    if (tocIndex != -1) {
      title = paginator.tocItem(tocIndex).title;
    }
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = paginator.title();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPageDisplay, pageCount, title, 0, textYOffset, true,
                    currentPageBookmarked, false);
}

std::string EpubReaderActivity::currentPageText() {
  RenderLock lock(*this);
  if (!paginator.chapterReady()) return "";
  freeink::book::Page page{};
  if (paginator.readPage(currentPage, &page) != freeink::book::BookStatus::Ok) return "";
  return FreeInkPageRenderer::pageText(page);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  // Push current position onto saved stack
  if (savePosition && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, lastCharStart};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, char %u", footnoteDepth, currentSpineIndex,
            static_cast<unsigned>(lastCharStart));
  }

  // Split "path#fragment" (link targets arrive container-resolved from the
  // page records; a bare "#frag" targets the current chapter).
  std::string target = hrefStr;
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos) {
    anchor = hrefStr.substr(hashPos + 1);
    target = hrefStr.substr(0, hashPos);
  }

  int targetSpineIndex = currentSpineIndex;
  if (!target.empty()) {
    targetSpineIndex = paginator.spineIndexForHref(target.c_str());
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  currentSpineIndex = targetSpineIndex;
  pendingAnchor = std::move(anchor);
  pendingCharStart.reset();
  pendingChapterFraction.reset();
  currentPage = 0;
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, char %u", footnoteDepth, pos.spineIndex,
          static_cast<unsigned>(pos.charStart));
  currentSpineIndex = pos.spineIndex;
  pendingCharStart = pos.charStart;
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }

  const std::string bmPath = BookmarkUtil::getBookmarkPath(path_);
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

// True when `b` points inside the page currently shown: exact char-range
// containment for engine-era bookmarks, book-percentage proximity for legacy
// entries written by the old engine.
static bool bookmarkOnCurrentPage(const BookmarkEntry& b, const int spineIndex, const uint32_t pageStartChar,
                                  const uint32_t pageEndChar, const float pageStartPct, const float pageEndPct) {
  if (b.hasCharStart) {
    return b.computedSpineIndex == spineIndex && b.charStart >= pageStartChar && b.charStart < pageEndChar;
  }
  constexpr float kEpsilon = 0.0001f;
  const float pct = std::clamp(b.percentage, 0.0f, 1.0f);
  return pct + kEpsilon >= pageStartPct && pct - kEpsilon <= pageEndPct;
}

void EpubReaderActivity::addBookmark() {
  if (!paginator.chapterReady()) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, char %u", currentSpineIndex, static_cast<unsigned>(lastCharStart));

  const uint32_t totalChars = paginator.totalChars();
  const uint32_t pageStartChar = paginator.charStartOfPage(currentPage);
  const uint32_t pageEndChar =
      currentPage + 1 < paginator.pageCount() ? paginator.charStartOfPage(currentPage + 1) : totalChars + 1;
  const float pageStartPct =
      paginator.bookProgress(currentSpineIndex, totalChars ? static_cast<float>(pageStartChar) / totalChars : 0.0f);
  const float pageEndPct =
      paginator.bookProgress(currentSpineIndex, totalChars ? static_cast<float>(pageEndChar) / totalChars : 0.0f);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkOnCurrentPage(b, currentSpineIndex, pageStartChar, pageEndChar,
                                                                      pageStartPct, pageEndPct);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    BookmarkEntry entry;
    entry.percentage = currentBookFraction();
    entry.xpath = syntheticXPath(currentSpineIndex);
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(currentPageText());
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = static_cast<uint16_t>(std::min<uint32_t>(paginator.pageCount(), UINT16_MAX));
    entry.computedChapterProgress = static_cast<uint16_t>(std::min<uint32_t>(currentPage, UINT16_MAX));
    entry.charStart = lastCharStart;
    entry.hasCharStart = true;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(path_);
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  if (!JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str())) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!paginator.chapterReady() || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const uint32_t totalChars = paginator.totalChars();
  const uint32_t pageStartChar = paginator.charStartOfPage(currentPage);
  const uint32_t pageEndChar =
      currentPage + 1 < paginator.pageCount() ? paginator.charStartOfPage(currentPage + 1) : totalChars + 1;
  const float pageStartPct =
      paginator.bookProgress(currentSpineIndex, totalChars ? static_cast<float>(pageStartChar) / totalChars : 0.0f);
  const float pageEndPct =
      paginator.bookProgress(currentSpineIndex, totalChars ? static_cast<float>(pageEndChar) / totalChars : 0.0f);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkOnCurrentPage(b, currentSpineIndex, pageStartChar, pageEndChar, pageStartPct, pageEndPct);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  snprintf(info.title, sizeof(info.title), "%s", paginator.title());
  info.spineIndex = currentSpineIndex;
  if (paginator.chapterReady()) {
    info.currentPage = static_cast<int>(currentPage) + 1;
    info.totalPages = static_cast<int>(paginator.pageCount());
    info.progressPercent = clampPercent(static_cast<int>(currentBookFraction() * 100.0f + 0.5f));
  }
  return info;
}

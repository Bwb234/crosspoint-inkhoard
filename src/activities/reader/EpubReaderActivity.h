#pragma once
#include <optional>
#include <string>
#include <vector>

#include "BookPaginator.h"
#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "FootnoteEntry.h"
#include "activities/Activity.h"

// EPUB reading UI over the FreeInkBook engine (BookPaginator). Position is
// tracked as (spineIndex, charStart) — the chapter character offset of the
// current page — which survives every layout-parameter change; the page
// number is derived per generation via pageForChar().
class EpubReaderActivity final : public Activity {
  std::string path_;
  std::string cacheDir_;
  BookPaginator paginator;

  int currentSpineIndex = 0;
  uint32_t currentPage = 0;
  // charStart of the page currently shown — the anchor that re-derives the
  // page after any re-pagination (settings/orientation change).
  uint32_t lastCharStart = 0;
  // The generation the open chapter was paginated for; a mismatch in render()
  // (settings changed while a menu was up) triggers reopen + reanchor.
  uint32_t openGeneration = 0;
  bool chapterOpen = false;

  // Pending landing position, applied once the target chapter's cache is
  // open (charStart wins over fraction; anchor wins over both).
  std::optional<uint32_t> pendingCharStart;
  std::optional<float> pendingChapterFraction;
  bool pendingLastPage = false;
  std::string pendingAnchor;

  int pagesUntilFullRefresh = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  bool buildPopupShown = false;  // indexing popup drawn for the current build
  bool bookmarksLoaded = false;  // deferred until after the first page render
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    uint32_t charStart;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Opens (paginating if needed) the chapter for currentSpineIndex under the
  // current generation and resolves pending landing state into currentPage.
  bool ensureChapterAndPosition();
  void renderPage(const freeink::book::Page& page, int statusBarSpace);
  void renderStatusBar() const;
  bool saveProgress();
  float currentBookFraction() const;
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openReaderMenu();
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();
  std::string currentPageText();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
      : Activity("EpubReader", renderer, mappedInput), path_(std::move(path)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};

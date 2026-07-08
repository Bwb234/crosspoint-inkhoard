#pragma once

#include <optional>
#include <string>

#include "BookPaginator.h"
#include "activities/Activity.h"

// Plain-text (.txt / .md) reading UI over the FreeInkBook engine: the file is
// one chapter driven through ChapterLayout::layoutPlainText, so justification,
// hyphenation, page caching, and character-offset progress all work exactly
// as they do for EPUBs.
class TxtReaderActivity final : public Activity {
  std::string path_;
  std::string cacheDir_;
  BookPaginator paginator;

  uint32_t currentPage = 0;
  uint32_t lastCharStart = 0;
  uint32_t openGeneration = 0;
  bool chapterOpen = false;
  bool buildPopupShown = false;
  std::optional<uint32_t> pendingCharStart;
  int pagesUntilFullRefresh = 0;

  bool ensureChapterAndPosition();
  void renderPage(const freeink::book::Page& page);
  void renderStatusBar() const;
  std::string title() const;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
      : Activity("TxtReader", renderer, mappedInput), path_(std::move(path)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};

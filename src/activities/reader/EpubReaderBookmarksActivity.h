#pragma once
#include "../../BookmarkEntry.h"
#include "../Activity.h"
#include "BookPaginator.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarksActivity final : public Activity {
  BookPaginator& paginator;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  std::vector<BookmarkEntry> bookmarks;
  int confirmingDelete = 0;  // 0 = hide dialog, 1 = show dialog, 2 = allow confirmation to delete

 public:
  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, BookPaginator& paginator,
                                       const std::string& epubPath)
      : Activity("EpubReaderBookmarks", renderer, mappedInput), paginator(paginator), epubPath(epubPath) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Calculate the vertical space to reserve for button hints based on orientation
  int getGutterBottom(const GfxRenderer& renderer);

  // Calculate the height available for the bookmark list based on orientation
  int getListHeight(const GfxRenderer& renderer);
};

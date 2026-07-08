#pragma once
#include "BookPaginator.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderChapterSelectionActivity final : public Activity {
  BookPaginator& paginator;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int selectorIndex = 0;

  // Total TOC items count
  int getTotalItems() const;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              BookPaginator& paginator, const int currentSpineIndex)
      : Activity("EpubReaderChapterSelection", renderer, mappedInput),
        paginator(paginator),
        currentSpineIndex(currentSpineIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

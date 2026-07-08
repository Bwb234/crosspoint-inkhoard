#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderUtils.h"
#include "FreeInkPageRenderer.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Same per-file cache dir convention the legacy Txt reader used.
  cacheDir_ = "/.crosspoint/txt_" + std::to_string(std::hash<std::string>{}(path_));
  Storage.ensureDirectoryExists("/.crosspoint");

  if (!paginator.open(path_, cacheDir_, renderer, /*forcePlainText=*/true)) {
    LOG_ERR("TRS", "Failed to open text file: %s", path_.c_str());
    activityManager.goToFullScreenMessage(tr(STR_PAGE_LOAD_ERROR), EpdFontFamily::BOLD);
    return;
  }

  const auto progress = EpubReaderUtils::loadProgress(cacheDir_);
  // Only v2 progress applies: the legacy txt format stored a raw page number
  // for a line-wrap pagination that no longer exists (reads back as a bogus
  // spine index — a plain text file has exactly one chapter).
  if (progress.valid && progress.spineIndex == 0 && progress.charStart != EpubReaderUtils::kNoCharStart) {
    pendingCharStart = progress.charStart;
  }

  APP_STATE.openEpubPath = path_;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(path_, title(), "", "");

  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  paginator.close();
}

std::string TxtReaderActivity::title() const {
  const size_t slash = path_.rfind('/');
  return path_.substr(slash == std::string::npos ? 0 : slash + 1);
}

void TxtReaderActivity::loop() {
  if (!paginator.isOpen()) {
    finish();
    return;
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(path_);
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (paginator.chapterReady() && currentPage + 1 < paginator.pageCount()) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

bool TxtReaderActivity::ensureChapterAndPosition() {
  const uint32_t gen = paginator.generation();
  if (!chapterOpen || openGeneration != gen) {
    // Settings/orientation change: reanchor on the page being shown.
    if (chapterOpen && !pendingCharStart.has_value()) {
      pendingCharStart = lastCharStart;
    }
    chapterOpen = false;
    buildPopupShown = false;

    if (!paginator.isChapterCached(0)) {
      GUI.drawPopup(renderer, tr(STR_INDEXING));
      pagesUntilFullRefresh = 1;
      buildPopupShown = true;
      // Free the glyph caches for the build; they re-warm on the next render.
      // (SD font tables stay resident — layout needs them per glyph.)
      if (auto* fcm = renderer.getFontCacheManager()) {
        if (auto* fdc = fcm->getDecompressor()) fdc->clearCache();
      }
      // Lend the framebuffer's 48 KB to the build arenas; the popup stays on
      // the panel and render() fully redraws after the restore below.
      renderer.releaseFrameBufferForBuild();
    }

    BookPaginator::BuildProgress progressCb;
    progressCb.ctx = this;
    progressCb.fn = [](void* ctx, uint32_t) {
      auto* self = static_cast<TxtReaderActivity*>(ctx);
      if (!self->buildPopupShown && self->renderer.hasFrameBuffer()) {
        GUI.drawPopup(self->renderer, tr(STR_INDEXING));
        self->pagesUntilFullRefresh = 1;
        self->buildPopupShown = true;
      }
    };

    const auto status = paginator.ensureChapter(0, progressCb);
    if (!renderer.hasFrameBuffer() && !renderer.restoreFrameBufferAfterBuild()) {
      LOG_ERR("TRS", "Framebuffer restore failed - restarting");
      ESP.restart();
    }
    if (status != freeink::book::BookStatus::Ok) {
      LOG_ERR("TRS", "Pagination failed: %d", static_cast<int>(status));
      return false;
    }
    chapterOpen = true;
    openGeneration = gen;
  }

  if (pendingCharStart.has_value()) {
    currentPage = paginator.pageForChar(*pendingCharStart);
    pendingCharStart.reset();
  }
  if (paginator.pageCount() > 0 && currentPage >= paginator.pageCount()) {
    currentPage = paginator.pageCount() - 1;
  }
  return true;
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!paginator.isOpen()) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom +=
      std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  paginator.configureLayout(static_cast<int16_t>(renderer.getScreenWidth()),
                            static_cast<int16_t>(renderer.getScreenHeight()), static_cast<int16_t>(orientedMarginLeft),
                            static_cast<int16_t>(orientedMarginRight), static_cast<int16_t>(orientedMarginTop),
                            static_cast<int16_t>(orientedMarginBottom));

  if (!ensureChapterAndPosition() || paginator.pageCount() == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  freeink::book::Page page{};
  if (paginator.readPage(currentPage, &page) != freeink::book::BookStatus::Ok) {
    LOG_ERR("TRS", "Failed to read page %u - clearing cache", currentPage);
    chapterOpen = false;
    requestUpdate();
    return;
  }
  lastCharStart = page.charStart;

  renderer.clearScreen();
  renderPage(page);

  EpubReaderUtils::saveProgress(cacheDir_, 0, lastCharStart);
}

void TxtReaderActivity::renderPage(const freeink::book::Page& page) {
  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  FreeInkPageRenderer::drawPage(renderer, paginator, page, cacheDir_);  // scan pass
  scope.endScanAndPrewarm();

  FreeInkPageRenderer::drawPage(renderer, paginator, page, cacheDir_);
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(
        renderer, [this, &page]() { FreeInkPageRenderer::drawPage(renderer, paginator, page, cacheDir_); });
  }
}

void TxtReaderActivity::renderStatusBar() const {
  const uint32_t pageCount = paginator.pageCount();
  const float progress = pageCount > 0 ? (currentPage + 1) * 100.0f / pageCount : 0;
  std::string barTitle;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    barTitle = title();
  }
  GUI.drawStatusBar(renderer, progress, static_cast<int>(currentPage) + 1, static_cast<int>(pageCount), barTitle);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  snprintf(info.title, sizeof(info.title), "%s", title().c_str());
  info.currentPage = static_cast<int>(currentPage) + 1;
  info.totalPages = static_cast<int>(paginator.pageCount());
  info.progressPercent =
      info.totalPages > 0 ? std::min(100, static_cast<int>(info.currentPage * 100.0f / info.totalPages + 0.5f)) : 0;
  return info;
}

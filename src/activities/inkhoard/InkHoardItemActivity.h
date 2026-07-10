#pragma once

#include <string>

#include "InkHoardClient.h"
#include "InkHoardModels.h"
#include "InkHoardUiLogic.h"
#include "inkhoard/InkHoardDownloadManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Item detail: Download / Open / Retry.
 * INKHOARD: plan 009
 */
class InkHoardItemActivity final : public Activity {
 public:
  enum class State { READY, DOWNLOADING, ERROR, OPENING };

  InkHoardItemActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, inkhoard::CompactItem item)
      : Activity("InkHoardItem", renderer, mappedInput), item_(item) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  State state = State::READY;
  inkhoard::CompactItem item_{};
  InkHoardClient client;
  InkHoardDownloadManager downloads{&client};
  bool downloaded = false;
  int actionIndex = 0;  // 0 download/retry, 1 open (when available)
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  std::string errorMessage;
  inkhoard::UiFailureKind failure = inkhoard::UiFailureKind::None;
  bool wifiActivated = false;
  bool openPending = false;

  void refreshDownloaded();
  void startDownload();
  void openLocal();
  int actionCount() const;
  bool preventAutoSleep() override { return true; }
};

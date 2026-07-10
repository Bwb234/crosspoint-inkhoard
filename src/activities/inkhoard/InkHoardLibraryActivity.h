#pragma once

#include <memory>
#include <string>
#include <vector>

#include "InkHoardClient.h"
#include "InkHoardModels.h"
#include "InkHoardUiLogic.h"
#include "inkhoard/InkHoardDownloadManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Browse InkHoard library pages / offline downloads.
 * INKHOARD: plan 009
 */
class InkHoardLibraryActivity final : public Activity {
 public:
  enum class State {
    NO_CREDENTIALS,
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    OFFLINE_LIST,
    ERROR,
  };

  explicit InkHoardLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InkHoardLibrary", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  State state = State::LOADING;
  InkHoardClient client;
  InkHoardDownloadManager downloads{&client};
  inkhoard::CursorStack cursors;
  // Heap: LibraryPage is ~45KB and must not live on the activity/stack frame.
  std::unique_ptr<inkhoard::LibraryPage> page;
  std::vector<inkhoard::Sidecar> offlineItems;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  inkhoard::UiFailureKind failure = inkhoard::UiFailureKind::None;
  bool wifiActivated = false;

  void checkCredentialsAndWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchPage();
  void loadOfflineList();
  void openSelected();
  void goNextPage();
  void goPrevPage();
  void mapError(inkhoard::ClientResult r);
  bool preventAutoSleep() override { return true; }
};

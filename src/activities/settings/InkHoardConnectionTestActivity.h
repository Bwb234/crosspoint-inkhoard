#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"

/**
 * Wi-Fi + InkHoard connection test (Bearer GET library?limit=1).
 * INKHOARD: plan 007
 */
class InkHoardConnectionTestActivity final : public Activity {
 public:
  explicit InkHoardConnectionTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InkHoardConnTest", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == TESTING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, TESTING, SUCCESS, FAILED };

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string detailMessage;

  void onWifiSelectionComplete(bool success);
  void performTest();
};

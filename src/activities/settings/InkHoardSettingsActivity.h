#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for InkHoard credentials and connection test.
 * INKHOARD: plan 007
 */
class InkHoardSettingsActivity final : public Activity {
 public:
  explicit InkHoardSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("InkHoardSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
};

#pragma once

#include <HalGPIO.h>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };
  enum class SwipeDir { None, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  MappedInputManager(HalGPIO& gpio, GfxRenderer& renderer) : gpio(gpio), renderer(renderer) {}

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  // Touch "back" gesture: a tap on the theme's header Back target, or in the
  // top-left corner, or a swipe up from the visible bottom edge. Folded into
  // Back's edges, so every screen gets it for free.
  bool wasBackGesture() const;
  // True (and writes the id) if a tap this frame hit a TouchRegistry item.
  // Activities treat the id as "select + activate". False on non-touch devices.
  bool wasItemTapped(int& id) const;
  // Stable touch-down candidate: fires once when a touch remains over an item
  // briefly without crossing tap slop, so swipes do not show row selection.
  bool wasItemTouchedDown(int& id) const;
  // Subset of wasItemTapped's releases held past the long-press threshold (check
  // this first). Distinguishes tap vs press-and-hold.
  bool wasItemLongPressed(int& id) const;
  // wasItemTapped for tab-bar tabs (id = tab index) and cover/card targets
  // (id = item index). Distinct kinds so a screen with both doesn't confuse them.
  bool wasTabTapped(int& id) const;
  bool wasCoverTapped(int& id) const;
  // True on a touch release anywhere on screen, with logical/oriented coords.
  bool wasScreenTapped(int& x, int& y) const;
  // Swipe direction in the current logical (oriented) frame, or None. A swipe also
  // raises the tap helpers above, so check this first and consume it.
  SwipeDir wasSwipe() const;
  // Page-scroll a list selection by a vertical swipe (touch nav without buttons):
  // swipe up advances down the list, swipe down moves up, by pageItems, clamped to
  // [0, count-1]. Updates index and returns true when a vertical swipe occurred, so
  // callers can requestUpdate() and return before the tap handlers run.
  bool wasListScroll(int& index, int count, int pageItems) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

 private:
  HalGPIO& gpio;
  GfxRenderer& renderer;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
  void rememberTouchHeldTime() const;
  bool wasBottomEdgeSwipeUp() const;

  mutable bool touchSelectTracking = false;
  mutable bool touchSelectEmitted = false;
  mutable int touchSelectId = -1;
  mutable bool touchHeldOverrideValid = false;
  mutable unsigned long touchHeldOverrideMs = 0;
  mutable unsigned long touchHeldOverrideAt = 0;
};

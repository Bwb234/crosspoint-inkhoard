#include "Activity.h"

#include <esp_heap_caps.h>

#include "ActivityManager.h"

// Heap next to every transition: `max` (largest contiguous block) against
// `free` is the fragmentation picture — long-lived allocations made after
// boot-time churn pin the middle of the free region, and these lines bisect
// which step planted them.
void Activity::onEnter() {
  LOG_DBG("ACT", "Entering activity: %s (heap free %u, max block %u)", name.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void Activity::onExit() {
  LOG_DBG("ACT", "Exiting activity: %s (heap free %u, max block %u)", name.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }

#include "InkHoardSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "InkHoardConnectionTestActivity.h"
#include "InkHoardCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_INKHOARD_SERVER_URL, StrId::STR_INKHOARD_DEVICE_TOKEN,
                                     StrId::STR_INKHOARD_DISPLAY_NAME, StrId::STR_INKHOARD_TEST_CONNECTION,
                                     StrId::STR_INKHOARD_CLEAR_CREDENTIALS};
}  // namespace

void InkHoardSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void InkHoardSettingsActivity::onExit() { Activity::onExit(); }

void InkHoardSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void InkHoardSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Server URL — prefill https:// if empty
    const std::string currentUrl = INKHOARD_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_INKHOARD_SERVER_URL),
                                                                   prefillUrl, 256, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               if (!INKHOARD_STORE.setServerUrl(kb.text)) {
                                 // Rejected (e.g. plain http) — leave previous value
                                 LOG_DBG("INKH", "%s", tr(STR_INKHOARD_URL_REJECTED));
                               } else {
                                 INKHOARD_STORE.saveToFile();
                               }
                               requestUpdate();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Device token — never prefill the real value
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_INKHOARD_DEVICE_TOKEN), "", 128,
                                                InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            if (!kb.text.empty()) {
              INKHOARD_STORE.setToken(kb.text);
              INKHOARD_STORE.saveToFile();
            }
            requestUpdate();
          }
        });
  } else if (selectedIndex == 2) {
    // Display name
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_INKHOARD_DISPLAY_NAME),
                                                INKHOARD_STORE.getDisplayName(), 64, InputType::Text),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            INKHOARD_STORE.setDisplayName(kb.text);
            INKHOARD_STORE.saveToFile();
            requestUpdate();
          }
        });
  } else if (selectedIndex == 3) {
    // Test connection
    if (!INKHOARD_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(std::make_unique<InkHoardConnectionTestActivity>(renderer, mappedInput),
                           [](const ActivityResult&) {});
  } else if (selectedIndex == 4) {
    // Clear credentials
    if (!INKHOARD_STORE.hasCredentials() && INKHOARD_STORE.getDisplayName().empty() &&
        INKHOARD_STORE.getServerUrl().empty()) {
      return;
    }
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_INKHOARD_CLEAR_CONFIRM),
                                               tr(STR_INKHOARD_CLEAR_BODY)),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            INKHOARD_STORE.clear();
            requestUpdate();
          }
        });
  }
}

void InkHoardSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_INKHOARD));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        if (index == 0) {
          const auto& url = INKHOARD_STORE.getServerUrl();
          return url.empty() ? std::string(tr(STR_NOT_SET)) : url;
        }
        if (index == 1) {
          return INKHOARD_STORE.getToken().empty() ? std::string(tr(STR_NOT_SET))
                                                   : INKHOARD_STORE.getRedactedToken();
        }
        if (index == 2) {
          const auto& name = INKHOARD_STORE.getDisplayName();
          return name.empty() ? std::string(tr(STR_NOT_SET)) : name;
        }
        if (index == 3) {
          return INKHOARD_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#include "InkHoardConnectionTestActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "InkHoardConnectionTest.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void InkHoardConnectionTestActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      statusMessage = tr(STR_AUTH_FAILED);
      detailMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = TESTING;
    statusMessage = tr(STR_INKHOARD_TESTING);
    detailMessage.clear();
  }
  requestUpdate();

  performTest();
}

void InkHoardConnectionTestActivity::performTest() {
  const auto outcome = InkHoardConnectionTest::run();

  {
    RenderLock lock(*this);
    if (outcome.result == InkHoardConnectionTest::Result::Ok) {
      state = SUCCESS;
      statusMessage = tr(STR_INKHOARD_CONNECTED);
      detailMessage.clear();
    } else {
      state = FAILED;
      statusMessage = tr(STR_AUTH_FAILED);
      switch (outcome.result) {
        case InkHoardConnectionTest::Result::NoCredentials:
          detailMessage = tr(STR_INKHOARD_NO_CREDENTIALS);
          break;
        case InkHoardConnectionTest::Result::LowMemory:
          detailMessage = tr(STR_INKHOARD_LOW_MEMORY);
          break;
        case InkHoardConnectionTest::Result::Unreachable:
          detailMessage = tr(STR_INKHOARD_UNREACHABLE);
          break;
        case InkHoardConnectionTest::Result::TlsFailure:
          detailMessage = tr(STR_INKHOARD_TLS_FAILED);
          break;
        case InkHoardConnectionTest::Result::Unauthorized:
          detailMessage = tr(STR_INKHOARD_TOKEN_REJECTED);
          break;
        case InkHoardConnectionTest::Result::Forbidden:
          detailMessage = tr(STR_INKHOARD_TOKEN_NO_ACCESS);
          break;
        case InkHoardConnectionTest::Result::HttpError:
          detailMessage = std::string(tr(STR_INKHOARD_SERVER_ERROR)) + " " + std::to_string(outcome.httpCode);
          break;
        default:
          detailMessage = outcome.detail;
          break;
      }
    }
  }
  requestUpdate();
}

void InkHoardConnectionTestActivity::onEnter() {
  Activity::onEnter();

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void InkHoardConnectionTestActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void InkHoardConnectionTestActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_INKHOARD_CONN_TEST));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == TESTING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    if (!detailMessage.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, detailMessage.c_str());
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void InkHoardConnectionTestActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}

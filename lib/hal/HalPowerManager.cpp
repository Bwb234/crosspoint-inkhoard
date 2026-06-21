#include "HalPowerManager.h"

#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
// The fuel gauge's I2C controller (Wire or Wire1) per the active board profile.
// On single-bus SoCs (ESP32-C3, SOC_I2C_NUM == 1) Wire1 doesn't exist, so always
// use Wire. On Sticky the gauge is on Wire1 so it doesn't fight the GT911 touch,
// which owns Wire.
TwoWire& gaugeWire() {
#if SOC_I2C_NUM > 1
  if (BoardConfig::ACTIVE.batteryGauge.i2cBus == 1) return Wire1;
#endif
  return Wire;
}
}  // namespace

void HalPowerManager::begin() {
  const auto& gauge = BoardConfig::ACTIVE.batteryGauge;
  if (gauge.gaugeAddr != 0) {
    // Board has an I2C fuel gauge (X3, LilyGo, Sticky, ...). Pins/freq come from
    // the active board profile, not hardcoded X3 values. I2C init must come AFTER
    // gpio.begin() so early hardware detection/probes are finished.
    gaugeWire().begin(gauge.i2cSda, gauge.i2cScl, gauge.i2cHz);
    gaugeWire().setTimeOut(4);
    _batteryUseI2C = true;
  } else if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    // ADC-sensed board (X4: GPIO0, M5Paper: GPIO35). Skip when the profile leaves
    // batteryAdc unassigned (PIN_UNASSIGNED) — pinMode(255) faults the GPIO mux.
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // Pre-sleep routines from the original firmware
#if !SOC_PM_SUPPORT_EXT1_WAKEUP  // RISC-V (C3 / X4)
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
  // (X4-specific: on classic ESP32/M5Paper GPIO13 is SD MISO, so this is skipped.)
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
#endif
  freeink::PowerManager::armPowerButtonWakeup();
  freeink::PowerManager::deepSleep();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from the I2C fuel gauge (16-bit LE register). Gauge
    // address comes from the active board profile (all current gauges are
    // BQ27220-class, SOC at 0x2C). On I2C error, keep last known value to avoid
    // UI jitter/slowdowns.
    const uint8_t gaugeAddr = BoardConfig::ACTIVE.batteryGauge.gaugeAddr;
    TwoWire& w = gaugeWire();
    w.beginTransmission(gaugeAddr);
    w.write(BQ27220_SOC_REG);
    if (w.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    w.requestFrom(gaugeAddr, (uint8_t)2);
    if (w.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = w.read();
    const uint8_t hi = w.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  // ADC pin from the active profile (X4 GPIO0 / M5Paper GPIO35); default 2:1 divider.
  static const BatteryMonitor battery = BatteryMonitor(BoardConfig::ACTIVE.batteryAdc);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}

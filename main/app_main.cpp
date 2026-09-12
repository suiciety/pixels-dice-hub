#include "board.h"
#include "dice_model.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "pixels_ble.h"
#include "preferences.h"
#include "ui.h"
#include "web_server.h"

namespace {

constexpr char kTag[] = "pixels_display";

} // namespace

extern "C" void app_main() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);

  static board::Hardware hardware;
  static app::DiceModel model;
  app::RestorePreferences(&model);

  ESP_ERROR_CHECK(board::Init(&hardware));
  ESP_ERROR_CHECK(app::StartUi(&hardware, &model));
  const bool wifi_enabled = app::RestoreWebNetworkEnabled(false);
  app::SetPixelsScannerWifiActive(wifi_enabled);
  if (wifi_enabled) {
    ESP_ERROR_CHECK(app::StartWebServer(&model));
  } else {
    ESP_LOGI(kTag, "Wi-Fi disabled; enable it from the touchscreen");
  }
  ESP_ERROR_CHECK(app::StartPixelsScanner(&model));
  ESP_LOGI(kTag, "Pixels dice display started");
}

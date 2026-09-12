#include "dice_model.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "pixels_ble.h"
#include "preferences.h"
#include "web_server.h"

namespace {

constexpr char kTag[] = "pixels_web";

} // namespace

extern "C" void app_main() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);

  static app::DiceModel model;
  app::RestorePreferences(&model);
  ESP_ERROR_CHECK(app::StartWebServer(&model));
  app::SetPixelsScannerWifiActive(true);
  ESP_ERROR_CHECK(app::StartPixelsScanner(&model));
  ESP_LOGI(kTag, "Pixels dice headless web display started");
}

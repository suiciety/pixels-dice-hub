#pragma once

#include "dice_model.h"
#include "esp_err.h"

namespace app {

esp_err_t StartPixelsScanner(DiceModel *model);
void SetPixelsScannerWifiActive(bool active);
esp_err_t RequestPixelsBlink(uint32_t pixel_id);
esp_err_t RequestPixelsInfo(uint32_t pixel_id);
bool IsPixelsBlinkBusy();

} // namespace app

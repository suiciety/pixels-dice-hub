#pragma once

#include "dice_model.h"
#include "esp_err.h"

namespace app {

enum class NetworkStatus {
  kOff,
  kAccessPoint,
  kConnecting,
  kConnected,
};

esp_err_t StartWebServer(DiceModel *model);
esp_err_t StopWebServer();
esp_err_t SetWebNetworkEnabled(DiceModel *model, bool enabled);
bool RestoreWebNetworkEnabled(bool default_value);
bool IsWebNetworkEnabled();
NetworkStatus GetWebNetworkStatus();
const char *NetworkStatusName(NetworkStatus status);

} // namespace app

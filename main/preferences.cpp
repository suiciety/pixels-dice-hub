#include "preferences.h"

#include "esp_timer.h"
#include "nvs.h"

namespace app {

void RestorePreferences(DiceModel *model) {
  if (model == nullptr) {
    return;
  }
  nvs_handle_t handle;
  if (nvs_open("pixels", NVS_READONLY, &handle) != ESP_OK) {
    return;
  }
  uint32_t ids[kMaxDice] = {};
  std::size_t size = sizeof(ids);
  if (nvs_get_blob(handle, "paired_ids", ids, &size) == ESP_OK) {
    model->RestorePaired(ids, size / sizeof(uint32_t));
  }
  uint8_t aggregate = 0;
  if (nvs_get_u8(handle, "aggregate", &aggregate) == ESP_OK &&
      aggregate <= static_cast<uint8_t>(AggregateMode::kLow)) {
    model->RestoreAggregate(static_cast<AggregateMode>(aggregate));
  }
  nvs_close(handle);
}

bool SavePreferences(const DiceModel &model) {
  uint32_t ids[kMaxDice] = {};
  const std::size_t count = model.PairedIds(ids, kMaxDice);
  nvs_handle_t handle;
  if (nvs_open("pixels", NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t result =
      nvs_set_blob(handle, "paired_ids", ids, count * sizeof(uint32_t));
  const Snapshot snapshot =
      model.GetSnapshot(static_cast<uint64_t>(esp_timer_get_time() / 1000));
  if (result == ESP_OK) {
    result = nvs_set_u8(handle, "aggregate",
                        static_cast<uint8_t>(snapshot.aggregate_mode));
  }
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }
  nvs_close(handle);
  return result == ESP_OK;
}

} // namespace app

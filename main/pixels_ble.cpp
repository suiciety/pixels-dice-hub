#include "pixels_ble.h"

#include <array>
#include <atomic>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "pixels_protocol.h"

namespace app {
namespace {

constexpr char kTag[] = "pixels_ble";
DiceModel *model;
uint8_t own_address_type;
std::atomic_bool wifi_active{true};
std::atomic_bool host_ready{false};
std::atomic_bool blink_busy{false};

void StartScan();
int GapEvent(ble_gap_event *event, void *);

constexpr uint16_t kInvalidConnectionHandle = 0xffff;
constexpr uint8_t kBlinkMessage = 29;

const ble_uuid128_t kModernService = BLE_UUID128_INIT(
    0x5b, 0x9b, 0xdc, 0x8e, 0x0c, 0x35, 0x62, 0xa9, 0xf2, 0x43, 0x5a, 0x7a,
    0x01, 0x00, 0xb9, 0xa6);
const ble_uuid128_t kModernWrite = BLE_UUID128_INIT(
    0x5b, 0x9b, 0xdc, 0x8e, 0x0c, 0x35, 0x62, 0xa9, 0xf2, 0x43, 0x5a, 0x7a,
    0x03, 0x00, 0xb9, 0xa6);
const ble_uuid128_t kLegacyService = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
    0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t kLegacyWrite = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
    0x02, 0x00, 0x40, 0x6e);

struct BlinkOperation {
  uint32_t pixel_id = 0;
  ble_addr_t address{};
  uint16_t connection_handle = kInvalidConnectionHandle;
  uint16_t service_start = 0;
  uint16_t service_end = 0;
  uint16_t write_handle = 0;
  bool legacy = false;
  uint8_t payload[14] = {};
};

BlinkOperation blink;
ble_npl_event blink_event;

struct PendingAdvertisement {
  ble_addr_t address{};
  pixels::Advertisement data{};
  uint64_t last_seen_ms = 0;
  pixels::RollState last_logged_state = pixels::RollState::kUnknown;
  uint8_t last_logged_face = 0;
  bool used = false;
  bool logged = false;
};

std::array<PendingAdvertisement, 16> pending;

void PutLe16(uint8_t *destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void PutLe32(uint8_t *destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

void ResumeAfterBlink() {
  blink = {};
  blink.connection_handle = kInvalidConnectionHandle;
  blink_busy.store(false);
  if (host_ready.load()) {
    StartScan();
  }
}

void DisconnectAfterBlink() {
  if (blink.connection_handle == kInvalidConnectionHandle) {
    ResumeAfterBlink();
    return;
  }
  const int result =
      ble_gap_terminate(blink.connection_handle, BLE_ERR_REM_USER_CONN_TERM);
  if (result != 0 && result != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "Unable to disconnect after blink: %d", result);
    ResumeAfterBlink();
  }
}

int BlinkWritten(uint16_t, const ble_gatt_error *error, ble_gatt_attr *,
                 void *) {
  if (error->status == 0) {
    ESP_LOGI(kTag, "Blink sent to Pixel %08lx",
             static_cast<unsigned long>(blink.pixel_id));
  } else {
    ESP_LOGW(kTag, "Blink write failed for Pixel %08lx: %d",
             static_cast<unsigned long>(blink.pixel_id), error->status);
  }
  DisconnectAfterBlink();
  return 0;
}

int WriteCharacteristicDiscovered(uint16_t connection_handle,
                                  const ble_gatt_error *error,
                                  const ble_gatt_chr *characteristic, void *) {
  if (error->status == 0) {
    blink.write_handle = characteristic->val_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE || blink.write_handle == 0) {
    ESP_LOGW(kTag, "Pixels write characteristic not found: %d",
             error->status);
    DisconnectAfterBlink();
    return 0;
  }
  const int result =
      ble_gattc_write_flat(connection_handle, blink.write_handle, blink.payload,
                           sizeof(blink.payload), BlinkWritten, nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to start Pixels blink write: %d", result);
    DisconnectAfterBlink();
  }
  return 0;
}

void DiscoverWriteCharacteristic() {
  const ble_uuid_t *write_uuid =
      blink.legacy ? &kLegacyWrite.u : &kModernWrite.u;
  const int result = ble_gattc_disc_chrs_by_uuid(
      blink.connection_handle, blink.service_start, blink.service_end,
      write_uuid, WriteCharacteristicDiscovered, nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to discover Pixels write characteristic: %d",
             result);
    DisconnectAfterBlink();
  }
}

int ServiceDiscovered(uint16_t, const ble_gatt_error *error,
                      const ble_gatt_svc *service, void *) {
  if (error->status == 0) {
    blink.service_start = service->start_handle;
    blink.service_end = service->end_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    ESP_LOGW(kTag, "Pixels service discovery failed: %d", error->status);
    DisconnectAfterBlink();
    return 0;
  }
  if (blink.service_start != 0) {
    DiscoverWriteCharacteristic();
    return 0;
  }
  if (!blink.legacy) {
    blink.legacy = true;
    const int result = ble_gattc_disc_svc_by_uuid(
        blink.connection_handle, &kLegacyService.u, ServiceDiscovered,
        nullptr);
    if (result != 0) {
      ESP_LOGW(kTag, "Unable to discover legacy Pixels service: %d", result);
      DisconnectAfterBlink();
    }
    return 0;
  }
  ESP_LOGW(kTag, "Pixel %08lx has no supported GATT service",
           static_cast<unsigned long>(blink.pixel_id));
  DisconnectAfterBlink();
  return 0;
}

void BeginBlink(ble_npl_event *) {
  bool found = false;
  for (const auto &entry : pending) {
    if (entry.used && entry.data.has_identity &&
        entry.data.pixel_id == blink.pixel_id) {
      blink.address = entry.address;
      found = true;
      break;
    }
  }
  if (!found) {
    ESP_LOGW(kTag, "No recent BLE address for Pixel %08lx",
             static_cast<unsigned long>(blink.pixel_id));
    ResumeAfterBlink();
    return;
  }

  const int cancel_result = ble_gap_disc_cancel();
  if (cancel_result != 0 && cancel_result != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "Unable to stop scan for blink: %d", cancel_result);
    ResumeAfterBlink();
    return;
  }
  const int connect_result =
      ble_gap_connect(own_address_type, &blink.address, 10000, nullptr,
                      GapEvent, nullptr);
  if (connect_result != 0) {
    ESP_LOGW(kTag, "Unable to connect to Pixel %08lx: %d",
             static_cast<unsigned long>(blink.pixel_id), connect_result);
    ResumeAfterBlink();
  }
}

PendingAdvertisement *FindPending(const ble_addr_t &address, uint64_t now_ms) {
  PendingAdvertisement *oldest = &pending[0];
  for (auto &entry : pending) {
    if (entry.used && entry.address.type == address.type &&
        std::memcmp(entry.address.val, address.val, sizeof(address.val)) == 0) {
      return &entry;
    }
    if (!entry.used) {
      oldest = &entry;
      break;
    }
    if (entry.last_seen_ms < oldest->last_seen_ms) {
      oldest = &entry;
    }
  }
  *oldest = {};
  oldest->used = true;
  oldest->address = address;
  oldest->last_seen_ms = now_ms;
  return oldest;
}

int GapEvent(ble_gap_event *event, void *) {
  if (event->type == BLE_GAP_EVENT_DISC) {
    pixels::Advertisement fragment;
    if (pixels::ParseAdvertisementFragment(
            event->disc.data, event->disc.length_data, event->disc.rssi,
            &fragment)) {
      const uint64_t now_ms =
          static_cast<uint64_t>(esp_timer_get_time() / 1000);
      PendingAdvertisement *entry = FindPending(event->disc.addr, now_ms);
      pixels::MergeAdvertisement(fragment, &entry->data);
      entry->last_seen_ms = now_ms;
      if (entry->data.has_identity && entry->data.has_status) {
        if (!entry->logged ||
            entry->last_logged_state != entry->data.roll_state ||
            entry->last_logged_face != entry->data.face_index) {
          ESP_LOGI(kTag,
                   "Pixel %08lx name='%s' type=%s state=%u face=%u "
                   "battery=%u%% rssi=%d",
                   static_cast<unsigned long>(entry->data.pixel_id),
                   entry->data.name, pixels::DieTypeName(entry->data.die_type),
                   static_cast<unsigned>(entry->data.roll_state),
                   static_cast<unsigned>(entry->data.face_index),
                   static_cast<unsigned>(entry->data.battery_percent),
                   entry->data.rssi);
          entry->last_logged_state = entry->data.roll_state;
          entry->last_logged_face = entry->data.face_index;
          entry->logged = true;
        }
        model->Ingest(entry->data, now_ms);
      }
    }
  } else if (event->type == BLE_GAP_EVENT_CONNECT && blink_busy.load()) {
    if (event->connect.status != 0) {
      ESP_LOGW(kTag, "Unable to connect for Pixel blink: %d",
               event->connect.status);
      ResumeAfterBlink();
      return 0;
    }
    blink.connection_handle = event->connect.conn_handle;
    blink.service_start = 0;
    blink.service_end = 0;
    blink.write_handle = 0;
    blink.legacy = false;
    const int result = ble_gattc_disc_svc_by_uuid(
        blink.connection_handle, &kModernService.u, ServiceDiscovered,
        nullptr);
    if (result != 0) {
      ESP_LOGW(kTag, "Unable to discover Pixels service: %d", result);
      DisconnectAfterBlink();
    }
  } else if (event->type == BLE_GAP_EVENT_DISCONNECT && blink_busy.load() &&
             event->disconnect.conn.conn_handle == blink.connection_handle) {
    ResumeAfterBlink();
  } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE &&
             host_ready.load() && !blink_busy.load()) {
    StartScan();
  }
  return 0;
}

void StartScan() {
  const bool sharing_radio = wifi_active.load();
  const ble_gap_disc_params params = {
      .itvl = BLE_GAP_SCAN_ITVL_MS(100),
      .window = static_cast<uint16_t>(
          sharing_radio ? BLE_GAP_SCAN_WIN_MS(50)
                        : BLE_GAP_SCAN_WIN_MS(95)),
      .filter_policy = 0,
      .limited = 0,
      .passive = 0,
      .filter_duplicates = 0,
      .disable_observer_mode = 0,
  };
  const int result = ble_gap_disc(own_address_type, BLE_HS_FOREVER, &params,
                                  GapEvent, nullptr);
  if (result != 0 && result != BLE_HS_EALREADY) {
    ESP_LOGE(kTag, "Unable to start BLE scan: %d", result);
  } else if (result == 0) {
    ESP_LOGI(kTag, "BLE scan duty set to %u%%",
             sharing_radio ? 50U : 95U);
  }
}

void OnSync() {
  const int result = ble_hs_id_infer_auto(0, &own_address_type);
  if (result != 0) {
    ESP_LOGE(kTag, "Unable to infer BLE address type: %d", result);
    return;
  }
  host_ready.store(true);
  StartScan();
}

void HostTask(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

} // namespace

esp_err_t StartPixelsScanner(DiceModel *dice_model) {
  if (dice_model == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  model = dice_model;
  ESP_RETURN_ON_ERROR(nimble_port_init(), kTag, "NimBLE init failed");
  ble_hs_cfg.sync_cb = OnSync;
  ble_npl_event_init(&blink_event, BeginBlink, nullptr);
  nimble_port_freertos_init(HostTask);
  return ESP_OK;
}

void SetPixelsScannerWifiActive(bool active) {
  if (wifi_active.exchange(active) == active || !host_ready.load() ||
      blink_busy.load()) {
    return;
  }
  const int result = ble_gap_disc_cancel();
  if (result != 0 && result != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "Unable to cancel BLE scan for duty change: %d", result);
  }
  StartScan();
}

esp_err_t RequestPixelsBlink(uint32_t pixel_id) {
  if (pixel_id == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!host_ready.load() || blink_busy.exchange(true)) {
    return ESP_ERR_INVALID_STATE;
  }

  blink = {};
  blink.pixel_id = pixel_id;
  blink.connection_handle = kInvalidConnectionHandle;
  blink.payload[0] = kBlinkMessage;
  blink.payload[1] = 2;
  PutLe16(&blink.payload[2], 700);
  PutLe32(&blink.payload[4], 0x0000ffff);
  PutLe32(&blink.payload[8], 0xffffffff);
  blink.payload[12] = 128;
  blink.payload[13] = 0;
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &blink_event);
  return ESP_OK;
}

bool IsPixelsBlinkBusy() { return blink_busy.load(); }

} // namespace app

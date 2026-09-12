#include "pixels_ble.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/ble.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "pixels_protocol.h"

namespace app {
namespace {

constexpr char kTag[] = "pixels_ble";
constexpr uint16_t kInvalidConnectionHandle = 0xffff;
constexpr std::size_t kCommandQueueSize = 8;
constexpr uint8_t kWhoAreYouMessage = 1;
constexpr uint8_t kIAmADieMessage = 2;
constexpr uint8_t kBlinkMessage = 29;
constexpr uint8_t kRequestTemperatureMessage = 60;
constexpr uint8_t kTemperatureMessage = 61;
constexpr uint8_t kInfoResponse = 1U << 0;
constexpr uint8_t kTemperatureResponse = 1U << 1;

DiceModel *model;
uint8_t own_address_type;
std::atomic_bool wifi_active{true};
std::atomic_bool host_ready{false};
std::atomic_bool command_pipeline_active{false};
std::atomic_uint command_count{0};
QueueHandle_t command_queue;

void StartScan();
int GapEvent(ble_gap_event *event, void *);

const ble_uuid128_t kModernService = BLE_UUID128_INIT(
    0x5b, 0x9b, 0xdc, 0x8e, 0x0c, 0x35, 0x62, 0xa9, 0xf2, 0x43, 0x5a, 0x7a,
    0x01, 0x00, 0xb9, 0xa6);
const ble_uuid128_t kModernNotify = BLE_UUID128_INIT(
    0x5b, 0x9b, 0xdc, 0x8e, 0x0c, 0x35, 0x62, 0xa9, 0xf2, 0x43, 0x5a, 0x7a,
    0x02, 0x00, 0xb9, 0xa6);
const ble_uuid128_t kModernWrite = BLE_UUID128_INIT(
    0x5b, 0x9b, 0xdc, 0x8e, 0x0c, 0x35, 0x62, 0xa9, 0xf2, 0x43, 0x5a, 0x7a,
    0x03, 0x00, 0xb9, 0xa6);
const ble_uuid128_t kLegacyService = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
    0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t kLegacyNotify = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
    0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t kLegacyWrite = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
    0x02, 0x00, 0x40, 0x6e);
const ble_uuid16_t kClientConfiguration =
    BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

enum class CommandType : uint8_t {
  kBlink,
  kRefreshInfo,
};

struct QueuedCommand {
  CommandType type = CommandType::kBlink;
  uint32_t pixel_id = 0;
};

struct CommandOperation {
  CommandType type = CommandType::kBlink;
  uint32_t pixel_id = 0;
  ble_addr_t address{};
  uint16_t connection_handle = kInvalidConnectionHandle;
  uint16_t service_start = 0;
  uint16_t service_end = 0;
  uint16_t write_handle = 0;
  uint16_t notify_handle = 0;
  uint16_t cccd_handle = 0;
  uint8_t request_index = 0;
  uint8_t responses = 0;
  bool legacy = false;
};

CommandOperation command;
ble_npl_event command_event;
ble_npl_callout command_timeout;

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

uint16_t ReadLe16(const uint8_t *source) {
  return static_cast<uint16_t>(source[0]) |
         static_cast<uint16_t>(source[1]) << 8;
}

int16_t ReadLeI16(const uint8_t *source) {
  return static_cast<int16_t>(ReadLe16(source));
}

uint32_t ReadLe32(const uint8_t *source) {
  return static_cast<uint32_t>(source[0]) |
         static_cast<uint32_t>(source[1]) << 8 |
         static_cast<uint32_t>(source[2]) << 16 |
         static_cast<uint32_t>(source[3]) << 24;
}

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

const char *CommandName(CommandType type) {
  return type == CommandType::kBlink ? "blink" : "info refresh";
}

void ScheduleCommandIfIdle() {
  bool expected = false;
  if (command_pipeline_active.compare_exchange_strong(expected, true)) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
  }
}

void CompleteCommand() {
  ble_npl_callout_stop(&command_timeout);
  if (command_count.load() > 0) {
    command_count.fetch_sub(1);
  }
  command = {};
  command.connection_handle = kInvalidConnectionHandle;

  if (uxQueueMessagesWaiting(command_queue) > 0) {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    return;
  }

  command_pipeline_active.store(false);
  if (uxQueueMessagesWaiting(command_queue) > 0) {
    ScheduleCommandIfIdle();
    return;
  }
  if (host_ready.load()) {
    StartScan();
  }
}

void DisconnectAfterCommand() {
  ble_npl_callout_stop(&command_timeout);
  if (command.connection_handle == kInvalidConnectionHandle) {
    CompleteCommand();
    return;
  }
  const int result =
      ble_gap_terminate(command.connection_handle, BLE_ERR_REM_USER_CONN_TERM);
  if (result != 0 && result != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "Unable to disconnect after %s: %d",
             CommandName(command.type), result);
    CompleteCommand();
  }
}

void CommandTimedOut(ble_npl_event *) {
  ESP_LOGW(kTag, "Pixel %08lx %s timed out",
           static_cast<unsigned long>(command.pixel_id),
           CommandName(command.type));
  DisconnectAfterCommand();
}

void StartResponseTimeout() {
  ble_npl_callout_reset(&command_timeout, ble_npl_time_ms_to_ticks32(3000));
}

int CommandWritten(uint16_t, const ble_gatt_error *error, ble_gatt_attr *,
                   void *) {
  if (error->status != 0) {
    ESP_LOGW(kTag, "Pixel %08lx %s write failed: %d",
             static_cast<unsigned long>(command.pixel_id),
             CommandName(command.type), error->status);
    DisconnectAfterCommand();
    return 0;
  }

  if (command.type == CommandType::kBlink) {
    ESP_LOGI(kTag, "Blink sent to Pixel %08lx",
             static_cast<unsigned long>(command.pixel_id));
    DisconnectAfterCommand();
    return 0;
  }

  ++command.request_index;
  if (command.request_index == 1) {
    const uint8_t request = kRequestTemperatureMessage;
    const int result =
        ble_gattc_write_flat(command.connection_handle, command.write_handle,
                             &request, sizeof(request), CommandWritten, nullptr);
    if (result != 0) {
      ESP_LOGW(kTag, "Unable to request Pixel temperature: %d", result);
      DisconnectAfterCommand();
    }
    return 0;
  }
  StartResponseTimeout();
  return 0;
}

void SendCommandPayload() {
  if (command.type == CommandType::kBlink) {
    uint8_t payload[14] = {};
    payload[0] = kBlinkMessage;
    payload[1] = 2;
    PutLe16(&payload[2], 700);
    PutLe32(&payload[4], 0x0000ffff);
    PutLe32(&payload[8], 0xffffffff);
    payload[12] = 128;
    const int result =
        ble_gattc_write_flat(command.connection_handle, command.write_handle,
                             payload, sizeof(payload), CommandWritten, nullptr);
    if (result != 0) {
      ESP_LOGW(kTag, "Unable to start Pixels blink write: %d", result);
      DisconnectAfterCommand();
    }
    return;
  }

  command.request_index = 0;
  command.responses = 0;
  const uint8_t request = kWhoAreYouMessage;
  const int result =
      ble_gattc_write_flat(command.connection_handle, command.write_handle,
                           &request, sizeof(request), CommandWritten, nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to request Pixel information: %d", result);
    DisconnectAfterCommand();
  }
}

int SubscriptionWritten(uint16_t, const ble_gatt_error *error, ble_gatt_attr *,
                        void *) {
  if (error->status != 0) {
    ESP_LOGW(kTag, "Unable to subscribe to Pixel notifications: %d",
             error->status);
    DisconnectAfterCommand();
    return 0;
  }
  SendCommandPayload();
  return 0;
}

int DescriptorDiscovered(uint16_t connection_handle,
                         const ble_gatt_error *error,
                         uint16_t,
                         const ble_gatt_dsc *descriptor, void *) {
  if (error->status == 0) {
    if (ble_uuid_cmp(&descriptor->uuid.u, &kClientConfiguration.u) == 0) {
      command.cccd_handle = descriptor->handle;
    }
    return 0;
  }
  if (error->status != BLE_HS_EDONE || command.cccd_handle == 0) {
    ESP_LOGW(kTag, "Pixels notification descriptor not found: %d",
             error->status);
    DisconnectAfterCommand();
    return 0;
  }
  const uint8_t subscribe[] = {1, 0};
  const int result = ble_gattc_write_flat(
      connection_handle, command.cccd_handle, subscribe, sizeof(subscribe),
      SubscriptionWritten, nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to subscribe to Pixel notifications: %d", result);
    DisconnectAfterCommand();
  }
  return 0;
}

int NotifyCharacteristicDiscovered(uint16_t connection_handle,
                                   const ble_gatt_error *error,
                                   const ble_gatt_chr *characteristic, void *) {
  if (error->status == 0) {
    command.notify_handle = characteristic->val_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE || command.notify_handle == 0) {
    ESP_LOGW(kTag, "Pixels notify characteristic not found: %d",
             error->status);
    DisconnectAfterCommand();
    return 0;
  }
  const int result = ble_gattc_disc_all_dscs(
      connection_handle, command.notify_handle, command.service_end,
      DescriptorDiscovered, nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to discover Pixel notification descriptor: %d",
             result);
    DisconnectAfterCommand();
  }
  return 0;
}

void DiscoverNotifyCharacteristic() {
  const ble_uuid_t *notify_uuid =
      command.legacy ? &kLegacyNotify.u : &kModernNotify.u;
  const int result = ble_gattc_disc_chrs_by_uuid(
      command.connection_handle, command.service_start, command.service_end,
      notify_uuid, NotifyCharacteristicDiscovered, nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to discover Pixels notify characteristic: %d",
             result);
    DisconnectAfterCommand();
  }
}

int WriteCharacteristicDiscovered(uint16_t,
                                  const ble_gatt_error *error,
                                  const ble_gatt_chr *characteristic, void *) {
  if (error->status == 0) {
    command.write_handle = characteristic->val_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE || command.write_handle == 0) {
    ESP_LOGW(kTag, "Pixels write characteristic not found: %d",
             error->status);
    DisconnectAfterCommand();
    return 0;
  }
  if (command.type == CommandType::kBlink) {
    SendCommandPayload();
  } else {
    DiscoverNotifyCharacteristic();
  }
  return 0;
}

void DiscoverWriteCharacteristic() {
  const ble_uuid_t *write_uuid =
      command.legacy ? &kLegacyWrite.u : &kModernWrite.u;
  const int result = ble_gattc_disc_chrs_by_uuid(
      command.connection_handle, command.service_start, command.service_end,
      write_uuid, WriteCharacteristicDiscovered, nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to discover Pixels write characteristic: %d",
             result);
    DisconnectAfterCommand();
  }
}

int ServiceDiscovered(uint16_t, const ble_gatt_error *error,
                      const ble_gatt_svc *service, void *) {
  if (error->status == 0) {
    command.service_start = service->start_handle;
    command.service_end = service->end_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    ESP_LOGW(kTag, "Pixels service discovery failed: %d", error->status);
    DisconnectAfterCommand();
    return 0;
  }
  if (command.service_start != 0) {
    DiscoverWriteCharacteristic();
    return 0;
  }
  if (!command.legacy) {
    command.legacy = true;
    const int result = ble_gattc_disc_svc_by_uuid(
        command.connection_handle, &kLegacyService.u, ServiceDiscovered,
        nullptr);
    if (result != 0) {
      ESP_LOGW(kTag, "Unable to discover legacy Pixels service: %d", result);
      DisconnectAfterCommand();
    }
    return 0;
  }
  ESP_LOGW(kTag, "Pixel %08lx has no supported GATT service",
           static_cast<unsigned long>(command.pixel_id));
  DisconnectAfterCommand();
  return 0;
}

void DiscoverService() {
  command.service_start = 0;
  command.service_end = 0;
  command.write_handle = 0;
  command.notify_handle = 0;
  command.cccd_handle = 0;
  command.legacy = false;
  const int result = ble_gattc_disc_svc_by_uuid(
      command.connection_handle, &kModernService.u, ServiceDiscovered,
      nullptr);
  if (result != 0) {
    ESP_LOGW(kTag, "Unable to discover Pixels service: %d", result);
    DisconnectAfterCommand();
  }
}

int MtuExchanged(uint16_t, const ble_gatt_error *, uint16_t, void *) {
  DiscoverService();
  return 0;
}

void BeginCommand(ble_npl_event *) {
  QueuedCommand queued;
  if (xQueueReceive(command_queue, &queued, 0) != pdTRUE) {
    command_pipeline_active.store(false);
    if (uxQueueMessagesWaiting(command_queue) > 0) {
      ScheduleCommandIfIdle();
    } else if (host_ready.load()) {
      StartScan();
    }
    return;
  }

  command = {};
  command.type = queued.type;
  command.pixel_id = queued.pixel_id;
  command.connection_handle = kInvalidConnectionHandle;

  bool found = false;
  for (const auto &entry : pending) {
    if (entry.used && entry.data.has_identity &&
        entry.data.pixel_id == command.pixel_id) {
      command.address = entry.address;
      found = true;
      break;
    }
  }
  if (!found) {
    ESP_LOGW(kTag, "No recent BLE address for Pixel %08lx",
             static_cast<unsigned long>(command.pixel_id));
    CompleteCommand();
    return;
  }

  const int cancel_result = ble_gap_disc_cancel();
  if (cancel_result != 0 && cancel_result != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "Unable to stop scan for %s: %d",
             CommandName(command.type), cancel_result);
    CompleteCommand();
    return;
  }
  const int connect_result =
      ble_gap_connect(own_address_type, &command.address, 10000, nullptr,
                      GapEvent, nullptr);
  if (connect_result != 0) {
    ESP_LOGW(kTag, "Unable to connect to Pixel %08lx for %s: %d",
             static_cast<unsigned long>(command.pixel_id),
             CommandName(command.type), connect_result);
    CompleteCommand();
  }
}

bool ParseLegacyInfo(const uint8_t *data, std::size_t length,
                     ConnectedDieInfo *info) {
  if (length != 22) {
    return false;
  }
  info->colorway = data[2];
  info->type = static_cast<pixels::DieType>(data[3]);
  info->profile_hash = ReadLe32(&data[4]);
  info->pixel_id = ReadLe32(&data[8]);
  info->available_flash = ReadLe16(&data[12]);
  info->firmware_timestamp = ReadLe32(&data[14]);
  info->roll_state = static_cast<pixels::RollState>(data[18]);
  info->face_index = data[19];
  info->battery = data[20];
  info->battery_state = data[21];
  return true;
}

bool NextChunk(const uint8_t *data, std::size_t length, std::size_t *offset,
               const uint8_t **chunk, std::size_t *chunk_size) {
  if (*offset >= length || data[*offset] == 0 ||
      *offset + data[*offset] > length) {
    return false;
  }
  *chunk = &data[*offset];
  *chunk_size = data[*offset];
  *offset += *chunk_size;
  return true;
}

bool ParseModernInfo(const uint8_t *data, std::size_t length,
                     ConnectedDieInfo *info) {
  std::size_t offset = 1;
  const uint8_t *chunk = nullptr;
  std::size_t chunk_size = 0;
  if (!NextChunk(data, length, &offset, &chunk, &chunk_size) ||
      chunk_size < 15) {
    return false;
  }
  info->firmware_version = ReadLe16(&chunk[1]);
  info->firmware_timestamp = ReadLe32(&chunk[3]);

  if (!NextChunk(data, length, &offset, &chunk, &chunk_size) ||
      chunk_size < 10) {
    return false;
  }
  info->pixel_id = ReadLe32(&chunk[1]);
  info->type = static_cast<pixels::DieType>(chunk[6]);
  info->colorway = chunk[8];

  if (!NextChunk(data, length, &offset, &chunk, &chunk_size)) {
    return false;
  }
  if (!NextChunk(data, length, &offset, &chunk, &chunk_size)) {
    return false;
  }
  const std::size_t name_length =
      std::min<std::size_t>(chunk_size - 1, sizeof(info->name) - 1);
  std::memcpy(info->name, &chunk[1], name_length);
  info->name[name_length] = '\0';
  info->has_name = name_length > 0;

  if (!NextChunk(data, length, &offset, &chunk, &chunk_size) ||
      chunk_size < 13) {
    return false;
  }
  info->profile_hash = ReadLe32(&chunk[1]);
  info->available_flash = ReadLe32(&chunk[5]);

  if (!NextChunk(data, length, &offset, &chunk, &chunk_size) ||
      chunk_size < 5) {
    return false;
  }
  info->battery = chunk[1];
  info->battery_state = chunk[2];
  info->roll_state = static_cast<pixels::RollState>(chunk[3]);
  info->face_index = chunk[4];
  return true;
}

void HandleNotification(const uint8_t *data, std::size_t length) {
  if (length == 0 || command.type != CommandType::kRefreshInfo) {
    return;
  }
  if (data[0] == kIAmADieMessage) {
    ConnectedDieInfo info;
    if ((ParseLegacyInfo(data, length, &info) ||
         ParseModernInfo(data, length, &info)) &&
        info.pixel_id == command.pixel_id) {
      model->UpdateConnectedInfo(
          info, static_cast<uint64_t>(esp_timer_get_time() / 1000));
      command.responses |= kInfoResponse;
      ESP_LOGI(kTag, "Updated connected info for Pixel %08lx",
               static_cast<unsigned long>(command.pixel_id));
    } else {
      ESP_LOGW(kTag, "Invalid identity response from Pixel %08lx (%u bytes)",
               static_cast<unsigned long>(command.pixel_id),
               static_cast<unsigned>(length));
    }
  } else if (data[0] == kTemperatureMessage && length >= 5) {
    model->UpdateTemperature(command.pixel_id, ReadLeI16(&data[1]),
                             ReadLeI16(&data[3]));
    command.responses |= kTemperatureResponse;
    ESP_LOGI(kTag, "Updated temperature for Pixel %08lx",
             static_cast<unsigned long>(command.pixel_id));
  }

  if ((command.responses & (kInfoResponse | kTemperatureResponse)) ==
      (kInfoResponse | kTemperatureResponse)) {
    DisconnectAfterCommand();
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
  } else if (event->type == BLE_GAP_EVENT_CONNECT &&
             command_pipeline_active.load()) {
    if (event->connect.status != 0) {
      ESP_LOGW(kTag, "Unable to connect for Pixel %s: %d",
               CommandName(command.type), event->connect.status);
      CompleteCommand();
      return 0;
    }
    command.connection_handle = event->connect.conn_handle;
    const int result =
        ble_gattc_exchange_mtu(command.connection_handle, MtuExchanged, nullptr);
    if (result != 0) {
      DiscoverService();
    }
  } else if (event->type == BLE_GAP_EVENT_NOTIFY_RX &&
             command_pipeline_active.load() &&
             command.type == CommandType::kRefreshInfo &&
             event->notify_rx.conn_handle == command.connection_handle &&
             event->notify_rx.attr_handle == command.notify_handle) {
    std::array<uint8_t, 160> data{};
    const uint16_t length =
        std::min<uint16_t>(OS_MBUF_PKTLEN(event->notify_rx.om), data.size());
    if (os_mbuf_copydata(event->notify_rx.om, 0, length, data.data()) == 0) {
      HandleNotification(data.data(), length);
    }
  } else if (event->type == BLE_GAP_EVENT_DISCONNECT &&
             command_pipeline_active.load() &&
             event->disconnect.conn.conn_handle ==
                 command.connection_handle) {
    CompleteCommand();
  } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE &&
             host_ready.load() && !command_pipeline_active.load()) {
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

esp_err_t QueueCommand(CommandType type, uint32_t pixel_id) {
  if (pixel_id == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!host_ready.load()) {
    return ESP_ERR_INVALID_STATE;
  }
  const QueuedCommand queued = {
      .type = type,
      .pixel_id = pixel_id,
  };
  if (xQueueSend(command_queue, &queued, 0) != pdTRUE) {
    return ESP_ERR_NO_MEM;
  }
  command_count.fetch_add(1);
  ScheduleCommandIfIdle();
  return ESP_OK;
}

} // namespace

esp_err_t StartPixelsScanner(DiceModel *dice_model) {
  if (dice_model == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  model = dice_model;
  command_queue = xQueueCreate(kCommandQueueSize, sizeof(QueuedCommand));
  if (command_queue == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  ESP_RETURN_ON_ERROR(nimble_port_init(), kTag, "NimBLE init failed");
  ble_hs_cfg.sync_cb = OnSync;
  ble_npl_event_init(&command_event, BeginCommand, nullptr);
  ble_npl_callout_init(&command_timeout, nimble_port_get_dflt_eventq(),
                       CommandTimedOut, nullptr);
  nimble_port_freertos_init(HostTask);
  return ESP_OK;
}

void SetPixelsScannerWifiActive(bool active) {
  if (wifi_active.exchange(active) == active || !host_ready.load() ||
      command_pipeline_active.load()) {
    return;
  }
  const int result = ble_gap_disc_cancel();
  if (result != 0 && result != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "Unable to cancel BLE scan for duty change: %d", result);
  }
  StartScan();
}

esp_err_t RequestPixelsBlink(uint32_t pixel_id) {
  return QueueCommand(CommandType::kBlink, pixel_id);
}

esp_err_t RequestPixelsInfo(uint32_t pixel_id) {
  return QueueCommand(CommandType::kRefreshInfo, pixel_id);
}

bool IsPixelsBlinkBusy() { return command_count.load() > 0; }

} // namespace app

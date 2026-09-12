#include "ui.h"

#include <algorithm>
#include <cstdio>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pixels_ble.h"
#include "preferences.h"
#include "web_server.h"

namespace app {
namespace {

constexpr uint32_t kBackground = 0x0b1020;
constexpr uint32_t kPanel = 0x172033;
constexpr uint32_t kText = 0xf8fafc;
constexpr uint32_t kMuted = 0x94a3b8;
constexpr uint32_t kAccent = 0x2563eb;
constexpr uint32_t kStaleMs = 15000;
constexpr char kTag[] = "pixels_ui";

board::Hardware *hardware;
DiceModel *model;
lv_obj_t *root;
lv_obj_t *content;
lv_obj_t *aggregate;
lv_obj_t *aggregate_mode;
lv_obj_t *aggregate_value;
lv_obj_t *aggregate_clear;
lv_obj_t *count_label;
lv_obj_t *network_status_label;
lv_obj_t *pair_page;
lv_obj_t *history_page;
lv_obj_t *history_status_label;
uint32_t history_pixel_id;
std::size_t rendered_history_count;
uint64_t rendered_history_timestamp;
uint32_t rendered_revision;
uint64_t last_dashboard_refresh_ms;
board::Orientation orientation = board::Orientation::kPortrait;

lv_color_t Color(uint32_t value) { return lv_color_hex(value); }

void AggregateClicked(lv_event_t *) {
  model->CycleAggregate();
  SavePreferences(*model);
}

void AggregateCleared(lv_event_t *) {
  model->ClearAggregate();
}

void BlinkDie(uint32_t pixel_id) {
  const esp_err_t result = RequestPixelsBlink(pixel_id);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "Unable to blink Pixel %08lx: %s",
             static_cast<unsigned long>(pixel_id), esp_err_to_name(result));
  }
}

void RefreshDieInfo(uint32_t pixel_id) {
  const esp_err_t result = RequestPixelsInfo(pixel_id);
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "Unable to refresh Pixel %08lx info: %s",
             static_cast<unsigned long>(pixel_id), esp_err_to_name(result));
  }
}

void WifiToggle(lv_event_t *) {
  const bool enable = !IsWebNetworkEnabled();
  const esp_err_t result = SetWebNetworkEnabled(model, enable);
  SetPixelsScannerWifiActive(IsWebNetworkEnabled());
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Unable to %s Wi-Fi: %s",
             enable ? "enable" : "disable", esp_err_to_name(result));
    return;
  }
  rendered_revision = 0;
}

uint32_t StateColor(pixels::RollState state) {
  switch (state) {
  case pixels::RollState::kRolling:
    return 0x38bdf8;
  case pixels::RollState::kHandling:
    return 0xf59e0b;
  case pixels::RollState::kCrooked:
    return 0xf87171;
  case pixels::RollState::kRolled:
    return 0x4ade80;
  case pixels::RollState::kOnFace:
    return 0xa78bfa;
  case pixels::RollState::kUnknown:
  default:
    return kMuted;
  }
}

uint32_t StateBackground(pixels::RollState state) {
  switch (state) {
  case pixels::RollState::kRolling:
    return 0x102b3f;
  case pixels::RollState::kHandling:
    return 0x302716;
  case pixels::RollState::kCrooked:
    return 0x321820;
  case pixels::RollState::kRolled:
    return 0x173322;
  default:
    return kPanel;
  }
}

void ClosePairing(lv_event_t *) {
  lv_obj_add_flag(pair_page, LV_OBJ_FLAG_HIDDEN);
}

void PairAction(lv_event_t *event) {
  const uint32_t pixel_id = static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  const Snapshot snapshot =
      model->GetSnapshot(static_cast<uint64_t>(esp_timer_get_time() / 1000));
  bool paired = false;
  for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
    if (snapshot.dice[i].pixel_id == pixel_id) {
      paired = true;
      break;
    }
  }
  const bool changed =
      paired ? model->Unpair(pixel_id) : model->Pair(pixel_id);
  if (changed && !paired) {
    BlinkDie(pixel_id);
    RefreshDieInfo(pixel_id);
  }
  SavePreferences(*model);
  rendered_revision = 0;
}

void OpenPairing(lv_event_t *) {
  lv_obj_remove_flag(pair_page, LV_OBJ_FLAG_HIDDEN);
  rendered_revision = 0;
}

lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    uint32_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, Color(color), 0);
  return label;
}

void StyleCard(lv_obj_t *object, uint32_t border) {
  lv_obj_set_style_bg_color(object, Color(kPanel), 0);
  lv_obj_set_style_border_color(object, Color(border), 0);
  lv_obj_set_style_border_width(object, 1, 0);
  lv_obj_set_style_radius(object, 8, 0);
  lv_obj_set_style_pad_all(object, 6, 0);
}

void CloseHistory(lv_event_t *) {
  BlinkDie(history_pixel_id);
  history_pixel_id = 0;
  rendered_history_count = 0;
  rendered_history_timestamp = 0;
  lv_obj_add_flag(history_page, LV_OBJ_FLAG_HIDDEN);
}

void UpdateHistoryStatus(const Die *die) {
  if (history_status_label == nullptr) {
    return;
  }
  if (die == nullptr) {
    lv_label_set_text(history_status_label, "WAITING FOR DIE STATUS");
    return;
  }
  char status[160];
  if (die->has_connected_info && die->has_temperature) {
    std::snprintf(
        status, sizeof(status),
        "%u%% %s  %s  %ddBm\nFW %u  SET %08lx\nMCU %.1fC  BAT %.1fC",
        die->battery, die->charging ? "YES" : "NO",
        pixels::RollStateName(die->roll_state), die->rssi,
        die->firmware_version,
        static_cast<unsigned long>(die->profile_hash),
        die->mcu_temperature_centi_c / 100.0,
        die->battery_temperature_centi_c / 100.0);
  } else if (die->has_connected_info) {
    std::snprintf(status, sizeof(status),
                  "%u%% %s  %s  %ddBm\nFW %u  SET %08lx",
                  die->battery, die->charging ? "YES" : "NO",
                  pixels::RollStateName(die->roll_state), die->rssi,
                  die->firmware_version,
                  static_cast<unsigned long>(die->profile_hash));
  } else {
    std::snprintf(status, sizeof(status),
                  "BAT %u%%  CHARGING %s\n%s  RSSI %ddBm\nREFRESHING DETAILS",
                  die->battery, die->charging ? "YES" : "NO",
                  pixels::RollStateName(die->roll_state), die->rssi);
  }
  lv_label_set_text(history_status_label, status);
}

void BuildHistory(uint32_t pixel_id, uint64_t now_ms) {
  const Snapshot snapshot = model->GetSnapshot(now_ms);
  const Die *selected = nullptr;
  for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
    if (snapshot.dice[i].pixel_id == pixel_id) {
      selected = &snapshot.dice[i];
      break;
    }
  }
  const RollHistory history = model->GetRollHistory(pixel_id);
  rendered_history_count = history.count;
  rendered_history_timestamp =
      history.count == 0 ? 0 : history.events[0].timestamp_ms;

  history_status_label = nullptr;
  lv_obj_clean(history_page);
  lv_obj_set_size(history_page, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(history_page, Color(kBackground), 0);
  lv_obj_set_style_pad_all(history_page, 8, 0);

  char heading[48] = "DIE HISTORY";
  if (selected != nullptr) {
    std::snprintf(heading, sizeof(heading), "%s %s",
                  pixels::DieTypeName(selected->type),
                  selected->name[0] == '\0' ? "Pixel" : selected->name);
  }
  lv_obj_t *title =
      MakeLabel(history_page, heading, &lv_font_montserrat_14, kText);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);
  lv_obj_set_width(title, std::max(lv_obj_get_width(history_page) - 70, 70L));
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

  lv_obj_t *done = lv_button_create(history_page);
  lv_obj_set_size(done, 54, 30);
  lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_set_style_bg_color(done, Color(kAccent), 0);
  lv_obj_add_event_cb(done, CloseHistory, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *done_label = MakeLabel(done, "BACK", &lv_font_montserrat_12, kText);
  lv_obj_center(done_label);

  history_status_label =
      MakeLabel(history_page, "", &lv_font_montserrat_12, kMuted);
  lv_obj_set_pos(history_status_label, 2, 24);
  lv_obj_set_width(history_status_label,
                   std::max(lv_obj_get_width(history_page) - 16, 80L));
  lv_label_set_long_mode(history_status_label, LV_LABEL_LONG_WRAP);
  UpdateHistoryStatus(selected);

  lv_obj_t *list = lv_obj_create(history_page);
  lv_obj_set_size(list, lv_pct(100),
                  std::max<lv_coord_t>(lv_obj_get_height(history_page) - 84,
                                       60));
  lv_obj_set_pos(list, 0, 78);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER);

  if (history.count == 0) {
    lv_obj_t *empty =
        MakeLabel(list, "NO COMPLETED ROLLS", &lv_font_montserrat_14, kMuted);
    lv_obj_set_width(empty, lv_pct(100));
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    return;
  }

  for (std::size_t i = 0; i < history.count; ++i) {
    const RollEvent &event = history.events[i];
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 36);
    StyleCard(row, 0x334155);

    char number[8];
    std::snprintf(number, sizeof(number), "#%u",
                  static_cast<unsigned>(i + 1));
    lv_obj_t *index =
        MakeLabel(row, number, &lv_font_montserrat_12, kMuted);
    lv_obj_align(index, LV_ALIGN_LEFT_MID, 0, 0);

    char age[32];
    std::snprintf(age, sizeof(age), "%llus ago",
                  static_cast<unsigned long long>(
                      (now_ms - event.timestamp_ms) / 1000));
    lv_obj_t *time = MakeLabel(row, age, &lv_font_montserrat_12, kMuted);
    lv_obj_align(time, LV_ALIGN_CENTER, 0, 0);

    char value[8];
    std::snprintf(value, sizeof(value), "%d", event.value);
    lv_obj_t *roll =
        MakeLabel(row, value, &lv_font_montserrat_18, kText);
    lv_obj_align(roll, LV_ALIGN_RIGHT_MID, 0, 0);
  }
}

void OpenHistory(lv_event_t *event) {
  history_pixel_id = static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  BlinkDie(history_pixel_id);
  RefreshDieInfo(history_pixel_id);
  BuildHistory(
      history_pixel_id,
      static_cast<uint64_t>(esp_timer_get_time() / 1000));
  lv_obj_remove_flag(history_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(history_page);
}

void BuildPairing(const Snapshot &snapshot) {
  lv_obj_clean(pair_page);
  lv_obj_set_style_bg_color(pair_page, Color(kBackground), 0);
  lv_obj_set_style_pad_all(pair_page, 8, 0);

  lv_obj_t *title =
      MakeLabel(pair_page, "PAIR DICE", &lv_font_montserrat_14, kText);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);
  lv_obj_t *done = lv_button_create(pair_page);
  lv_obj_set_size(done, 54, 30);
  lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_set_style_bg_color(done, Color(kAccent), 0);
  lv_obj_add_event_cb(done, ClosePairing, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *done_label = MakeLabel(done, "DONE", &lv_font_montserrat_12, kText);
  lv_obj_center(done_label);

  const NetworkStatus network_status = GetWebNetworkStatus();
  const bool network_enabled = network_status != NetworkStatus::kOff;
  lv_obj_t *wifi = lv_button_create(pair_page);
  lv_obj_set_size(wifi, lv_pct(100), 30);
  lv_obj_set_pos(wifi, 0, 34);
  lv_obj_set_style_bg_color(wifi,
                            Color(network_enabled ? 0x166534 : 0x334155), 0);
  lv_obj_add_event_cb(wifi, WifiToggle, LV_EVENT_CLICKED, nullptr);
  char wifi_text[48];
  std::snprintf(wifi_text, sizeof(wifi_text), "WIFI %s - TAP TO %s",
                NetworkStatusName(network_status),
                network_enabled ? "DISABLE" : "ENABLE");
  lv_obj_t *wifi_label =
      MakeLabel(wifi, wifi_text, &lv_font_montserrat_12, kText);
  lv_obj_center(wifi_label);

  lv_obj_t *list = lv_obj_create(pair_page);
  const int page_height = lv_obj_get_height(pair_page);
  lv_obj_set_size(list, lv_pct(100), std::max(page_height - 76, 60));
  lv_obj_set_pos(list, 0, 70);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER);

  auto add_row = [list](const Die &die, bool paired) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    StyleCard(row, paired ? 0x22c55e : 0x38bdf8);

    char heading[32];
    std::snprintf(heading, sizeof(heading), "%s  %s",
                  pixels::DieTypeName(die.type),
                  die.name[0] == '\0' ? "Pixel" : die.name);
    lv_obj_t *name = MakeLabel(row, heading, &lv_font_montserrat_12, kText);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    char detail[40];
    std::snprintf(detail, sizeof(detail), "%08lx  %ddBm",
                  static_cast<unsigned long>(die.pixel_id), die.rssi);
    lv_obj_t *info = MakeLabel(row, detail, &lv_font_montserrat_12, kMuted);
    lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *button = lv_button_create(row);
    lv_obj_set_size(button, 54, 30);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(button, Color(paired ? 0x334155 : kAccent), 0);
    lv_obj_add_event_cb(
        button, PairAction, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(die.pixel_id)));
    lv_obj_t *button_label = MakeLabel(button, paired ? "REMOVE" : "ADD",
                                       &lv_font_montserrat_12, kText);
    lv_obj_center(button_label);
  };

  for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
    add_row(snapshot.dice[i], true);
  }
  for (std::size_t i = 0; i < snapshot.candidate_count; ++i) {
    add_row(snapshot.candidates[i], false);
  }
}

void TileGeometry(std::size_t count, bool landscape, int *columns, int *rows) {
  if (landscape) {
    *columns = count <= 1   ? 1
               : count <= 2 ? 2
               : count <= 4 ? 2
               : count <= 6 ? 3
                            : 4;
  } else {
    *columns = count <= 2 ? 1 : 2;
  }
  *rows = static_cast<int>((count + *columns - 1) / *columns);
}

void BuildDashboard(const Snapshot &snapshot, uint64_t now_ms) {
  lv_obj_clean(content);
  char count[8];
  std::snprintf(count, sizeof(count), "%u",
                static_cast<unsigned>(snapshot.dice_count));
  lv_label_set_text(count_label, count);

  const bool landscape = orientation == board::Orientation::kLandscapeLeft ||
                         orientation == board::Orientation::kLandscapeRight;
  const int width = lv_obj_get_width(content);
  const int height = lv_obj_get_height(content);
  int columns = 1;
  int rows = 1;
  TileGeometry(snapshot.dice_count == 0 ? 1 : snapshot.dice_count, landscape,
               &columns, &rows);
  const int gap = 4;
  const int tile_width = (width - gap * (columns - 1)) / columns;
  const int tile_height = (height - gap * (rows - 1)) / rows;

  if (snapshot.dice_count == 0) {
    lv_obj_t *empty = lv_obj_create(content);
    lv_obj_set_size(empty, tile_width, tile_height);
    StyleCard(empty, 0x334155);
    lv_obj_t *title =
        MakeLabel(empty, "NO DICE", &lv_font_montserrat_24, kText);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -18);
    lv_obj_t *hint =
        MakeLabel(empty, "Tap + to pair", &lv_font_montserrat_14, kMuted);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 18);
    return;
  }

  constexpr uint32_t accents[] = {0x8b5cf6, 0x38bdf8, 0xf59e0b, 0x22c55e,
                                  0xec4899, 0x06b6d4, 0xf97316, 0x84cc16};
  for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
    const Die &die = snapshot.dice[i];
    const int column = static_cast<int>(i) % columns;
    const int row = static_cast<int>(i) / columns;
    lv_obj_t *tile = lv_obj_create(content);
    lv_obj_set_size(tile, tile_width, tile_height);
    lv_obj_set_pos(tile, column * (tile_width + gap),
                   row * (tile_height + gap));
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        tile, OpenHistory, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(die.pixel_id)));
    const bool stale =
        die.last_seen_ms == 0 || now_ms - die.last_seen_ms > kStaleMs;
    const uint32_t state_color = StateColor(die.roll_state);
    StyleCard(tile, stale ? 0x64748b : state_color);
    if (!stale) {
      lv_obj_set_style_bg_color(tile, Color(StateBackground(die.roll_state)),
                                0);
      lv_obj_set_style_border_width(tile, 2, 0);
    }
    if (stale) {
      lv_obj_set_style_opa(tile, LV_OPA_60, 0);
    }

    lv_obj_t *type =
        MakeLabel(tile, pixels::DieTypeName(die.type), &lv_font_montserrat_14,
                  stale ? kMuted : accents[i]);
    lv_obj_align(type, LV_ALIGN_TOP_LEFT, 0, 0);
    char battery[10];
    std::snprintf(battery, sizeof(battery), stale ? "OFF" : "%u%%",
                  die.battery);
    lv_obj_t *battery_label = MakeLabel(tile, battery, &lv_font_montserrat_12,
                                        die.battery < 15 ? 0xf87171 : kMuted);
    lv_obj_align(battery_label, LV_ALIGN_TOP_RIGHT, 0, 0);

    char value[8] = "-";
    if (die.has_roll) {
      std::snprintf(value, sizeof(value), "%d", die.last_roll);
    }
    const lv_font_t *value_font = tile_height >= 90   ? &lv_font_montserrat_48
                                  : tile_height >= 60 ? &lv_font_montserrat_32
                                                      : &lv_font_montserrat_24;
    lv_obj_t *roll = MakeLabel(tile, value, value_font, kText);
    lv_obj_align(roll, LV_ALIGN_CENTER, 0, tile_height >= 90 ? 2 : -1);
    lv_obj_t *state =
        MakeLabel(tile, stale ? "OFFLINE"
                              : pixels::RollStateName(die.roll_state),
                  &lv_font_montserrat_12, stale ? kMuted : state_color);
    lv_obj_align(state, LV_ALIGN_BOTTOM_MID, 0, 0);
  }

  const char *mode = snapshot.aggregate_mode == AggregateMode::kSum ? "SUM"
                     : snapshot.aggregate_mode == AggregateMode::kHigh
                         ? "HIGH"
                         : "LOW";
  char mode_text[24];
  std::snprintf(mode_text, sizeof(mode_text), "%s >  %luR", mode,
                static_cast<unsigned long>(snapshot.aggregate_roll_count));
  lv_label_set_text(aggregate_mode, mode_text);
  char value[24] = "-";
  if (snapshot.has_aggregate) {
    std::snprintf(value, sizeof(value), "%lld",
                  static_cast<long long>(snapshot.aggregate_value));
  }
  lv_label_set_text(aggregate_value, value);
}

void Relayout() {
  const bool landscape = orientation == board::Orientation::kLandscapeLeft ||
                         orientation == board::Orientation::kLandscapeRight;
  const int width = landscape ? 320 : 172;
  const int height = landscape ? 172 : 320;
  const int header_height = 26;
  const int aggregate_size = landscape ? 78 : 56;

  lv_obj_set_size(root, width, height);
  if (landscape) {
    lv_obj_set_size(content, width - aggregate_size - 12,
                    height - header_height - 8);
    lv_obj_set_pos(content, 6, header_height);
    lv_obj_set_size(aggregate, aggregate_size - 6, height - header_height - 8);
    lv_obj_set_pos(aggregate, width - aggregate_size, header_height);
    lv_obj_align(aggregate_mode, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_align(aggregate_value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(aggregate_clear, LV_ALIGN_BOTTOM_MID, 0, 0);
  } else {
    lv_obj_set_size(content, width - 16,
                    height - header_height - aggregate_size - 12);
    lv_obj_set_pos(content, 8, header_height);
    lv_obj_set_size(aggregate, width - 16, aggregate_size - 4);
    lv_obj_set_pos(aggregate, 8, height - aggregate_size);
    lv_obj_align(aggregate_mode, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(aggregate_value, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_align(aggregate_clear, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }
  rendered_revision = 0;
}

void UiTimer(lv_timer_t *) {
  const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
  const NetworkStatus network_status = GetWebNetworkStatus();
  const char *short_status =
      network_status == NetworkStatus::kConnected  ? "NET"
      : network_status == NetworkStatus::kConnecting ? "..."
      : network_status == NetworkStatus::kAccessPoint ? "AP"
                                                     : "OFF";
  lv_label_set_text(network_status_label, short_status);
  lv_obj_set_style_text_color(
      network_status_label,
      Color(network_status == NetworkStatus::kConnected
                ? 0x4ade80
                : network_status == NetworkStatus::kConnecting
                      ? 0xf59e0b
                      : network_status == NetworkStatus::kAccessPoint
                            ? 0x38bdf8
                            : kMuted),
      0);
  const Snapshot snapshot = model->GetSnapshot(now_ms);
  const bool periodic_refresh = now_ms - last_dashboard_refresh_ms >= 1000;
  if (snapshot.revision != rendered_revision || periodic_refresh) {
    BuildDashboard(snapshot, now_ms);
    if (!lv_obj_has_flag(pair_page, LV_OBJ_FLAG_HIDDEN)) {
      BuildPairing(snapshot);
    }
    rendered_revision = snapshot.revision;
    last_dashboard_refresh_ms = now_ms;
  }
  if (!lv_obj_has_flag(history_page, LV_OBJ_FLAG_HIDDEN)) {
    const Die *selected = nullptr;
    for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
      if (snapshot.dice[i].pixel_id == history_pixel_id) {
        selected = &snapshot.dice[i];
        break;
      }
    }
    UpdateHistoryStatus(selected);
    const RollHistory history = model->GetRollHistory(history_pixel_id);
    const uint64_t newest_timestamp =
        history.count == 0 ? 0 : history.events[0].timestamp_ms;
    if (history.count != rendered_history_count ||
        newest_timestamp != rendered_history_timestamp) {
      BuildHistory(history_pixel_id, now_ms);
    }
  }
}

void OrientationTask(void *) {
  board::Orientation candidate = orientation;
  int stable_samples = 0;
  while (true) {
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    if (board::ReadAcceleration(hardware, &x, &y, &z)) {
      const board::Orientation detected =
          board::DetectOrientation(x, y, orientation);
      if (detected == candidate) {
        ++stable_samples;
      } else {
        candidate = detected;
        stable_samples = 1;
      }
      if (candidate != orientation && stable_samples >= 8 &&
          lvgl_port_lock(1000)) {
        orientation = candidate;
        board::SetOrientation(hardware, orientation);
        Relayout();
        lvgl_port_unlock();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

} // namespace

esp_err_t StartUi(board::Hardware *board_hardware, DiceModel *dice_model) {
  hardware = board_hardware;
  model = dice_model;

  if (!lvgl_port_lock(1000)) {
    return ESP_ERR_TIMEOUT;
  }
  root = lv_screen_active();
  lv_obj_set_style_bg_color(root, Color(kBackground), 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);

  lv_obj_t *title = MakeLabel(root, "PIXELS", &lv_font_montserrat_14, kText);
  lv_obj_set_pos(title, 8, 6);
  count_label = MakeLabel(root, "0", &lv_font_montserrat_12, kMuted);
  lv_obj_align(count_label, LV_ALIGN_TOP_RIGHT, -32, 8);
  network_status_label =
      MakeLabel(root, "OFF", &lv_font_montserrat_12, kMuted);
  lv_obj_align(network_status_label, LV_ALIGN_TOP_MID, 10, 8);
  lv_obj_t *pair_button = lv_button_create(root);
  lv_obj_set_size(pair_button, 28, 24);
  lv_obj_align(pair_button, LV_ALIGN_TOP_RIGHT, -4, 2);
  lv_obj_set_style_bg_color(pair_button, Color(0x334155), 0);
  lv_obj_add_event_cb(pair_button, OpenPairing, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *plus = MakeLabel(pair_button, "+", &lv_font_montserrat_18, kText);
  lv_obj_center(plus);

  content = lv_obj_create(root);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0);

  aggregate = lv_obj_create(root);
  lv_obj_set_style_bg_color(aggregate, Color(kAccent), 0);
  lv_obj_set_style_border_width(aggregate, 0, 0);
  lv_obj_set_style_radius(aggregate, 10, 0);
  lv_obj_add_flag(aggregate, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(aggregate, AggregateClicked, LV_EVENT_CLICKED, nullptr);
  aggregate_mode =
      MakeLabel(aggregate, "SUM  >", &lv_font_montserrat_14, 0xbfdbfe);
  lv_obj_align(aggregate_mode, LV_ALIGN_TOP_MID, 0, 0);
  aggregate_value = MakeLabel(aggregate, "-", &lv_font_montserrat_32, kText);
  lv_obj_align(aggregate_value, LV_ALIGN_CENTER, 0, 2);
  aggregate_clear = lv_button_create(aggregate);
  lv_obj_set_size(aggregate_clear, 52, 22);
  lv_obj_align(aggregate_clear, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(aggregate_clear, Color(0x991b1b), 0);
  lv_obj_add_event_cb(aggregate_clear, AggregateCleared, LV_EVENT_CLICKED,
                      nullptr);
  lv_obj_t *clear_label =
      MakeLabel(aggregate_clear, "CLEAR", &lv_font_montserrat_12, kText);
  lv_obj_center(clear_label);

  pair_page = lv_obj_create(root);
  lv_obj_set_size(pair_page, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(pair_page, 0, 0);
  lv_obj_add_flag(pair_page, LV_OBJ_FLAG_HIDDEN);

  history_page = lv_obj_create(root);
  lv_obj_set_size(history_page, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(history_page, 0, 0);
  lv_obj_add_flag(history_page, LV_OBJ_FLAG_HIDDEN);

  Relayout();
  lv_timer_create(UiTimer, 250, nullptr);
  lvgl_port_unlock();

  xTaskCreate(OrientationTask, "orientation", 3072, nullptr, 3, nullptr);
  return ESP_OK;
}

} // namespace app

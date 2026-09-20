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
constexpr uint32_t kDimmed = 0x334155;
constexpr uint32_t kAccent = 0x2563eb;
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
lv_obj_t *stats_page;
lv_obj_t *calculator_page;
lv_obj_t *mode_page;
lv_obj_t *stats_status_label;
uint32_t history_pixel_id;
std::size_t rendered_history_count;
uint64_t rendered_history_timestamp;
uint32_t rendered_revision;
uint64_t last_dashboard_refresh_ms;
uint64_t pair_page_opened_ms;
uint64_t rendered_pairing_signature;
uint32_t rendered_calculator_revision;
bool calculator_defaults_initialized;
NetworkStatus rendered_pairing_network_status = NetworkStatus::kOff;
board::Orientation orientation = board::Orientation::kPortrait;

void OpenStats(lv_event_t *event);
void BuildCalculator();
void OpenCalculator(lv_event_t *event);

lv_color_t Color(uint32_t value) { return lv_color_hex(value); }

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

void CloseCalculator(lv_event_t *) {
  model->CancelCalculator();
  lv_obj_add_flag(calculator_page, LV_OBJ_FLAG_HIDDEN);
}

void CalculatorStart(lv_event_t *) {
  model->StartCalculator();
  BuildCalculator();
}

void CalculatorAdjust(lv_event_t *event) {
  const uintptr_t encoded =
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const auto setting =
      static_cast<CalculatorSetting>((encoded >> 8U) & 0xffU);
  const int8_t delta = static_cast<int8_t>(encoded & 0xffU);
  model->AdjustCalculator(setting, delta);
  BuildCalculator();
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
  const uint64_t now_ms =
      static_cast<uint64_t>(esp_timer_get_time() / 1000);
  if (now_ms - pair_page_opened_ms < 300) {
    return;
  }
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

void StylePage(lv_obj_t *page) {
  lv_obj_set_size(page, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(page, Color(kBackground), 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_pad_all(page, 8, 0);
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
}

void CloseModePage(lv_event_t *) {
  lv_obj_add_flag(mode_page, LV_OBJ_FLAG_HIDDEN);
}

void ModeSelected(lv_event_t *event) {
  const uintptr_t selection =
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  lv_obj_add_flag(mode_page, LV_OBJ_FLAG_HIDDEN);
  if (selection <= static_cast<uintptr_t>(AggregateMode::kLow)) {
    model->RestoreAggregate(static_cast<AggregateMode>(selection));
    SavePreferences(*model);
    return;
  }

  const auto preset =
      static_cast<CalculatorPreset>(selection - 10U);
  const CalculatorSnapshot snapshot = model->GetCalculatorSnapshot();
  model->AdjustCalculator(
      CalculatorSetting::kPreset,
      static_cast<int>(preset) - static_cast<int>(snapshot.config.preset));
  OpenCalculator(nullptr);
}

void AddModeOption(lv_obj_t *page, int y, const char *label,
                   uintptr_t selection, bool selected) {
  lv_obj_t *button = lv_button_create(page);
  lv_obj_set_size(button, lv_pct(100), 36);
  lv_obj_set_pos(button, 0, y);
  lv_obj_set_style_bg_color(button, Color(selected ? kAccent : kPanel), 0);
  lv_obj_add_event_cb(button, ModeSelected, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(selection));
  lv_obj_t *text =
      MakeLabel(button, label, &lv_font_montserrat_12, kText);
  lv_obj_align(text, LV_ALIGN_LEFT_MID, 4, 0);
}

void BuildModePage() {
  const Snapshot dashboard = model->GetSnapshot(
      static_cast<uint64_t>(esp_timer_get_time() / 1000));
  const CalculatorSnapshot calculator = model->GetCalculatorSnapshot();
  lv_obj_clean(mode_page);
  StylePage(mode_page);
  lv_obj_add_flag(mode_page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(mode_page, LV_DIR_VER);

  lv_obj_t *title =
      MakeLabel(mode_page, "RESULT / GAME", &lv_font_montserrat_14, kText);
  lv_obj_set_pos(title, 2, 6);
  lv_obj_t *back = lv_button_create(mode_page);
  lv_obj_set_size(back, 54, 30);
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(back, Color(0x334155), 0);
  lv_obj_add_event_cb(back, CloseModePage, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *back_label =
      MakeLabel(back, "BACK", &lv_font_montserrat_12, kText);
  lv_obj_center(back_label);

  int y = 44;
  lv_obj_t *results =
      MakeLabel(mode_page, "ROLL RESULTS", &lv_font_montserrat_12, kMuted);
  lv_obj_set_pos(results, 2, y);
  y += 22;
  AddModeOption(mode_page, y, "SUM", 0,
                dashboard.aggregate_mode == AggregateMode::kSum);
  y += 40;
  AddModeOption(mode_page, y, "HIGHEST", 1,
                dashboard.aggregate_mode == AggregateMode::kHigh);
  y += 40;
  AddModeOption(mode_page, y, "LOWEST", 2,
                dashboard.aggregate_mode == AggregateMode::kLow);
  y += 48;

  lv_obj_t *guided =
      MakeLabel(mode_page, "GUIDED ROLLS", &lv_font_montserrat_12, kMuted);
  lv_obj_set_pos(guided, 2, y);
  y += 22;
  for (int preset = static_cast<int>(CalculatorPreset::kD20Check);
       preset <= static_cast<int>(CalculatorPreset::kDamagePool); ++preset) {
    const auto value = static_cast<CalculatorPreset>(preset);
    AddModeOption(mode_page, y, CalculatorPresetName(value),
                  static_cast<uintptr_t>(preset) + 10U,
                  calculator.config.preset == value);
    y += 40;
  }
}

void AggregateClicked(lv_event_t *) {
  BuildModePage();
  lv_obj_remove_flag(mode_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(mode_page);
}

void MakeBatteryIndicator(lv_obj_t *parent, const Die &die, bool stale) {
  const uint32_t color = stale       ? 0x64748b
                         : die.charging ? 0x4ade80
                         : die.battery < 15 ? 0xf87171
                                            : kMuted;
  lv_obj_t *body = lv_obj_create(parent);
  lv_obj_set_size(body, 18, 10);
  lv_obj_align(body, LV_ALIGN_TOP_RIGHT, -4, 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 1, 0);
  lv_obj_set_style_border_color(body, Color(color), 0);
  lv_obj_set_style_radius(body, 2, 0);
  lv_obj_set_style_pad_all(body, 0, 0);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *fill = lv_obj_create(body);
  const int fill_width = (14 * die.battery + 99) / 100;
  lv_obj_set_size(fill, std::max(fill_width, 1), 6);
  lv_obj_align(fill, LV_ALIGN_LEFT_MID, 1, 0);
  lv_obj_set_style_bg_color(fill, Color(color), 0);
  lv_obj_set_style_bg_opa(fill, die.battery == 0 ? LV_OPA_TRANSP : LV_OPA_COVER,
                         0);
  lv_obj_set_style_border_width(fill, 0, 0);
  lv_obj_set_style_radius(fill, 1, 0);
  lv_obj_set_style_pad_all(fill, 0, 0);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *terminal = lv_obj_create(parent);
  lv_obj_set_size(terminal, 3, 6);
  lv_obj_align(terminal, LV_ALIGN_TOP_RIGHT, 0, 2);
  lv_obj_set_style_bg_color(terminal, Color(color), 0);
  lv_obj_set_style_border_width(terminal, 0, 0);
  lv_obj_set_style_radius(terminal, 1, 0);
  lv_obj_set_style_pad_all(terminal, 0, 0);
  lv_obj_remove_flag(terminal, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(terminal, LV_OBJ_FLAG_CLICKABLE);
}

void CloseHistory(lv_event_t *) {
  BlinkDie(history_pixel_id);
  lv_obj_add_flag(stats_page, LV_OBJ_FLAG_HIDDEN);
  history_pixel_id = 0;
  rendered_history_count = 0;
  rendered_history_timestamp = 0;
  lv_obj_add_flag(history_page, LV_OBJ_FLAG_HIDDEN);
}

void CloseStats(lv_event_t *) {
  lv_obj_add_flag(stats_page, LV_OBJ_FLAG_HIDDEN);
}

void UnpairFromStats(lv_event_t *) {
  if (history_pixel_id != 0 && model->Unpair(history_pixel_id)) {
    SavePreferences(*model);
  }
  history_pixel_id = 0;
  rendered_history_count = 0;
  rendered_history_timestamp = 0;
  rendered_revision = 0;
  lv_obj_add_flag(stats_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(history_page, LV_OBJ_FLAG_HIDDEN);
}

void UpdateStatsStatus(const Die *die) {
  if (stats_status_label == nullptr) {
    return;
  }
  if (die == nullptr) {
    lv_label_set_text(stats_status_label, "DIE NOT AVAILABLE");
    return;
  }
  char status[224];
  if (die->has_connected_info && die->has_temperature) {
    std::snprintf(
        status, sizeof(status),
        "ID %08lx\nBATTERY %u%%  %s\nSTATE %s\nSIGNAL %ddBm\n"
        "FIRMWARE %u\nPROFILE %08lx\nFLASH %lu bytes\n"
        "MCU %.1fC  BAT %.1fC",
        static_cast<unsigned long>(die->pixel_id),
        die->battery, die->charging ? "YES" : "NO",
        pixels::RollStateName(die->roll_state), die->rssi,
        die->firmware_version,
        static_cast<unsigned long>(die->profile_hash),
        static_cast<unsigned long>(die->available_flash),
        die->mcu_temperature_centi_c / 100.0,
        die->battery_temperature_centi_c / 100.0);
  } else if (die->has_connected_info) {
    std::snprintf(
        status, sizeof(status),
        "ID %08lx\nBATTERY %u%%  %s\nSTATE %s\nSIGNAL %ddBm\n"
        "FIRMWARE %u\nPROFILE %08lx\nFLASH %lu bytes",
                  static_cast<unsigned long>(die->pixel_id),
                  die->battery, die->charging ? "YES" : "NO",
                  pixels::RollStateName(die->roll_state), die->rssi,
                  die->firmware_version,
                  static_cast<unsigned long>(die->profile_hash),
                  static_cast<unsigned long>(die->available_flash));
  } else {
    std::snprintf(
        status, sizeof(status),
        "ID %08lx\nBATTERY %u%%  %s\nSTATE %s\nSIGNAL %ddBm\n"
        "REFRESHING DETAILS",
                  static_cast<unsigned long>(die->pixel_id),
                  die->battery, die->charging ? "YES" : "NO",
                  pixels::RollStateName(die->roll_state), die->rssi);
  }
  lv_label_set_text(stats_status_label, status);
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

  lv_obj_clean(history_page);
  StylePage(history_page);

  char heading[32] = "ROLLS";
  if (selected != nullptr) {
    std::snprintf(heading, sizeof(heading), "%s ROLLS",
                  pixels::DieTypeName(selected->type));
  }
  lv_obj_t *title =
      MakeLabel(history_page, heading, &lv_font_montserrat_12, kText);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);
  lv_obj_set_width(title, std::max(lv_obj_get_width(history_page) - 106, 48L));
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

  lv_obj_t *done = lv_button_create(history_page);
  lv_obj_set_size(done, 46, 30);
  lv_obj_align(done, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_set_style_bg_color(done, Color(kAccent), 0);
  lv_obj_add_event_cb(done, CloseHistory, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *done_label = MakeLabel(done, "BACK", &lv_font_montserrat_12, kText);
  lv_obj_center(done_label);

  lv_obj_t *info = lv_button_create(history_page);
  lv_obj_set_size(info, 46, 30);
  lv_obj_align(info, LV_ALIGN_TOP_RIGHT, -50, -2);
  lv_obj_set_style_bg_color(info, Color(0x334155), 0);
  lv_obj_add_event_cb(info, OpenStats, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *info_label =
      MakeLabel(info, "INFO", &lv_font_montserrat_12, kText);
  lv_obj_center(info_label);

  lv_obj_t *list = lv_obj_create(history_page);
  lv_obj_set_size(list, lv_pct(100),
                  std::max<lv_coord_t>(lv_obj_get_height(history_page) - 44,
                                       60));
  lv_obj_set_pos(list, 0, 38);
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

void BuildStats(uint32_t pixel_id, uint64_t now_ms) {
  const Snapshot snapshot = model->GetSnapshot(now_ms);
  const Die *selected = nullptr;
  for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
    if (snapshot.dice[i].pixel_id == pixel_id) {
      selected = &snapshot.dice[i];
      break;
    }
  }

  stats_status_label = nullptr;
  lv_obj_clean(stats_page);
  StylePage(stats_page);

  lv_obj_t *title =
      MakeLabel(stats_page, "DIE INFO", &lv_font_montserrat_14, kText);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);

  lv_obj_t *back = lv_button_create(stats_page);
  lv_obj_set_size(back, 54, 30);
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_set_style_bg_color(back, Color(kAccent), 0);
  lv_obj_add_event_cb(back, CloseStats, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *back_label =
      MakeLabel(back, "BACK", &lv_font_montserrat_12, kText);
  lv_obj_center(back_label);

  lv_obj_t *details = lv_obj_create(stats_page);
  lv_obj_set_size(details, lv_pct(100),
                  std::max<lv_coord_t>(lv_obj_get_height(stats_page) - 86,
                                       50));
  lv_obj_set_pos(details, 0, 38);
  lv_obj_set_style_bg_opa(details, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(details, 0, 0);
  lv_obj_set_style_pad_all(details, 2, 0);

  stats_status_label =
      MakeLabel(details, "", &lv_font_montserrat_12, kMuted);
  lv_obj_set_width(stats_status_label, lv_pct(100));
  lv_label_set_long_mode(stats_status_label, LV_LABEL_LONG_WRAP);
  UpdateStatsStatus(selected);

  lv_obj_t *unpair = lv_button_create(stats_page);
  lv_obj_set_size(unpair, lv_pct(100), 36);
  lv_obj_align(unpair, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(unpair, Color(0x991b1b), 0);
  lv_obj_add_event_cb(unpair, UnpairFromStats, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *unpair_label =
      MakeLabel(unpair, "UNPAIR DIE", &lv_font_montserrat_12, kText);
  lv_obj_center(unpair_label);
}

void OpenStats(lv_event_t *) {
  if (history_pixel_id == 0) {
    return;
  }
  RefreshDieInfo(history_pixel_id);
  BuildStats(history_pixel_id,
             static_cast<uint64_t>(esp_timer_get_time() / 1000));
  lv_obj_remove_flag(stats_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(stats_page);
}

void OpenHistory(lv_event_t *event) {
  history_pixel_id = static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  BlinkDie(history_pixel_id);
  BuildHistory(
      history_pixel_id,
      static_cast<uint64_t>(esp_timer_get_time() / 1000));
  lv_obj_remove_flag(history_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(history_page);
}

uint64_t PairingSignature(const Snapshot &snapshot) {
  uint64_t signature = 1469598103934665603ULL;
  auto mix = [&signature](uint32_t value) {
    signature ^= value;
    signature *= 1099511628211ULL;
  };
  mix(static_cast<uint32_t>(snapshot.dice_count));
  for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
    mix(snapshot.dice[i].pixel_id);
  }
  mix(0xffffffffU);
  mix(static_cast<uint32_t>(snapshot.candidate_count));
  for (std::size_t i = 0; i < snapshot.candidate_count; ++i) {
    mix(snapshot.candidates[i].pixel_id);
  }
  return signature;
}

void BuildPairing(const Snapshot &snapshot) {
  rendered_pairing_signature = PairingSignature(snapshot);
  rendered_pairing_network_status = GetWebNetworkStatus();
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

  const NetworkStatus network_status = rendered_pairing_network_status;
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

    char detail[24];
    std::snprintf(detail, sizeof(detail), "SIGNAL %ddBm", die.rssi);
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

void OpenPairing(lv_event_t *) {
  const uint64_t now_ms =
      static_cast<uint64_t>(esp_timer_get_time() / 1000);
  const Snapshot snapshot = model->GetSnapshot(now_ms);
  BuildPairing(snapshot);
  pair_page_opened_ms = now_ms;
  lv_obj_remove_flag(pair_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(pair_page);
}

void AddCalculatorControl(lv_obj_t *parent, int y, const char *label,
                          const char *value, CalculatorSetting setting,
                          bool toggle = false) {
  const int width = lv_obj_get_content_width(parent);
  lv_obj_t *caption =
      MakeLabel(parent, label, &lv_font_montserrat_12, kMuted);
  lv_obj_set_pos(caption, 2, y + 10);

  if (!toggle) {
    lv_obj_t *minus = lv_button_create(parent);
    lv_obj_set_size(minus, 34, 32);
    lv_obj_set_pos(minus, width - 108, y);
    lv_obj_set_style_bg_color(minus, Color(0x334155), 0);
    const uintptr_t encoded =
        (static_cast<uintptr_t>(setting) << 8U) | 0xffU;
    lv_obj_add_event_cb(minus, CalculatorAdjust, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(encoded));
    lv_obj_t *minus_label =
        MakeLabel(minus, "-", &lv_font_montserrat_18, kText);
    lv_obj_center(minus_label);
  }

  lv_obj_t *value_button = lv_button_create(parent);
  lv_obj_set_size(value_button, toggle ? 72 : 34, 32);
  lv_obj_set_pos(value_button, width - (toggle ? 74 : 70), y);
  lv_obj_set_style_bg_color(value_button, Color(toggle ? kAccent : kPanel),
                            0);
  if (toggle) {
    const uintptr_t encoded =
        (static_cast<uintptr_t>(setting) << 8U) | 1U;
    lv_obj_add_event_cb(value_button, CalculatorAdjust, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(encoded));
  }
  lv_obj_t *value_label =
      MakeLabel(value_button, value, &lv_font_montserrat_12, kText);
  lv_obj_center(value_label);

  if (!toggle) {
    lv_obj_t *plus = lv_button_create(parent);
    lv_obj_set_size(plus, 34, 32);
    lv_obj_set_pos(plus, width - 34, y);
    lv_obj_set_style_bg_color(plus, Color(0x334155), 0);
    const uintptr_t encoded =
        (static_cast<uintptr_t>(setting) << 8U) | 1U;
    lv_obj_add_event_cb(plus, CalculatorAdjust, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(encoded));
    lv_obj_t *plus_label =
        MakeLabel(plus, "+", &lv_font_montserrat_18, kText);
    lv_obj_center(plus_label);
  }
}

void BuildCalculator() {
  rendered_calculator_revision = model->Revision();
  const CalculatorSnapshot snapshot = model->GetCalculatorSnapshot();
  lv_obj_clean(calculator_page);
  StylePage(calculator_page);
  const bool landscape = orientation == board::Orientation::kLandscapeLeft ||
                         orientation == board::Orientation::kLandscapeRight;
  if (landscape) {
    lv_obj_add_flag(calculator_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(calculator_page, LV_DIR_VER);
  }

  lv_obj_t *title =
      MakeLabel(calculator_page, "CALCULATOR", &lv_font_montserrat_14, kText);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 2);
  lv_obj_t *back = lv_button_create(calculator_page);
  lv_obj_set_size(back, 52, 30);
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_set_style_bg_color(back, Color(0x334155), 0);
  lv_obj_add_event_cb(back, CloseCalculator, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *back_label =
      MakeLabel(back, snapshot.status == CalculatorStatus::kRolling
                          ? "CANCEL"
                          : "BACK",
                &lv_font_montserrat_12, kText);
  lv_obj_center(back_label);

  if (snapshot.status == CalculatorStatus::kRolling ||
      snapshot.status == CalculatorStatus::kComplete) {
    lv_obj_t *preset =
        MakeLabel(calculator_page, CalculatorPresetName(snapshot.config.preset),
                  &lv_font_montserrat_12, 0x60a5fa);
    lv_obj_set_pos(preset, 2, 38);

    lv_obj_t *instruction =
        MakeLabel(calculator_page, snapshot.instruction,
                  &lv_font_montserrat_14, kText);
    lv_obj_set_width(instruction, lv_pct(100));
    lv_obj_set_pos(instruction, 2, 62);
    lv_label_set_long_mode(instruction, LV_LABEL_LONG_WRAP);

    char progress[48];
    std::snprintf(progress, sizeof(progress), "%u of %u rolls complete",
                  snapshot.completed_rolls, snapshot.total_rolls);
    lv_obj_t *progress_label =
        MakeLabel(calculator_page, progress, &lv_font_montserrat_12, kMuted);
    lv_obj_set_pos(progress_label, 2, 116);

    if (snapshot.status == CalculatorStatus::kComplete) {
      lv_obj_t *result =
          MakeLabel(calculator_page, snapshot.result_text,
                    &lv_font_montserrat_32,
                    snapshot.has_check && !snapshot.check_passed
                        ? 0xf87171
                        : 0x4ade80);
      lv_obj_set_width(result, lv_pct(100));
      lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_CENTER, 0);
      if (landscape) {
        lv_obj_set_pos(result, 0, 140);
      } else {
        lv_obj_align(result, LV_ALIGN_CENTER, 0, 18);
      }

      lv_obj_t *again = lv_button_create(calculator_page);
      lv_obj_set_size(again, lv_pct(100), 40);
      if (landscape) {
        lv_obj_set_pos(again, 0, 184);
      } else {
        lv_obj_align(again, LV_ALIGN_BOTTOM_MID, 0, 0);
      }
      lv_obj_set_style_bg_color(again, Color(kAccent), 0);
      lv_obj_add_event_cb(again, CalculatorStart, LV_EVENT_CLICKED, nullptr);
      lv_obj_t *again_label =
          MakeLabel(again, "ROLL AGAIN", &lv_font_montserrat_14, kText);
      lv_obj_center(again_label);
    }
    return;
  }

  lv_obj_t *preset = lv_obj_create(calculator_page);
  lv_obj_set_size(preset, lv_pct(100), 40);
  lv_obj_set_pos(preset, 0, 36);
  lv_obj_set_style_bg_color(preset, Color(kAccent), 0);
  lv_obj_set_style_border_width(preset, 0, 0);
  lv_obj_set_style_radius(preset, 8, 0);
  lv_obj_set_style_pad_all(preset, 0, 0);
  lv_obj_remove_flag(preset, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *preset_label =
      MakeLabel(preset, CalculatorPresetName(snapshot.config.preset),
                &lv_font_montserrat_12, kText);
  lv_obj_center(preset_label);

  int y = 84;
  char value[20];
  const auto add_number = [&](const char *label, int number,
                              CalculatorSetting setting) {
    std::snprintf(value, sizeof(value), "%d", number);
    AddCalculatorControl(calculator_page, y, label, value, setting);
    y += 38;
  };
  if (snapshot.config.preset == CalculatorPreset::kD20Check) {
    add_number(landscape ? "MODIFIER" : "MOD", snapshot.config.modifier,
               CalculatorSetting::kModifier);
    std::snprintf(value, sizeof(value), snapshot.config.dc_enabled ? "DC %u"
                                                                  : "OFF",
                  snapshot.config.dc);
    AddCalculatorControl(calculator_page, y, "TARGET", value,
                         CalculatorSetting::kDcEnabled, true);
    y += 38;
    if (snapshot.config.dc_enabled) {
      add_number(landscape ? "DC VALUE" : "DC", snapshot.config.dc,
                 CalculatorSetting::kDc);
    }
  } else if (snapshot.config.preset == CalculatorPreset::kAdvantage ||
             snapshot.config.preset == CalculatorPreset::kDisadvantage) {
    add_number(landscape ? "MODIFIER" : "MOD", snapshot.config.modifier,
               CalculatorSetting::kModifier);
  } else {
    AddCalculatorControl(calculator_page, y, "DIE",
                         CalculatorPoolDieName(snapshot.config.pool_sides),
                         CalculatorSetting::kPoolSides, true);
    y += 38;
    add_number("ROLLS", snapshot.config.pool_count,
               CalculatorSetting::kPoolCount);
    if (snapshot.config.preset == CalculatorPreset::kSuccessPool) {
      add_number(landscape ? "SUCCESS ON" : "TARGET",
                 snapshot.config.threshold,
                 CalculatorSetting::kThreshold);
    } else if (snapshot.config.preset == CalculatorPreset::kKeepHighest ||
               snapshot.config.preset == CalculatorPreset::kKeepLowest) {
      add_number("KEEP", snapshot.config.keep_count,
                 CalculatorSetting::kKeepCount);
    } else {
      add_number(landscape ? "MODIFIER" : "MOD",
                 snapshot.config.modifier,
                 CalculatorSetting::kModifier);
      AddCalculatorControl(calculator_page, y,
                           landscape ? "CRITICAL" : "CRIT",
                           snapshot.config.critical ? "ON" : "OFF",
                           CalculatorSetting::kCritical, true);
      y += 38;
    }
  }

  if (snapshot.status == CalculatorStatus::kError) {
    lv_obj_t *error =
        MakeLabel(calculator_page, snapshot.instruction,
                  &lv_font_montserrat_12, 0xf87171);
    lv_obj_set_width(error, lv_pct(100));
    lv_obj_set_pos(error, 2, std::min(y + 2, 242));
    lv_label_set_long_mode(error, LV_LABEL_LONG_WRAP);
  }

  lv_obj_t *start = lv_button_create(calculator_page);
  lv_obj_set_size(start, lv_pct(100), 40);
  if (landscape) {
    lv_obj_set_pos(start, 0, y + 6);
  } else {
    lv_obj_align(start, LV_ALIGN_BOTTOM_MID, 0, 0);
  }
  lv_obj_set_style_bg_color(start, Color(0x166534), 0);
  lv_obj_add_event_cb(start, CalculatorStart, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *start_label =
      MakeLabel(start, "START ROUND", &lv_font_montserrat_14, kText);
  lv_obj_center(start_label);
}

void OpenCalculator(lv_event_t *) {
  if (!calculator_defaults_initialized) {
    const Snapshot snapshot = model->GetSnapshot(
        static_cast<uint64_t>(esp_timer_get_time() / 1000));
    if (snapshot.dice_count > 1) {
      model->AdjustCalculator(
          CalculatorSetting::kPoolCount,
          static_cast<int>(snapshot.dice_count) - 1);
    }
    calculator_defaults_initialized = true;
  }
  BuildCalculator();
  lv_obj_remove_flag(calculator_page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(calculator_page);
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
  std::array<std::size_t, kMaxDice> active_indices{};
  std::size_t active_count = 0;
  for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
    const Die &die = snapshot.dice[i];
    if (die.last_activity_ms != 0 &&
        now_ms - die.last_activity_ms <= kDieActiveMs) {
      active_indices[active_count++] = i;
    }
  }

  char count[12];
  std::snprintf(count, sizeof(count), "%u/%u",
                static_cast<unsigned>(active_count),
                static_cast<unsigned>(snapshot.dice_count));
  lv_label_set_text(count_label, count);

  const bool landscape = orientation == board::Orientation::kLandscapeLeft ||
                         orientation == board::Orientation::kLandscapeRight;
  const int width = lv_obj_get_width(content);
  const int height = lv_obj_get_height(content);
  int columns = 1;
  int rows = 1;
  TileGeometry(active_count == 0 ? 1 : active_count, landscape,
               &columns, &rows);
  const int gap = 4;
  const int tile_width = (width - gap * (columns - 1)) / columns;
  const int tile_height = (height - gap * (rows - 1)) / rows;

  if (active_count == 0) {
    lv_obj_t *empty = lv_obj_create(content);
    lv_obj_set_size(empty, tile_width, tile_height);
    StyleCard(empty, 0x334155);
    lv_obj_add_flag(empty, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(empty, OpenPairing, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *title =
        MakeLabel(empty, "NO ACTIVE DICE", &lv_font_montserrat_18, kText);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -32);
    char summary[40];
    std::snprintf(summary, sizeof(summary), "%u PAIRED  |  %u FOUND",
                  static_cast<unsigned>(snapshot.dice_count),
                  static_cast<unsigned>(snapshot.candidate_count));
    lv_obj_t *summary_label =
        MakeLabel(empty, summary, &lv_font_montserrat_12, kMuted);
    lv_obj_align(summary_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *hint = MakeLabel(empty, "HANDLE A DIE\nOR TAP TO PAIR",
                               &lv_font_montserrat_12, kMuted);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 32);
    return;
  }

  constexpr uint32_t accents[] = {0x8b5cf6, 0x38bdf8, 0xf59e0b, 0x22c55e,
                                  0xec4899, 0x06b6d4, 0xf97316, 0x84cc16};
  for (std::size_t i = 0; i < active_count; ++i) {
    const Die &die = snapshot.dice[active_indices[i]];
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
        die.last_seen_ms == 0 || now_ms - die.last_seen_ms > kDieOfflineMs;
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
    MakeBatteryIndicator(tile, die, stale);

    char value[8] = "-";
    if (die.has_roll) {
      std::snprintf(value, sizeof(value), "%d", die.last_roll);
    }
    const lv_font_t *value_font =
        tile_height >= 72 && tile_width >= 72 ? &lv_font_montserrat_48
                                              : &lv_font_montserrat_32;
    const bool awaiting_roll =
        die.roll_state == pixels::RollState::kHandling ||
        die.roll_state == pixels::RollState::kRolling;
    lv_obj_t *roll =
        MakeLabel(tile, value, value_font, awaiting_roll ? kDimmed : kText);
    lv_obj_set_style_text_outline_stroke_color(roll, Color(kBackground), 0);
    lv_obj_set_style_text_outline_stroke_width(roll, 1, 0);
    lv_obj_set_style_text_outline_stroke_opa(roll, LV_OPA_COVER, 0);
    lv_obj_align(roll, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *state_ring = lv_obj_create(tile);
    lv_obj_set_size(state_ring, 12, 12);
    lv_obj_set_style_radius(state_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(state_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state_ring, 3, 0);
    lv_obj_set_style_border_color(
        state_ring, Color(stale ? 0x64748b : state_color), 0);
    lv_obj_set_style_pad_all(state_ring, 0, 0);
    lv_obj_remove_flag(state_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(state_ring, LV_ALIGN_BOTTOM_LEFT, 0, 0);
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
  const int aggregate_size = landscape ? 78 : 66;

  lv_obj_set_size(root, width, height);
  if (landscape) {
    lv_obj_set_size(content, width - aggregate_size - 12,
                    height - header_height - 8);
    lv_obj_set_pos(content, 6, header_height);
    lv_obj_set_size(aggregate, aggregate_size - 6, height - header_height - 8);
    lv_obj_set_pos(aggregate, width - aggregate_size, header_height);
    lv_obj_set_size(aggregate_clear, 52, 22);
    lv_obj_align(aggregate_mode, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_align(aggregate_value, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(aggregate_clear, LV_ALIGN_BOTTOM_MID, 0, 0);
  } else {
    lv_obj_set_size(content, width - 16,
                    height - header_height - aggregate_size - 12);
    lv_obj_set_pos(content, 8, header_height);
    lv_obj_set_size(aggregate, width - 16, aggregate_size - 4);
    lv_obj_set_pos(aggregate, 8, height - aggregate_size);
    lv_obj_set_size(aggregate_clear, 42, 48);
    lv_obj_align(aggregate_mode, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_align(aggregate_value, LV_ALIGN_BOTTOM_LEFT, 4, 2);
    lv_obj_align(aggregate_clear, LV_ALIGN_RIGHT_MID, 0, 0);
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
  const bool model_changed = snapshot.revision != rendered_revision;
  const bool periodic_refresh = now_ms - last_dashboard_refresh_ms >= 1000;
  if (model_changed || periodic_refresh) {
    BuildDashboard(snapshot, now_ms);
    rendered_revision = snapshot.revision;
    last_dashboard_refresh_ms = now_ms;
  }
  if (!lv_obj_has_flag(pair_page, LV_OBJ_FLAG_HIDDEN) &&
      (PairingSignature(snapshot) != rendered_pairing_signature ||
       network_status != rendered_pairing_network_status)) {
    BuildPairing(snapshot);
  }
  if (!lv_obj_has_flag(history_page, LV_OBJ_FLAG_HIDDEN)) {
    const RollHistory history = model->GetRollHistory(history_pixel_id);
    const uint64_t newest_timestamp =
        history.count == 0 ? 0 : history.events[0].timestamp_ms;
    if (history.count != rendered_history_count ||
        newest_timestamp != rendered_history_timestamp) {
      BuildHistory(history_pixel_id, now_ms);
    }
  }
  if (!lv_obj_has_flag(stats_page, LV_OBJ_FLAG_HIDDEN)) {
    const Die *selected = nullptr;
    for (std::size_t i = 0; i < snapshot.dice_count; ++i) {
      if (snapshot.dice[i].pixel_id == history_pixel_id) {
        selected = &snapshot.dice[i];
        break;
      }
    }
    UpdateStatsStatus(selected);
  }
  if (!lv_obj_has_flag(calculator_page, LV_OBJ_FLAG_HIDDEN) &&
      snapshot.revision != rendered_calculator_revision) {
    BuildCalculator();
  }
}

void OrientationTask(void *) {
  board::Orientation candidate = orientation;
  int stable_samples = 0;
  bool logged_first_sample = false;
  while (true) {
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    if (board::ReadAcceleration(hardware, &x, &y, &z)) {
      const board::Orientation detected =
          board::DetectOrientation(x, y, orientation);
      if (!logged_first_sample) {
        ESP_LOGI(kTag, "IMU acceleration x=%d y=%d z=%d orientation=%u", x, y,
                 z, static_cast<unsigned>(detected));
        logged_first_sample = true;
      }
      if (detected == candidate) {
        ++stable_samples;
      } else {
        candidate = detected;
        stable_samples = 1;
      }
      if (candidate != orientation && stable_samples >= 8 &&
          lvgl_port_lock(1000)) {
        orientation = candidate;
        ESP_LOGI(kTag, "Display orientation changed to %u (x=%d y=%d z=%d)",
                 static_cast<unsigned>(orientation), x, y, z);
        board::SetOrientation(hardware, orientation);
        Relayout();
        if (!lv_obj_has_flag(calculator_page, LV_OBJ_FLAG_HIDDEN)) {
          BuildCalculator();
        }
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
  board::SetOrientation(hardware, orientation);
  root = lv_screen_active();
  lv_obj_set_style_bg_color(root, Color(kBackground), 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);

  lv_obj_t *title = MakeLabel(root, "PIXELS", &lv_font_montserrat_14, kText);
  lv_obj_set_pos(title, 8, 6);
  count_label = MakeLabel(root, "0", &lv_font_montserrat_12, kMuted);
  lv_obj_align(count_label, LV_ALIGN_TOP_RIGHT, -42, 8);
  network_status_label =
      MakeLabel(root, "OFF", &lv_font_montserrat_12, kMuted);
  lv_obj_align(network_status_label, LV_ALIGN_TOP_MID, 10, 8);
  lv_obj_t *pair_button = lv_button_create(root);
  lv_obj_set_size(pair_button, 36, 26);
  lv_obj_align(pair_button, LV_ALIGN_TOP_RIGHT, -2, 0);
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
  lv_obj_set_style_pad_all(aggregate, 6, 0);
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
      MakeLabel(aggregate_clear, "CLR", &lv_font_montserrat_12, kText);
  lv_obj_center(clear_label);

  pair_page = lv_obj_create(root);
  lv_obj_set_size(pair_page, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(pair_page, 0, 0);
  lv_obj_add_flag(pair_page, LV_OBJ_FLAG_HIDDEN);

  history_page = lv_obj_create(root);
  lv_obj_set_size(history_page, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(history_page, 0, 0);
  lv_obj_add_flag(history_page, LV_OBJ_FLAG_HIDDEN);

  stats_page = lv_obj_create(root);
  lv_obj_set_size(stats_page, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(stats_page, 0, 0);
  lv_obj_add_flag(stats_page, LV_OBJ_FLAG_HIDDEN);

  calculator_page = lv_obj_create(root);
  lv_obj_set_size(calculator_page, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(calculator_page, 0, 0);
  lv_obj_add_flag(calculator_page, LV_OBJ_FLAG_HIDDEN);

  mode_page = lv_obj_create(root);
  lv_obj_set_size(mode_page, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(mode_page, 0, 0);
  lv_obj_add_flag(mode_page, LV_OBJ_FLAG_HIDDEN);

  Relayout();
  lv_timer_create(UiTimer, 250, nullptr);
  lvgl_port_unlock();

  xTaskCreate(OrientationTask, "orientation", 3072, nullptr, 3, nullptr);
  return ESP_OK;
}

} // namespace app

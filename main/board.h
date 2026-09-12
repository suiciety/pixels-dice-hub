#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

namespace board {

constexpr int kPortraitWidth = 172;
constexpr int kPortraitHeight = 320;

enum class Orientation : uint8_t {
  kPortrait = 0,
  kLandscapeRight = 1,
  kPortraitInverted = 2,
  kLandscapeLeft = 3,
};

struct Hardware {
  i2c_master_bus_handle_t i2c_bus = nullptr;
  esp_lcd_panel_io_handle_t panel_io = nullptr;
  esp_lcd_panel_handle_t panel = nullptr;
  esp_lcd_touch_handle_t touch = nullptr;
  lv_display_t *display = nullptr;
};

esp_err_t Init(Hardware *hardware);
bool ReadAcceleration(Hardware *hardware, int16_t *x, int16_t *y, int16_t *z);
void SetOrientation(Hardware *hardware, Orientation orientation);
Orientation DetectOrientation(int16_t x, int16_t y,
                              Orientation current_orientation);

} // namespace board

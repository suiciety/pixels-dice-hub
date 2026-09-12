#include "board.h"

#include <cmath>
#include <cstring>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_jd9853.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_axs5106.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {
namespace {

constexpr char kTag[] = "board";

constexpr gpio_num_t kLcdSclk = GPIO_NUM_1;
constexpr gpio_num_t kLcdMosi = GPIO_NUM_2;
constexpr gpio_num_t kLcdCs = GPIO_NUM_14;
constexpr gpio_num_t kLcdDc = GPIO_NUM_15;
constexpr gpio_num_t kLcdReset = GPIO_NUM_22;
constexpr gpio_num_t kBacklight = GPIO_NUM_23;

constexpr gpio_num_t kI2cSda = GPIO_NUM_18;
constexpr gpio_num_t kI2cScl = GPIO_NUM_19;
constexpr gpio_num_t kTouchReset = GPIO_NUM_20;
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_21;

constexpr uint8_t kQmiAddress = 0x6b;
constexpr uint8_t kQmiWhoAmI = 0x00;
constexpr uint8_t kQmiCtrl1 = 0x02;
constexpr uint8_t kQmiCtrl2 = 0x03;
constexpr uint8_t kQmiCtrl7 = 0x08;
constexpr uint8_t kQmiStatus0 = 0x2e;
constexpr uint8_t kQmiAccelXLow = 0x35;
constexpr uint8_t kQmiReset = 0x60;
constexpr int16_t kOrientationThreshold = 6000;
constexpr int16_t kOrientationHysteresis = 1200;

i2c_master_dev_handle_t qmi_handle;

esp_err_t WriteRegister(i2c_master_dev_handle_t device, uint8_t reg,
                        uint8_t value) {
  const uint8_t data[] = {reg, value};
  return i2c_master_transmit(device, data, sizeof(data), 100);
}

esp_err_t InitI2c(Hardware *hardware) {
  i2c_master_bus_config_t config = {};
  config.i2c_port = I2C_NUM_0;
  config.sda_io_num = kI2cSda;
  config.scl_io_num = kI2cScl;
  config.clk_source = I2C_CLK_SRC_DEFAULT;
  config.glitch_ignore_cnt = 7;
  config.flags.enable_internal_pullup = true;
  return i2c_new_master_bus(&config, &hardware->i2c_bus);
}

esp_err_t InitImu(Hardware *hardware) {
  const i2c_device_config_t config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = kQmiAddress,
      .scl_speed_hz = 400000,
      .scl_wait_us = 0,
      .flags = {.disable_ack_check = false},
  };
  ESP_RETURN_ON_ERROR(
      i2c_master_bus_add_device(hardware->i2c_bus, &config, &qmi_handle), kTag,
      "Unable to add QMI8658");

  uint8_t id = 0;
  ESP_RETURN_ON_ERROR(
      i2c_master_transmit_receive(qmi_handle, &kQmiWhoAmI, 1, &id, 1, 100),
      kTag, "Unable to read QMI8658 ID");
  if (id != 0x05) {
    ESP_LOGE(kTag, "Unexpected QMI8658 ID: 0x%02x", id);
    return ESP_ERR_NOT_FOUND;
  }

  ESP_RETURN_ON_ERROR(WriteRegister(qmi_handle, kQmiReset, 0xb0), kTag,
                      "Unable to reset QMI8658");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(WriteRegister(qmi_handle, kQmiCtrl1, 0x40), kTag,
                      "Unable to configure QMI8658");
  ESP_RETURN_ON_ERROR(WriteRegister(qmi_handle, kQmiCtrl2, 0x95), kTag,
                      "Unable to configure accelerometer");
  ESP_RETURN_ON_ERROR(WriteRegister(qmi_handle, kQmiCtrl7, 0x01), kTag,
                      "Unable to enable accelerometer");
  return ESP_OK;
}

esp_err_t InitDisplay(Hardware *hardware) {
  constexpr std::size_t kBufferPixels = kPortraitWidth * 40;
  spi_bus_config_t bus = {};
  bus.mosi_io_num = kLcdMosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = kLcdSclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kBufferPixels * sizeof(uint16_t);
  ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                      kTag, "Unable to initialize LCD SPI bus");

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = kLcdCs;
  io_config.dc_gpio_num = kLcdDc;
  io_config.spi_mode = 0;
  io_config.pclk_hz = 40000000;
  io_config.trans_queue_depth = 10;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                          static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST),
                          &io_config, &hardware->panel_io),
                      kTag, "Unable to create LCD panel IO");

  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = kLcdReset,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
      .bits_per_pixel = 16,
      .flags = {.reset_active_high = false},
      .vendor_config = nullptr,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9853(hardware->panel_io,
                                               &panel_config, &hardware->panel),
                      kTag, "Unable to create JD9853 panel");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(hardware->panel), kTag,
                      "Unable to reset LCD");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(hardware->panel), kTag,
                      "Unable to initialize LCD");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(hardware->panel, true), kTag,
                      "Unable to set LCD inversion");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(hardware->panel, 34, 0), kTag,
                      "Unable to set LCD offset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(hardware->panel, true), kTag,
                      "Unable to enable LCD");
  return ESP_OK;
}

esp_err_t InitTouch(Hardware *hardware) {
  const i2c_device_config_t device_config = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = ESP_LCD_TOUCH_IO_I2C_AXS5106_ADDRESS,
      .scl_speed_hz = 400000,
      .scl_wait_us = 0,
      .flags = {.disable_ack_check = false},
  };
  i2c_master_dev_handle_t device = nullptr;
  ESP_RETURN_ON_ERROR(
      i2c_master_bus_add_device(hardware->i2c_bus, &device_config, &device),
      kTag, "Unable to add AXS5106");

  const esp_lcd_touch_config_t touch_config = {
      .x_max = kPortraitWidth,
      .y_max = kPortraitHeight,
      .rst_gpio_num = kTouchReset,
      .int_gpio_num = kTouchInterrupt,
      .levels = {.reset = 0, .interrupt = 0},
      .flags = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
      .process_coordinates = nullptr,
      .interrupt_callback = nullptr,
      .user_data = nullptr,
      .driver_data = nullptr,
  };
  return esp_lcd_touch_new_i2c_axs5106(device, &touch_config, &hardware->touch);
}

esp_err_t InitBacklight() {
  const ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = 5000,
      .clk_cfg = LEDC_AUTO_CLK,
      .deconfigure = false,
  };
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag,
                      "Unable to configure backlight timer");
  ledc_channel_config_t channel = {};
  channel.gpio_num = kBacklight;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = LEDC_CHANNEL_0;
  channel.intr_type = LEDC_INTR_DISABLE;
  channel.timer_sel = LEDC_TIMER_0;
  channel.duty = 900;
  return ledc_channel_config(&channel);
}

esp_err_t InitLvgl(Hardware *hardware) {
  lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
  lvgl_config.task_stack = 8192;
  lvgl_config.task_max_sleep_ms = 100;
  ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_config), kTag,
                      "Unable to initialize LVGL");

  lvgl_port_display_cfg_t display_config = {};
  display_config.io_handle = hardware->panel_io;
  display_config.panel_handle = hardware->panel;
  display_config.buffer_size = kPortraitWidth * 40;
  display_config.double_buffer = true;
  display_config.hres = kPortraitWidth;
  display_config.vres = kPortraitHeight;
  display_config.color_format = LV_COLOR_FORMAT_RGB565;
  display_config.flags.buff_dma = true;
  display_config.flags.sw_rotate = true;
  display_config.flags.swap_bytes = true;
  hardware->display = lvgl_port_add_disp(&display_config);
  if (hardware->display == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  lvgl_port_touch_cfg_t touch_config = {};
  touch_config.disp = hardware->display;
  touch_config.handle = hardware->touch;
  if (lvgl_port_add_touch(&touch_config) == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

} // namespace

esp_err_t Init(Hardware *hardware) {
  if (hardware == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  ESP_RETURN_ON_ERROR(InitI2c(hardware), kTag, "I2C init failed");
  ESP_RETURN_ON_ERROR(InitImu(hardware), kTag, "IMU init failed");
  ESP_RETURN_ON_ERROR(InitDisplay(hardware), kTag, "Display init failed");
  ESP_RETURN_ON_ERROR(InitTouch(hardware), kTag, "Touch init failed");
  ESP_RETURN_ON_ERROR(InitLvgl(hardware), kTag, "LVGL init failed");
  ESP_RETURN_ON_ERROR(InitBacklight(), kTag, "Backlight init failed");
  return ESP_OK;
}

bool ReadAcceleration(Hardware *, int16_t *x, int16_t *y, int16_t *z) {
  if (qmi_handle == nullptr || x == nullptr || y == nullptr || z == nullptr) {
    return false;
  }
  uint8_t status = 0;
  if (i2c_master_transmit_receive(qmi_handle, &kQmiStatus0, 1, &status, 1,
                                  100) != ESP_OK ||
      (status & 0x01U) == 0) {
    return false;
  }

  uint8_t raw[6] = {};
  if (i2c_master_transmit_receive(qmi_handle, &kQmiAccelXLow, 1, raw,
                                  sizeof(raw), 100) != ESP_OK) {
    return false;
  }
  *x = static_cast<int16_t>(raw[0] | (raw[1] << 8U));
  *y = static_cast<int16_t>(raw[2] | (raw[3] << 8U));
  *z = static_cast<int16_t>(raw[4] | (raw[5] << 8U));
  return true;
}

Orientation DetectOrientation(int16_t x, int16_t y,
                              Orientation current_orientation) {
  const int32_t abs_x = std::abs(static_cast<int32_t>(x));
  const int32_t abs_y = std::abs(static_cast<int32_t>(y));
  if (std::max(abs_x, abs_y) < kOrientationThreshold ||
      std::abs(abs_x - abs_y) < kOrientationHysteresis) {
    return current_orientation;
  }
  if (abs_x > abs_y) {
    return x > 0 ? Orientation::kLandscapeRight : Orientation::kLandscapeLeft;
  }
  return y > 0 ? Orientation::kPortrait : Orientation::kPortraitInverted;
}

void SetOrientation(Hardware *hardware, Orientation orientation) {
  if (hardware == nullptr || hardware->display == nullptr) {
    return;
  }
  const lv_display_rotation_t rotations[] = {
      LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90, LV_DISPLAY_ROTATION_180,
      LV_DISPLAY_ROTATION_270};
  lv_display_set_rotation(hardware->display,
                          rotations[static_cast<uint8_t>(orientation)]);
}

} // namespace board

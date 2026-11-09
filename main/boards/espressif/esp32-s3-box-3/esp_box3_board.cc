#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "esp_lcd_ili9341.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "idle_face.h"
#include "sensor_dock.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>

#define TAG "EspBox3Board"

// Status bar: append the sensor dock's room climate next to the clock, refreshed
// at most every 10 minutes (the bar itself repaints once a second).
class Box3LcdDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;

    // Draw our own battery: outline + level fill + the percentage centred on top.
    // The stock material-symbol glyph is only 14 px, too small to overlay a number.
    void UpdateStatusBar(bool update_all = false) override {
        SpiLcdDisplay::UpdateStatusBar(update_all);
        UpdateDockBattery();
        UpdateIdleFace();
    }

    // Create the idle clock face once the stock UI exists; it is drawn on top of
    // the emoji box so no LVGL surgery in lcd_display.cc is needed.
    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        if (idle_face_.IsInitialized()) {
            return;
        }
        DisplayLockGuard lock(this);
        lv_obj_t* screen = lv_screen_active();
        lv_obj_update_layout(screen);
        const int32_t top = top_bar_ != nullptr ? lv_obj_get_height(top_bar_) : 0;
        const lv_color_t fg = status_label_ != nullptr
                                  ? lv_obj_get_style_text_color(status_label_, LV_PART_MAIN)
                                  : lv_color_black();
        const lv_color_t bg = lv_obj_get_style_bg_color(screen, LV_PART_MAIN);
        idle_face_.Init(screen, LV_HOR_RES, LV_VER_RES, top, fg, bg);
        // Keep the low-battery warning above the face.
        if (low_battery_popup_ != nullptr) {
            lv_obj_move_foreground(low_battery_popup_);
        }
    }

    std::string GetStatusExtraText() override {
        constexpr int64_t kEnvRefreshUs = 10LL * 60 * 1000 * 1000;
        static int64_t last_read_us = 0;
        static std::string cached;
        const int64_t now = esp_timer_get_time();
        if (cached.empty() || now - last_read_us > kEnvRefreshUs) {
            float temperature = 0.0f;
            float humidity = 0.0f;
            if (SensorDock::GetInstance().ReadTemperatureHumidity(temperature, humidity)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.0fC %.0f%%", temperature, humidity);
                cached = buf;
            }
            last_read_us = now;
        }
        return cached;
    }

private:
    IdleFace idle_face_;
    lv_obj_t* batt_box_ = nullptr;
    lv_obj_t* batt_fill_ = nullptr;
    lv_obj_t* batt_nub_ = nullptr;
    lv_obj_t* batt_text_ = nullptr;

    // Show the clock face while the device idles and keep it ticking. The small
    // status-bar clock is blanked so the time is not on screen twice.
    void UpdateIdleFace() {
        if (!idle_face_.IsInitialized()) {
            return;
        }
        const bool idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
        const bool previewing =
            preview_image_ != nullptr && !lv_obj_has_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        const bool show = idle && !previewing;

        if (!Lock(100)) {
            return;
        }
        idle_face_.SetVisible(show);
        if (show) {
            if (status_label_ != nullptr && lv_label_get_text(status_label_)[0] != '\0') {
                lv_label_set_text(status_label_, "");
            }
            idle_face_.Refresh();
        }
        Unlock();

        if (show) {
            idle_face_.StartWeatherFetch();
        }
    }

    void UpdateDockBattery() {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        const bool have = Board::GetInstance().GetBatteryLevel(level, charging, discharging);

        if (!Lock(0)) {
            return;
        }

        // The built-in glyph stays empty: our widget replaces it in the same slot.
        if (battery_label_ != nullptr) {
            lv_label_set_text(battery_label_, "");
        }

        if (!have) {
            if (batt_box_ != nullptr) {
                lv_obj_add_flag(batt_box_, LV_OBJ_FLAG_HIDDEN);
            }
            Unlock();
            return;
        }

        // Inherit the bar's text colour/font instead of reaching into the theme type.
        const lv_color_t fg = battery_label_ != nullptr
                                  ? lv_obj_get_style_text_color(battery_label_, LV_PART_MAIN)
                                  : lv_color_black();
        const lv_color_t fill_color =
            level <= 20 ? lv_color_hex(0xD32F2F) : (level <= 50 ? lv_color_hex(0xF9A825) : fg);

        if (batt_box_ == nullptr) {
            lv_obj_t* parent = battery_label_ != nullptr ? lv_obj_get_parent(battery_label_) : nullptr;
            if (parent == nullptr) {
                Unlock();
                return;
            }

            batt_box_ = lv_obj_create(parent);
            lv_obj_set_size(batt_box_, 44, 18);
            lv_obj_set_style_radius(batt_box_, 3, 0);
            lv_obj_set_style_border_width(batt_box_, 1, 0);
            lv_obj_set_style_border_color(batt_box_, fg, 0);
            lv_obj_set_style_bg_opa(batt_box_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(batt_box_, 0, 0);
            lv_obj_set_style_margin_left(batt_box_, 4, 0);
            lv_obj_clear_flag(batt_box_, LV_OBJ_FLAG_SCROLLABLE);

            batt_fill_ = lv_obj_create(batt_box_);
            lv_obj_set_size(batt_fill_, 0, 12);
            lv_obj_align(batt_fill_, LV_ALIGN_LEFT_MID, 2, 0);
            lv_obj_set_style_radius(batt_fill_, 1, 0);
            lv_obj_set_style_border_width(batt_fill_, 0, 0);
            lv_obj_set_style_pad_all(batt_fill_, 0, 0);
            lv_obj_set_style_bg_opa(batt_fill_, LV_OPA_50, 0);
            lv_obj_set_style_bg_color(batt_fill_, fill_color, 0);

            batt_nub_ = lv_obj_create(batt_box_);
            lv_obj_set_size(batt_nub_, 2, 8);
            lv_obj_align(batt_nub_, LV_ALIGN_RIGHT_MID, 3, 0);
            lv_obj_set_style_radius(batt_nub_, 1, 0);
            lv_obj_set_style_border_width(batt_nub_, 0, 0);
            lv_obj_set_style_pad_all(batt_nub_, 0, 0);
            lv_obj_set_style_bg_color(batt_nub_, fg, 0);
            lv_obj_set_style_bg_opa(batt_nub_, LV_OPA_COVER, 0);

            batt_text_ = lv_label_create(batt_box_);
            lv_obj_set_style_text_font(batt_text_, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(batt_text_, fg, 0);
            lv_obj_center(batt_text_);
        }

        lv_obj_clear_flag(batt_box_, LV_OBJ_FLAG_HIDDEN);
        char text[8];
        snprintf(text, sizeof(text), "%d%%", level);
        lv_label_set_text(batt_text_, text);

        int width = static_cast<int>(38.0f * (level / 100.0f));
        if (width < 1 && level > 0) {
            width = 1;
        }
        lv_obj_set_width(batt_fill_, width);
        lv_obj_set_style_bg_color(batt_fill_, fill_color, 0);

        Unlock();
    }
};

// Init ili9341 by custom cmd
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},

    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},

    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},

    {0, (uint8_t []){0}, 0xff, 0},
};

class EspBox3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_6;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_7;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }


    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeIli9341Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_5;
        io_config.dc_gpio_num = GPIO_NUM_4;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const ili9341_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_48;
        panel_config.flags.reset_active_high = 1,
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void *)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new Box3LcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

public:
    EspBox3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeIli9341Display();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();

        // ESP32-S3-BOX-3-SENSOR dock (optional): AHT21 temp/humidity, radar, IR, 18650.
        // Everything degrades gracefully when the dock is not attached.
        auto& sensor_dock = SensorDock::GetInstance();
        const bool dock_ok = sensor_dock.Init();
        if (dock_ok) {
            ESP_LOGI(TAG, "ESP32-S3-BOX-3-SENSOR dock detected");
        }
        sensor_dock.RegisterTools();
        RegisterIdleFaceTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    // Top-right battery indicator: the dock's 18650, sampled every 2 minutes.
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        constexpr int64_t kBatteryRefreshUs = 2LL * 60 * 1000 * 1000;
        static int64_t last_read_us = 0;
        static int cached_level = -1;
        static bool have_value = false;
        const int64_t now = esp_timer_get_time();
        if (!have_value || now - last_read_us > kBatteryRefreshUs) {
            float voltage = 0.0f;
            int percent = 0;
            have_value = SensorDock::GetInstance().ReadBattery(voltage, percent);
            cached_level = percent;
            last_read_us = now;
        }
        if (!have_value) {
            return false;  // no dock -> no icon (boards without a battery behave the same)
        }
        level = cached_level;
        charging = false;     // the dock exposes no charge-status pin we read
        discharging = true;
        return true;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(EspBox3Board);

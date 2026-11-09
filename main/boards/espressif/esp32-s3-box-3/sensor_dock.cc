#include "sensor_dock.h"

#include <cstring>
#include <sstream>
#include <string>

#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mcp_server.h"

#define TAG "SensorDock"

// Diagnostic build: run the IR loopback self test once at boot so the serial log
// captures what the dock's own receiver hears. Set to 0 for a shipping build.
#define IR_AC_BOOT_SELFTEST 0

// IR transmit is off: the dock's emitter is never energised, the RMT transmit
// channel is not even created, and no emitting tool is offered to the model
// (self.ir.send_nec / send_aircon / selftest). The receive-only learn tool stays,
// so the dock can still listen to a physical remote. Flip to true to restore
// emission - everything else is unchanged.
constexpr bool kIrTransmitEnabled = false;

// ---- ESP32-S3-BOX-3-SENSOR pin map (from espressif/esp-box bsp_board.h + schematic) ----
static constexpr gpio_num_t kExpandScl = GPIO_NUM_40;
static constexpr gpio_num_t kExpandSda = GPIO_NUM_41;
static constexpr gpio_num_t kRadarOut = GPIO_NUM_21;
static constexpr gpio_num_t kIrTx = GPIO_NUM_39;
static constexpr gpio_num_t kIrRx = GPIO_NUM_38;
static constexpr gpio_num_t kIrEn = GPIO_NUM_44;
static constexpr adc_channel_t kBatteryChannel = ADC_CHANNEL_9;  // GPIO10
static constexpr float kBatteryUpperR = 301000.0f;               // 301K / 100K divider
static constexpr float kBatteryLowerR = 100000.0f;

static constexpr uint8_t kAht20Addr = 0x38;

SensorDock& SensorDock::GetInstance() {
    static SensorDock instance;
    return instance;
}

bool SensorDock::Init() {
    if (!InitExpandI2c()) {
        ESP_LOGW(TAG, "expand I2C bus unavailable - sensor dock not usable");
        return false;
    }

    ScanExpandBus();

    InitRadar();
    aht20_ready_ = ProbeAht20() && Aht20Init();
    InitBattery();
    InitIr();

    present_ = aht20_ready_ || dock_detected_;
    ESP_LOGI(TAG, "sensor dock: aht20=%d radar=%d battery=%d ir=%d", aht20_ready_,
             gpio_get_level(kRadarOut) >= 0, adc_ready_, ir_ready_);
#if IR_AC_BOOT_SELFTEST
    if (ir_ready_) {
        const std::string report = IrSelfTest();
        ESP_LOGI(TAG, "boot ir selftest: %s", report.c_str());
    }
#endif
    return present_;
}

bool SensorDock::InitExpandI2c() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;  // port 1 is the board's codec bus
    bus_cfg.sda_io_num = kExpandSda;
    bus_cfg.scl_io_num = kExpandScl;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &expand_bus_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kAht20Addr;
    dev_cfg.scl_speed_hz = 100000;
    err = i2c_master_bus_add_device(expand_bus_, &dev_cfg, &aht20_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add aht20 failed: %s", esp_err_to_name(err));
        aht20_ = nullptr;
    }
    return true;
}

void SensorDock::ScanExpandBus() {
    std::string found;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(expand_bus_, addr, 50) == ESP_OK) {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X ", addr);
            found += buf;
        }
    }
    ESP_LOGI(TAG, "expand bus devices: %s", found.empty() ? "(none)" : found.c_str());
    if (!found.empty()) {
        dock_detected_ = true;
    }
}

bool SensorDock::ProbeAht20() {
    if (aht20_ == nullptr) {
        return false;
    }
    bool ok = i2c_master_probe(expand_bus_, kAht20Addr, 100) == ESP_OK;
    if (!ok) {
        ESP_LOGW(TAG, "AHT20 not answering on 0x38 (sensor dock absent?)");
    }
    return ok;
}

bool SensorDock::Aht20Init() {
    if (aht20_ == nullptr) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    uint8_t status = 0;
    if (i2c_master_receive(aht20_, &status, 1, 100) != ESP_OK) {
        return false;
    }
    if ((status & 0x18) != 0x18) {
        uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
        if (i2c_master_transmit(aht20_, init_cmd, sizeof(init_cmd), 100) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

bool SensorDock::ReadTemperatureHumidity(float& temperature, float& humidity) {
    if (!aht20_ready_) {
        return false;
    }
    uint8_t trigger[3] = {0xAC, 0x33, 0x00};
    if (i2c_master_transmit(aht20_, trigger, sizeof(trigger), 100) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(90));

    uint8_t data[7] = {0};
    if (i2c_master_receive(aht20_, data, sizeof(data), 100) != ESP_OK) {
        return false;
    }
    if (data[0] & 0x80) {
        return false;  // still busy
    }

    uint32_t raw_humidity = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t raw_temp = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    humidity = raw_humidity * 100.0f / 1048576.0f;
    temperature = raw_temp * 200.0f / 1048576.0f - 50.0f;
    return true;
}

bool SensorDock::InitRadar() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << kRadarOut;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&cfg) == ESP_OK;
}

bool SensorDock::RadarPresence() {
    // Without the dock the pin floats, so never report presence in that case.
    if (!dock_detected_) {
        return false;
    }
    // AT581X drives its OUT pin high while a person is detected.
    return gpio_get_level(kRadarOut) == 1;
}

bool SensorDock::InitBattery() {
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit_cfg, &adc_handle_) != ESP_OK) {
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(adc_handle_, kBatteryChannel, &chan_cfg) != ESP_OK) {
        return false;
    }

#ifdef ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc_cali_) != ESP_OK) {
        adc_cali_ = nullptr;
    }
#endif

    adc_ready_ = true;
    return true;
}

bool SensorDock::ReadBattery(float& voltage, int& percent) {
    if (!adc_ready_ || !dock_detected_) {
        return false;  // IO10 floats without the 18650 dock
    }
    int raw = 0;
    if (adc_oneshot_read(adc_handle_, kBatteryChannel, &raw) != ESP_OK) {
        return false;
    }
    int millivolts = 0;
    if (adc_cali_ != nullptr) {
        if (adc_cali_raw_to_voltage(adc_cali_, raw, &millivolts) != ESP_OK) {
            return false;
        }
    } else {
        millivolts = raw * 3100 / 4095;  // rough fallback
    }

    voltage = (millivolts / 1000.0f) * (kBatteryUpperR + kBatteryLowerR) / kBatteryLowerR;

    // Calibrated on this dock: a fully charged 18650 is measured at ~4.10 V at the pack
    // (the charge path drops the rest), so 4.10 V is the 100 % anchor, not a nominal 4.20.
    constexpr float kFullV = 4.05f;
    constexpr float kEmptyV = 3.30f;

    if (voltage >= kFullV - 0.01f) {  // float tolerance: 4.05f is stored as 4.0499...
        percent = 100;
    } else if (voltage <= kEmptyV) {
        percent = 0;
    } else {
        percent = (int)((voltage - kEmptyV) / (kFullV - kEmptyV) * 100.0f);
    }

    ESP_LOGI(TAG, "battery: adc %d mV -> pack %.2f V -> %d%%", millivolts, voltage, percent);
    return true;
}

bool SensorDock::InitIr() {
    gpio_config_t en_cfg = {};
    en_cfg.pin_bit_mask = 1ULL << kIrEn;
    en_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&en_cfg);
    gpio_set_level(kIrEn, 1);  // CTRL is active-low: high holds the IR stage off

    gpio_config_t rx_cfg = {};
    rx_cfg.pin_bit_mask = 1ULL << kIrRx;
    rx_cfg.mode = GPIO_MODE_INPUT;
    gpio_config(&rx_cfg);

    if (kIrTransmitEnabled) {
        rmt_tx_channel_config_t tx_cfg = {};
        tx_cfg.gpio_num = kIrTx;
        tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
        tx_cfg.resolution_hz = 1000000;  // 1 tick = 1 us
        // One Mitsubishi A/C frame is 146 symbols, so reserve several memory blocks.
        tx_cfg.mem_block_symbols = 192;
        tx_cfg.trans_queue_depth = 4;
        if (rmt_new_tx_channel(&tx_cfg, &ir_channel_) != ESP_OK) {
            ESP_LOGW(TAG, "rmt_new_tx_channel failed");
            return false;
        }

        // Real IR receivers expect a modulated carrier; without this the LED is
        // simply held on for the whole mark and nothing decodes. 38 kHz, 1/3 duty.
        rmt_carrier_config_t carrier_cfg = {};
        carrier_cfg.frequency_hz = 38000;
        carrier_cfg.duty_cycle = 0.33;
        carrier_cfg.flags.polarity_active_low = false;
        if (rmt_apply_carrier(ir_channel_, &carrier_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "rmt_apply_carrier failed - IR will go out unmodulated");
        }

        rmt_copy_encoder_config_t copy_cfg = {};
        if (rmt_new_copy_encoder(&copy_cfg, &ir_encoder_) != ESP_OK) {
            ESP_LOGW(TAG, "rmt_new_copy_encoder failed");
            return false;
        }

        if (rmt_enable(ir_channel_) != ESP_OK) {
            return false;
        }
    } else {
        ESP_LOGI(TAG, "IR transmit disabled: emitter held off, receive-only mode");
    }

    // Receive side: the dock's demodulating receiver drives GPIO38, so RMT just
    // records the envelope. Used by IrSelfTest to prove light leaves the LED.
    rmt_rx_channel_config_t ir_rx_cfg = {};
    ir_rx_cfg.gpio_num = kIrRx;
    ir_rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    ir_rx_cfg.resolution_hz = 1000000;
    ir_rx_cfg.mem_block_symbols = 192;
    ir_rx_cfg.flags.invert_in = false;
    ir_rx_cfg.flags.with_dma = false;
    if (rmt_new_rx_channel(&ir_rx_cfg, &ir_rx_channel_) != ESP_OK) {
        ESP_LOGW(TAG, "rmt_new_rx_channel failed - IR self test unavailable");
        ir_rx_channel_ = nullptr;
    } else {
        ir_rx_queue_ = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));
        rmt_rx_event_callbacks_t cbs = {};
        cbs.on_recv_done = [](rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata,
                              void* ctx) -> bool {
            if (edata) {
                auto& dock = *static_cast<SensorDock*>(ctx);
                dock.ir_rx_count_ = edata->num_symbols < 192 ? edata->num_symbols : 192;
                if (edata->received_symbols && dock.ir_rx_count_ > 0) {
                    memcpy(dock.ir_rx_symbols_, edata->received_symbols,
                           dock.ir_rx_count_ * sizeof(rmt_symbol_word_t));
                }
                BaseType_t higher = pdFALSE;
                xQueueSendFromISR(dock.ir_rx_queue_, edata, &higher);
                return higher == pdTRUE;
            }
            return false;
        };
        if (rmt_rx_register_event_callbacks(ir_rx_channel_, &cbs, this) != ESP_OK ||
            rmt_enable(ir_rx_channel_) != ESP_OK) {
            ESP_LOGW(TAG, "IR receive channel not enabled");
            ir_rx_channel_ = nullptr;
        }
    }

    ir_ready_ = true;
    return true;
}

bool SensorDock::IrTransmit(const rmt_symbol_word_t* symbols, size_t count) {
    if (!kIrTransmitEnabled || !ir_ready_ || ir_channel_ == nullptr) {
        return false;
    }
    // The dock's IR stage is enabled by pulling CTRL/EN LOW (verified with the
    // on-board receiver: high = LED dark, low = the frame comes back intact).
    gpio_set_level(kIrEn, 0);
    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 1;
    esp_err_t err = rmt_transmit(ir_channel_, ir_encoder_, symbols,
                                 count * sizeof(rmt_symbol_word_t), &tx_config);
    if (err == ESP_OK) {
        rmt_tx_wait_all_done(ir_channel_, 1000);
    } else {
        ESP_LOGW(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
    }
    gpio_set_level(kIrEn, 1);
    return err == ESP_OK;
}

bool SensorDock::IrSendNec(uint8_t address, uint8_t command) {
    if (!kIrTransmitEnabled || !ir_ready_) {
        ESP_LOGW(TAG, "IR transmit is disabled in this build");
        return false;
    }

    rmt_symbol_word_t symbols[68] = {};
    int n = 0;
    // leader: 9 ms mark + 4.5 ms space
    symbols[n++] = {.duration0 = 9000, .level0 = 1, .duration1 = 4500, .level1 = 0};

    auto push_bit = [&](bool one) {
        symbols[n++] = {.duration0 = 560,
                        .level0 = 1,
                        .duration1 = static_cast<uint16_t>(one ? 1690 : 560),
                        .level1 = 0};
    };

    uint8_t bytes[4] = {address, static_cast<uint8_t>(~address), command,
                        static_cast<uint8_t>(~command)};
    for (uint8_t byte : bytes) {
        for (int bit = 0; bit < 8; bit++) {
            push_bit((byte >> bit) & 0x01);
        }
    }
    symbols[n++] = {.duration0 = 560, .level0 = 1, .duration1 = 0, .level1 = 0};

    return IrTransmit(symbols, n);
}

// ---- Mitsubishi Electric MSZ air conditioner, 144-bit frame ----
// Timing and byte layout follow IRremoteESP8266's MITSUBISHI_AC protocol
// (ir_Mitsubishi.cpp): 18 bytes, LSB-first, 38 kHz carrier.
static constexpr uint16_t kAcHdrMark = 3400;
static constexpr uint16_t kAcHdrSpace = 1750;
static constexpr uint16_t kAcBitMark = 450;
static constexpr uint16_t kAcOneSpace = 1300;
static constexpr uint16_t kAcZeroSpace = 420;
static constexpr uint16_t kAcRptMark = 440;
static constexpr uint16_t kAcRptSpace = 15500;

static constexpr int kAcMinTemp = 16;
static constexpr int kAcMaxTemp = 31;

// Known-good idle frame (bytes 0-10 of the library's reset state, rest zero).
static const uint8_t kAcTemplate[18] = {0x23, 0xCB, 0x26, 0x01, 0x00, 0x20,
                                        0x08, 0x06, 0x30, 0x45, 0x67};

// Byte 8 carries a mode-dependent constant plus the horizontal (wide) vane.
static void AcApplyMode(uint8_t* ac, int mode) {
    switch (mode) {
        case 0b011:  // cool
            ac[8] = 0b00110110;
            break;
        case 0b010:  // dry
            ac[8] = 0b00110010;
            break;
        case 0b111:  // fan only
            ac[8] = 0b00110111;
            break;
        case 0b001:  // heat
        case 0b100:  // auto
        default:
            ac[8] = 0b00110000;
            break;
    }
    // Byte 6 bits 3-5 hold the mode code as well.
    ac[6] = static_cast<uint8_t>((ac[6] & ~0b00111000) | ((mode & 0x07) << 3));
}

// Byte 7: bits 0-3 = degrees above 16, bit 4 = half degree.
static void AcApplyTemp(uint8_t* ac, int temperature) {
    if (temperature < kAcMinTemp) temperature = kAcMinTemp;
    if (temperature > kAcMaxTemp) temperature = kAcMaxTemp;
    ac[7] = static_cast<uint8_t>((ac[7] & ~0b00011111) |
                                 ((temperature - kAcMinTemp) & 0x0F));
}

// Byte 9: bits 0-2 fan, bits 3-5 vertical vane, bit 6 vane-valid, bit 7 fan-auto.
static void AcApplyFan(uint8_t* ac, int fan) {
    int value = fan;
    if (value < 0 || value > 6) value = 0;
    const bool auto_mode = (value == 0);
    if (value >= 5) value--;  // there is no "5"; 5/6 map onto 4/5
    ac[9] = static_cast<uint8_t>((ac[9] & ~0b10000111) | (value & 0x07) |
                                 (auto_mode ? 0x80 : 0x00));
}

static void AcApplyVane(uint8_t* ac, int vane) {
    if (vane < 0 || vane > 7) vane = 0;
    ac[9] = static_cast<uint8_t>((ac[9] & ~0b01111000) | ((vane & 0x07) << 3) | 0x40);
}

static void AcApplyWideVane(uint8_t* ac, int wide_vane) {
    if (wide_vane < 1 || wide_vane > 8) return;  // 0 = leave as-is
    ac[8] = static_cast<uint8_t>((ac[8] & 0x0F) | ((wide_vane & 0x0F) << 4));
}

static void AcApplyPower(uint8_t* ac, bool on) {
    ac[5] = static_cast<uint8_t>((ac[5] & ~0x20) | (on ? 0x20 : 0x00));
}

// Byte 17 = plain sum of the first 17 bytes.
static void AcApplyChecksum(uint8_t* ac) {
    uint8_t sum = 0;
    for (int i = 0; i < 17; i++) {
        sum = static_cast<uint8_t>(sum + ac[i]);
    }
    ac[17] = sum;
}

// Expand an 18-byte A/C state into RMT symbols (header, 144 bits LSB-first,
// 440/15500 footer) and return the symbol count.
static int AcBuildSymbols(const uint8_t* ac, rmt_symbol_word_t* symbols) {
    int n = 0;
    symbols[n++] = {.duration0 = kAcHdrMark, .level0 = 1, .duration1 = kAcHdrSpace, .level1 = 0};
    for (int i = 0; i < 18; i++) {
        for (int bit = 0; bit < 8; bit++) {
            const bool one = ((ac[i] >> bit) & 0x01) != 0;
            symbols[n++] = {.duration0 = kAcBitMark,
                            .level0 = 1,
                            .duration1 = static_cast<uint16_t>(one ? kAcOneSpace : kAcZeroSpace),
                            .level1 = 0};
        }
    }
    symbols[n++] = {.duration0 = kAcRptMark, .level0 = 1, .duration1 = kAcRptSpace, .level1 = 0};
    return n;
}

bool SensorDock::IrSendMitsubishiAc(bool power_on, int mode, int temperature, int fan, int vane,
                                    int wide_vane) {
    if (!kIrTransmitEnabled || !ir_ready_) {
        ESP_LOGW(TAG, "IR transmit is disabled in this build");
        return false;
    }

    uint8_t ac[18];
    if (ac_state_ready_) {
        memcpy(ac, ac_state_, sizeof(ac));
    } else {
        memcpy(ac, kAcTemplate, sizeof(ac));
        for (int i = 11; i < 18; i++) ac[i] = 0;
    }

    AcApplyMode(ac, mode);
    AcApplyWideVane(ac, wide_vane);
    AcApplyTemp(ac, temperature);
    AcApplyFan(ac, fan);
    AcApplyVane(ac, vane);
    AcApplyPower(ac, power_on);
    AcApplyChecksum(ac);
    memcpy(ac_state_, ac, sizeof(ac));
    ac_state_ready_ = true;
    ac_power_ = power_on;
    ac_mode_ = mode;
    ac_temperature_ = temperature;
    ac_fan_ = fan;
    ac_vane_ = vane;

    rmt_symbol_word_t symbols[146] = {};
    const int n = AcBuildSymbols(ac, symbols);

    ESP_LOGI(TAG, "mitsubishi ac frame: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X sum=%02X",
             ac[0], ac[1], ac[2], ac[3], ac[4], ac[5], ac[6], ac[7], ac[8], ac[9], ac[10], ac[11],
             ac[12], ac[13], ac[14], ac[15], ac[16], ac[17]);
    bool ok = IrTransmit(symbols, n);
    vTaskDelay(pdMS_TO_TICKS(25));
    ok = IrTransmit(symbols, n) && ok;
    return ok;
}

// Transmit + listen on the dock's own receiver, so we learn whether the emitter
// produces light at all and which drive variant the unit would hear.
//   phase A: enable pin high, 38 kHz carrier   (what send_aircon does)
//   phase B: enable pin low,  38 kHz carrier   (inverted enable stage)
//   phase C: enable pin high, no carrier       (bare LED, no modulation)
std::string SensorDock::IrSelfTest() {
    if (!kIrTransmitEnabled) {
        return "{\"success\": false, \"error\": \"IR transmit is disabled in this build\"}";
    }
    if (!ir_ready_ || ir_rx_channel_ == nullptr || ir_rx_queue_ == nullptr) {
        return "{\"success\": false, \"error\": \"IR transmit or receive channel unavailable\"}";
    }

    uint8_t ac[18];
    memcpy(ac, kAcTemplate, sizeof(ac));
    for (int i = 11; i < 18; i++) ac[i] = 0;
    AcApplyMode(ac, 0b011);   // cool
    AcApplyTemp(ac, 24);
    AcApplyFan(ac, 0);        // auto
    AcApplyVane(ac, 0);       // auto
    AcApplyPower(ac, true);
    AcApplyChecksum(ac);

    rmt_symbol_word_t symbols[146] = {};
    const int n = AcBuildSymbols(ac, symbols);

    std::ostringstream out;
    out << "{\"success\": true, \"frame\": \"";
    for (int i = 0; i < 18; i++) {
        char h[4];
        snprintf(h, sizeof(h), "%02X", ac[i]);
        out << h;
    }
    out << "\", \"phases\": [";

    const char* names[3] = {"ctrl_low_carrier", "ctrl_high_carrier", "ctrl_low_no_carrier"};
    for (int phase = 0; phase < 3; phase++) {
        // drive variant
        gpio_set_level(kIrEn, phase == 1 ? 1 : 0);
        if (phase == 2) {
            rmt_apply_carrier(ir_channel_, nullptr);
        } else {
            rmt_carrier_config_t carrier_cfg = {};
            carrier_cfg.frequency_hz = 38000;
            carrier_cfg.duty_cycle = 0.33;
            carrier_cfg.flags.polarity_active_low = false;
            rmt_apply_carrier(ir_channel_, &carrier_cfg);
        }

        ir_rx_count_ = 0;
        rmt_receive_config_t recv_cfg = {};
        recv_cfg.signal_range_min_ns = 3000;      // ignore < 3 us glitches
        recv_cfg.signal_range_max_ns = 20000000;  // 20 ms idle ends the capture
        esp_err_t err = rmt_receive(ir_rx_channel_, ir_rx_symbols_, sizeof(ir_rx_symbols_),
                                    &recv_cfg);
        if (err != ESP_OK) {
            out << (phase ? ", " : "") << "{\"phase\": \"" << names[phase]
                << "\", \"error\": \"rmt_receive failed\"}";
            gpio_set_level(kIrEn, 1);
            continue;
        }

        rmt_transmit_config_t tx_config = {};
        tx_config.loop_count = 1;
        rmt_transmit(ir_channel_, ir_encoder_, symbols, n * sizeof(rmt_symbol_word_t), &tx_config);
        rmt_tx_wait_all_done(ir_channel_, 1000);

        rmt_rx_done_event_data_t edata = {};
        const bool got = xQueueReceive(ir_rx_queue_, &edata, pdMS_TO_TICKS(400)) == pdTRUE;
        gpio_set_level(kIrEn, 1);

        out << (phase ? ", " : "") << "{\"phase\": \"" << names[phase] << "\", \"got\": "
            << (got ? "true" : "false") << ", \"symbols\": " << ir_rx_count_ << ", \"head\": \"";
        int printed = 0;
        for (size_t i = 0; i < ir_rx_count_ && printed < 6; i++) {
            if (ir_rx_symbols_[i].duration0) {
                out << (printed ? " " : "") << ir_rx_symbols_[i].duration0 << "L"
                    << ir_rx_symbols_[i].level0;
                printed++;
            }
            if (printed < 6 && ir_rx_symbols_[i].duration1) {
                out << (printed ? " " : "") << ir_rx_symbols_[i].duration1 << "L"
                    << ir_rx_symbols_[i].level1;
                printed++;
            }
        }
        out << "\"}";
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    out << "]}";
    // leave the transmitter in its normal configuration
    {
        rmt_carrier_config_t carrier_cfg = {};
        carrier_cfg.frequency_hz = 38000;
        carrier_cfg.duty_cycle = 0.33;
        carrier_cfg.flags.polarity_active_low = false;
        rmt_apply_carrier(ir_channel_, &carrier_cfg);
        gpio_set_level(kIrEn, 1);
    }

    const std::string report = out.str();
    ESP_LOGI(TAG, "ir selftest: %s", report.c_str());
    return report;
}


static std::string JsonError(const char* message);

// Decode a captured Mitsubishi Electric frame: header (3400/1750), 144 bits LSB-first
// where the space length discriminates 0 (~420 us) from 1 (~1300 us).
int SensorDock::AcDecode(const rmt_symbol_word_t* symbols, size_t count, uint8_t* out,
                         int max_bytes) {
    int bit = 0;
    for (size_t i = 0; i < count && bit < max_bytes * 8; i++) {
        const rmt_symbol_word_t& sym = symbols[i];
        if (sym.level0 != 1) {
            continue;  // not a mark, skip
        }
        if (sym.duration0 > 2000) {
            continue;  // header mark, not a data bit
        }
        if (sym.duration0 < 200) {
            continue;  // noise
        }
        const bool one = sym.duration1 > 800;
        if (one) {
            out[bit / 8] = static_cast<uint8_t>(out[bit / 8] | (1 << (bit % 8)));
        }
        bit++;
    }
    return bit / 8;
}

std::string SensorDock::IrLearn(int seconds) {
    if (ir_rx_channel_ == nullptr || ir_rx_queue_ == nullptr) {
        return "{\"success\": false, \"error\": \"IR receiver unavailable\"}";
    }
    if (seconds <= 0 || seconds > 20) {
        seconds = 8;
    }
    // Drop any stale completion event: receiving into a null buffer faults.
    xQueueReset(ir_rx_queue_);
    ir_rx_count_ = 0;
    // The dock's IR stage (LED + receiver) is only powered while CTRL/EN is low;
    // IrTransmit restores it high, so the receiver would be dead without this.
    gpio_set_level(kIrEn, 0);

    rmt_receive_config_t rx_cfg = {};
    rx_cfg.signal_range_min_ns = 1000;       // ignore <1 us glitches
    rx_cfg.signal_range_max_ns = 20000000;   // 20 ms of silence ends the frame

    esp_err_t err = rmt_receive(ir_rx_channel_, ir_rx_symbols_,
                                sizeof(ir_rx_symbols_), &rx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive failed: %s", esp_err_to_name(err));
        gpio_set_level(kIrEn, 1);
        return JsonError("could not start the IR receiver");
    }
    ESP_LOGI(TAG, "ir learn: listening for %d s", seconds);

    rmt_rx_done_event_data_t done = {};
    if (xQueueReceive(ir_rx_queue_, &done, pdMS_TO_TICKS(seconds * 1000)) != pdTRUE) {
        gpio_set_level(kIrEn, 1);
        return "{\"success\": false, \"error\": \"no IR received - point the remote at the box\"}";
    }

    uint8_t bytes[24] = {0};
    const int decoded = AcDecode(ir_rx_symbols_, ir_rx_count_, bytes, sizeof(bytes));

    std::ostringstream out;
    out << "{\"success\": true, \"symbols\": " << ir_rx_count_
        << ", \"bytes\": " << decoded << ", \"hex\": \"";
    for (int i = 0; i < decoded; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", bytes[i]);
        out << buf;
    }
    out << "\", \"header\": \"";
    if (ir_rx_count_ > 0) {
        out << ir_rx_symbols_[0].duration0 << "L" << ir_rx_symbols_[0].level0 << " "
            << ir_rx_symbols_[0].duration1 << "L" << ir_rx_symbols_[0].level1;
    }
    out << "\"}";
    gpio_set_level(kIrEn, 1);
    ESP_LOGI(TAG, "ir learn: %s", out.str().c_str());
    return out.str();
}

static std::string JsonError(const char* message) {
    std::ostringstream out;
    out << "{\"success\": false, \"error\": \"" << message << "\"}";
    return out.str();
}

void SensorDock::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.env.get_temperature_humidity",
        "Read the room temperature and humidity from the sensor dock. "
        "Use when the user asks how warm/cold or how humid it is here.\n"
        "Return value: JSON with temperature_c and humidity_percent.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            float temperature = 0.0f;
            float humidity = 0.0f;
            if (!SensorDock::GetInstance().ReadTemperatureHumidity(temperature, humidity)) {
                return JsonError("temperature/humidity sensor not available");
            }
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"success\": true, \"temperature_c\": %.1f, \"humidity_percent\": %.1f}",
                     temperature, humidity);
            return std::string(buf);
        });

    mcp.AddTool(
        "self.radar.get_presence",
        "Check whether a person is present in front of the device (2.4 GHz radar on the "
        "sensor dock). Use for 'is anyone there', presence or occupancy questions.\n"
        "Return value: JSON with presence true/false.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            if (!SensorDock::GetInstance().Present()) {
                return JsonError("radar sensor dock not connected");
            }
            bool presence = SensorDock::GetInstance().RadarPresence();
            return std::string(presence ? "{\"success\": true, \"presence\": true}"
                                        : "{\"success\": true, \"presence\": false}");
        });

    mcp.AddTool(
        "self.battery.get_status",
        "Read the 18650 battery voltage and rough charge percentage of the sensor dock.\n"
        "Return value: JSON with voltage_v and percent.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            float voltage = 0.0f;
            int percent = 0;
            if (!SensorDock::GetInstance().Present() ||
                !SensorDock::GetInstance().ReadBattery(voltage, percent)) {
                return JsonError("battery measurement not available");
            }
            char buf[96];
            snprintf(buf, sizeof(buf), "{\"success\": true, \"voltage_v\": %.2f, \"percent\": %d}",
                     voltage, percent);
            return std::string(buf);
        });

    if (kIrTransmitEnabled) {
        mcp.AddTool(
            "self.ir.send_nec",
            "Send an NEC infrared command through the sensor dock's IR emitter (e.g. to an air "
            "conditioner or TV). Provide the NEC address and command bytes.\n"
            "Parameters: address (0-255), command (0-255).\n"
            "Return value: JSON with success true/false.",
            PropertyList({Property("address", kPropertyTypeInteger, 0, 255),
                          Property("command", kPropertyTypeInteger, 0, 255)}),
            [](const PropertyList& properties) -> ReturnValue {
                int address = properties["address"].value<int>();
                int command = properties["command"].value<int>();
                if (!SensorDock::GetInstance().IrSendNec(static_cast<uint8_t>(address),
                                                         static_cast<uint8_t>(command))) {
                    return JsonError("IR emitter not available");
                }
                char buf[96];
                snprintf(buf, sizeof(buf), "{\"success\": true, \"address\": %d, \"command\": %d}",
                         address, command);
                return std::string(buf);
            });

        mcp.AddTool(
            "self.ir.send_aircon",
            "Control the Mitsubishi Electric split air conditioner (aircon / AC) in front of this "
            "device through the sensor dock's IR emitter. Use it for any request to switch the "
            "aircon on or off, change the temperature, change fan speed, or change the louvre "
            "swing. Every call sends the complete setting, so always pass the mode and temperature "
            "you want the unit to end up at, keeping the previous ones unless the user asked to "
            "change them.\n"
            "Parameters: power (on|off), mode (cool|dry|heat|fan|auto), temperature (16-31 celsius), "
            "fan (auto|quiet|1|2|3|4|max), vane (auto|top|high|middle|low|bottom|swing).\n"
            "The aircon is one-way IR: it cannot report its state back, so tell the user what was "
            "sent rather than claiming you read the unit.\n"
            "Return value: JSON with success and the transmitted settings.",
            PropertyList({Property("power", kPropertyTypeString, std::string("on")),
                          Property("mode", kPropertyTypeString, std::string("cool")),
                          Property("temperature", kPropertyTypeInteger, 24, 16, 31),
                          Property("fan", kPropertyTypeString, std::string("auto")),
                          Property("vane", kPropertyTypeString, std::string("auto"))}),
            [](const PropertyList& properties) -> ReturnValue {
                auto lower = [](std::string text) {
                    for (char& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    return text;
                };
                const std::string power_text = lower(properties["power"].value<std::string>());
                const std::string mode_text = lower(properties["mode"].value<std::string>());
                const std::string fan_text = lower(properties["fan"].value<std::string>());
                const std::string vane_text = lower(properties["vane"].value<std::string>());
                const int temperature = properties["temperature"].value<int>();

                const bool power_on = !(power_text == "off" || power_text == "false" ||
                                        power_text == "0" || power_text == "no");

                int mode = 0b011;  // cool
                if (mode_text == "auto") mode = 0b100;
                else if (mode_text == "dry" || mode_text == "dehumidify") mode = 0b010;
                else if (mode_text == "heat") mode = 0b001;
                else if (mode_text == "fan" || mode_text == "fan_only") mode = 0b111;

                int fan = 0;  // auto
                if (fan_text == "quiet" || fan_text == "silent" || fan_text == "low") fan = 6;
                else if (fan_text == "1") fan = 1;
                else if (fan_text == "2" || fan_text == "medium") fan = 2;
                else if (fan_text == "3") fan = 3;
                else if (fan_text == "4" || fan_text == "high") fan = 4;
                else if (fan_text == "max" || fan_text == "5" || fan_text == "turbo") fan = 5;

                int vane = 0;  // auto
                if (vane_text == "top" || vane_text == "highest") vane = 1;
                else if (vane_text == "high") vane = 2;
                else if (vane_text == "middle" || vane_text == "mid") vane = 3;
                else if (vane_text == "low") vane = 4;
                else if (vane_text == "bottom" || vane_text == "lowest") vane = 5;
                else if (vane_text == "swing" || vane_text == "auto_move") vane = 7;

                if (!SensorDock::GetInstance().IrSendMitsubishiAc(power_on, mode, temperature, fan,
                                                                  vane)) {
                    return JsonError("IR emitter not available");
                }
                char buf[224];
                snprintf(buf, sizeof(buf),
                         "{\"success\": true, \"power\": \"%s\", \"mode\": \"%s\", "
                         "\"temperature\": %d, \"fan\": \"%s\", \"vane\": \"%s\", "
                         "\"note\": \"sent once, no state feedback from the unit\"}",
                         power_on ? "on" : "off", mode_text.c_str(), temperature, fan_text.c_str(),
                         vane_text.c_str());
                return std::string(buf);
            });

    }  // kIrTransmitEnabled

    mcp.AddTool(
        "self.ir.learn",
        "Listen on the dock's infrared receiver and report one frame from a physical remote "
        "(receive only - this device cannot transmit infrared). Ask the user to point the "
        "remote at the device and press a button while this runs. "
        "Parameters: seconds (1-20, default 8).\n"
        "Return value: JSON with symbols, decoded byte count, the frame as hex and the header "
        "timings.",
        PropertyList({Property("seconds", kPropertyTypeInteger, 8, 1, 20)}),
        [](const PropertyList& properties) -> ReturnValue {
            return std::string(
                SensorDock::GetInstance().IrLearn(properties["seconds"].value<int>()));
        });

    if (kIrTransmitEnabled) {
        mcp.AddTool(
            "self.ir.selftest",
            "Diagnose the infrared emitter by transmitting one aircon frame and listening on the "
            "dock's own IR receiver under three drive variants (enable high/low, with and without a "
            "38 kHz carrier). Use this when the user says the aircon did not react, or asks to test "
            "the IR. Each phase reports how many IR pulses came back and their opening timings; "
            "pulses near 3400 then 1750 microseconds mean the emitter really is sending.\n"
            "Return value: JSON with the frame bytes and one entry per phase.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                return SensorDock::GetInstance().IrSelfTest();
            });
    }
}

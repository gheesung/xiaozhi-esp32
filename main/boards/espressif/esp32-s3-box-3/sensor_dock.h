#ifndef SENSOR_DOCK_H
#define SENSOR_DOCK_H

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_oneshot.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string>

// ESP32-S3-BOX-3-SENSOR dock (optional bottom board)
//   AHT21/AHT20 temp+humidity   I2C  SCL=GPIO40 SDA=GPIO41  addr 0x38
//   radar (AT581X) presence     OUT  GPIO21 (level = person present)
//   IR emitter / receiver       TX=GPIO39 RX=GPIO38 CTRL/EN=GPIO44
//   18650 battery               BAT_MEAS_ADC = GPIO10, divider 301K / 100K
class SensorDock {
public:
    static SensorDock& GetInstance();

    bool Init();
    bool Present() const { return present_; }

    bool ReadTemperatureHumidity(float& temperature, float& humidity);
    bool RadarPresence();
    bool ReadBattery(float& voltage, int& percent);
    bool IrSendNec(uint8_t address, uint8_t command);
    // Mitsubishi Electric MSZ A/C (144-bit) frame.
    //   mode: 0b100 auto, 0b011 cool, 0b010 dry, 0b001 heat, 0b111 fan
    //   fan : 0 auto, 1..4, 5 max, 6 quiet
    //   vane: 0 auto, 1 top, 2 high, 3 middle, 4 low, 5 bottom, 7 swing
    //   wide_vane: 1..5 left..right, 6 wide, 8 auto (0 keeps the current one)
    bool IrSendMitsubishiAc(bool power_on, int mode, int temperature, int fan, int vane,
                            int wide_vane = 0);
    // Loopback diagnostic: transmit the A/C frame under three drive variants and
    // report what the dock's own IR receiver saw, to prove light actually leaves
    // the emitter and which enable-polarity / carrier setting is right.
    std::string IrSelfTest();
    // Capture whatever a real remote sends (Mitsubishi Electric frame) and return the
    // decoded bytes plus timings, so the transmitted frame can be diffed against it.
    std::string IrLearn(int seconds);
    int IrAcCurrentTemperature() const { return ac_temperature_; }
    int IrAcCurrentMode() const { return ac_mode_; }
    int IrAcCurrentFan() const { return ac_fan_; }
    int IrAcCurrentVane() const { return ac_vane_; }
    bool IrAcPowered() const { return ac_power_; }

    void RegisterTools();

private:
    SensorDock() = default;

    bool InitExpandI2c();
    bool ProbeAht20();
    bool Aht20Init();
    bool InitRadar();
    bool InitBattery();
    bool InitIr();
    bool IrTransmit(const rmt_symbol_word_t* symbols, size_t count);
    static int AcDecode(const rmt_symbol_word_t* symbols, size_t count, uint8_t* out, int max_bytes);
    void ScanExpandBus();

    i2c_master_bus_handle_t expand_bus_ = nullptr;
    i2c_master_dev_handle_t aht20_ = nullptr;
    bool present_ = false;
    bool dock_detected_ = false;
    bool aht20_ready_ = false;

    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_ = nullptr;
    bool adc_ready_ = false;

    rmt_channel_handle_t ir_channel_ = nullptr;
    rmt_encoder_handle_t ir_encoder_ = nullptr;
    bool ir_ready_ = false;

    rmt_channel_handle_t ir_rx_channel_ = nullptr;
    QueueHandle_t ir_rx_queue_ = nullptr;
    rmt_symbol_word_t ir_rx_symbols_[192] = {};
    size_t ir_rx_count_ = 0;

    // Cached Mitsubishi A/C state: the remote is stateless, so every frame has
    // to carry the whole configuration; start from the known-good template.
    uint8_t ac_state_[18] = {0};
    bool ac_state_ready_ = false;
    bool ac_power_ = false;
    int ac_mode_ = 3;         // cool
    int ac_temperature_ = 24;
    int ac_fan_ = 0;          // auto
    int ac_vane_ = 0;         // auto
};

#endif  // SENSOR_DOCK_H

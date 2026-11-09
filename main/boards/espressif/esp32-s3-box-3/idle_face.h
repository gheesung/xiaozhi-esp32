#ifndef IDLE_FACE_H
#define IDLE_FACE_H

#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Idle clock face for the ESP32-S3-BOX-3.
//
// While the device idles, the stock UI shows the animated emoji face. This
// widget covers that area with a desktop weather-clock layout instead - big
// time with seconds, the current NEA condition drawn as an icon, the date,
// the outdoor temperature and the PSI badge - and disappears again the moment
// the device leaves the idle state.
//
// The clock is one fixed-width cell per digit: Montserrat's digits are
// proportional ('1' is 17.75 px against '0' at 32 px on the 48 px face), so a
// single "HH:MM" label changes width whenever a digit changes and drags the
// row with it. A cell never changes width, so nothing moves while time runs.
//
// Weather comes from Singapore's open data (NEA via data.gov.sg) and is
// refreshed by a low-priority background task, so the LVGL frame rate never
// depends on the network.
//
// Threading: every method except StartWeatherFetch() must be called with the
// display lock already held (DisplayLockGuard / LcdDisplay::Lock).
class IdleFace {
public:
    void Init(lv_obj_t* screen, int32_t width, int32_t height, int32_t top_offset, lv_color_t fg,
              lv_color_t bg);
    bool IsInitialized() const { return face_ != nullptr; }

    void SetVisible(bool visible);
    bool IsVisible() const { return visible_; }

    // Repaint the clock and pick up new weather/news when a fetch landed.
    void Refresh();

    // Spawn the NEA fetch task; idempotent, safe from any task.
    void StartWeatherFetch();

    // Ticker speed in px/second (0 = frozen). Request is async: the value is
    // applied on the next idle tick, where the LVGL lock is already held, so this
    // can be called from an MCP tool callback or any other task.
    void RequestNewsScrollSpeed(int32_t px_per_second);
    int32_t NewsScrollSpeed() const { return speed_.load(); }

private:
    enum class Icon { kNone, kSun, kMoon, kPartly, kCloud, kRain, kStorm };

    struct Weather {
        bool valid = false;
        float temperature_c = 0.0f;
        std::string condition;
        int psi = -1;
        int64_t fetched_at = 0;
    };

    // widgets
    lv_obj_t* face_ = nullptr;
    lv_obj_t* icon_box_ = nullptr;
    lv_obj_t* clock_box_ = nullptr;
    lv_obj_t* clock_cells_[4] = {nullptr, nullptr, nullptr, nullptr};  // HH MM
    lv_obj_t* second_cells_[2] = {nullptr, nullptr};                  // SS
    lv_obj_t* badge_ = nullptr;
    lv_obj_t* badge_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* temp_label_ = nullptr;
    lv_obj_t* area_label_ = nullptr;
    std::atomic<int32_t> speed_{0};       // px/second, 0 until the saved value is read
    std::atomic<int32_t> pending_speed_{-1};  // -1 = nothing requested
    lv_obj_t* news_box_ = nullptr;
    lv_obj_t* news_label_ = nullptr;
    lv_obj_t* condition_label_ = nullptr;
    lv_obj_t* updated_label_ = nullptr;

    lv_color_t fg_ = lv_color_white();
    lv_color_t bg_ = lv_color_black();
    bool visible_ = false;
    char shown_digits_[6] = {0};  // last painted HHMMSS, one char per cell
    char shown_date_[32] = {0};
    uint32_t shown_weather_version_ = 0;
    uint32_t shown_news_version_ = 0;
    int32_t idle_ticks_ = 0;
    int32_t debug_dumps_ = 0;
    int32_t debug_first_tick_ = 0;

    // weather, written by the fetch task only
    mutable std::mutex weather_mutex_;
    Weather weather_;
    uint32_t weather_version_ = 0;
    std::string news_;          // "headline  |  headline  |  headline"
    uint32_t news_version_ = 0;
    bool weather_task_started_ = false;

    static lv_obj_t* MakeDigitCell(lv_obj_t* parent, const lv_font_t* font, int32_t width,
                                   lv_color_t color);
    static int32_t MaxDigitWidth(const lv_font_t* font);
    void SetDigit(lv_obj_t* cell, int index, char digit);
    void StartNewsScroll();
    void ApplyPendingSpeed();
    static int32_t ClampSpeed(int32_t px_per_second);
    void BuildIcon(Icon icon);
    void ApplyWeather(const Weather& weather, bool night);
    void WeatherLoop();
    bool FetchWeather(Weather& out);
    bool FetchNews(std::string& out);
    void DumpIdleShotToSerial();
};

// Register the device-side tool that lets the model retune the ticker at runtime
// (`self.display.set_news_scroll_speed`). Call once, after the MCP server exists.
void RegisterIdleFaceTools();

#endif  // IDLE_FACE_H

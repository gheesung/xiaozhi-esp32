#include "idle_face.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "application.h"
#include "board.h"
#include "display/display.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "IdleFace"

LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_20);

namespace {

// Debug hook: log the first idle paint to the console as base64 JPEG so the
// layout can be checked without a camera on the screen. Flip to true, flash,
// capture the console with esp_idf_monitor and run
// ~/xiaozhi-fw-build/decode_serial_shot.py on the log. Off in normal builds.
constexpr bool kDumpIdleShotToSerial = false;
constexpr int kDebugChunk = 180;  // base64 characters per log line
// Home area used for both the NEA station and the 2-hour nowcast area name.
constexpr char kArea[] = "Sembawang";
constexpr char kTempUrl[] = "https://api.data.gov.sg/v1/environment/air-temperature";
constexpr char kForecastUrl[] = "https://api.data.gov.sg/v1/environment/2-hour-weather-forecast";
constexpr char kPsiUrl[] = "https://api.data.gov.sg/v1/environment/psi";
constexpr int32_t kIconSize = 56;
constexpr int32_t kRefreshSeconds = 600;  // NEA nowcast updates every 30 min, temperature every min
constexpr int32_t kRetrySeconds = 30;     // until the first successful fetch
// CNA's public RSS feed: ~15 KB, plain XML, no key. Headlines refresh every 2 h.
constexpr char kNewsUrl[] = "https://www.channelnewsasia.com/api/v1/rss-outbound-feed?_format=xml";
constexpr int32_t kNewsRefreshSeconds = 7200;
constexpr int32_t kNewsRetrySeconds = 300;
constexpr int kNewsHeadlines = 3;
constexpr int32_t kNewsScrollPxPerSecond = 6;

lv_color_t SunColor() { return lv_color_hex(0xFFB300); }
lv_color_t RainColor() { return lv_color_hex(0x4FC3F7); }

lv_color_t PsiColor(int psi) {
    if (psi <= 50) {
        return lv_color_hex(0x2E7D32);  // Good
    }
    if (psi <= 100) {
        return lv_color_hex(0x1565C0);  // Moderate
    }
    if (psi <= 200) {
        return lv_color_hex(0xE8710A);  // Unhealthy
    }
    if (psi <= 300) {
        return lv_color_hex(0xC62828);  // Very unhealthy
    }
    return lv_color_hex(0x7B1FA2);      // Hazardous
}

std::string Lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

lv_obj_t* NewBox(lv_obj_t* parent, int32_t width, int32_t height) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

// A filled, optionally rounded rectangle - the only primitive the icons need.
void Shape(lv_obj_t* parent, int32_t x, int32_t y, int32_t width, int32_t height, lv_color_t color,
           int32_t radius) {
    lv_obj_t* obj = NewBox(parent, width, height);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
}

void CloudShape(lv_obj_t* parent, lv_color_t color, int32_t dy) {
    Shape(parent, 10, 20 + dy, 20, 20, color, LV_RADIUS_CIRCLE);  // left lobe
    Shape(parent, 20, 10 + dy, 26, 26, color, LV_RADIUS_CIRCLE);  // middle lobe
    Shape(parent, 34, 18 + dy, 20, 20, color, LV_RADIUS_CIRCLE);  // right lobe
    Shape(parent, 10, 28 + dy, 44, 12, color, 6);                 // base
}

// Read a member path safely; returns nullptr when any step is missing.
const cJSON* Path(const cJSON* node, const char* a, const char* b = nullptr,
                  const char* c = nullptr) {
    for (const char* key : {a, b, c}) {
        if (key == nullptr) {
            break;
        }
        if (node == nullptr) {
            return nullptr;
        }
        node = cJSON_GetObjectItemCaseSensitive(node, key);
    }
    return node;
}

// The one face on this device, so an MCP tool callback can reach it.
IdleFace* g_face = nullptr;

// Every NEA endpoint nests its payload under items[0].
const cJSON* FirstItem(const cJSON* root) {
    const cJSON* items = Path(root, "items");
    return cJSON_IsArray(items) ? cJSON_GetArrayItem(items, 0) : nullptr;
}

}  // namespace

void IdleFace::Init(lv_obj_t* screen, int32_t width, int32_t height, int32_t top_offset,
                    lv_color_t fg, lv_color_t bg) {
    fg_ = fg;
    bg_ = bg;
    g_face = this;

    // The face covers everything below the status bar, so the network and
    // battery icons stay readable while it is up.
    face_ = NewBox(screen, width, height - top_offset);
    lv_obj_align(face_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(face_, bg_, 0);
    lv_obj_set_style_bg_opa(face_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(face_, 6, 0);
    lv_obj_set_style_pad_row(face_, 2, 0);
    lv_obj_set_flex_flow(face_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(face_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(face_, LV_OBJ_FLAG_HIDDEN);

    // Row 1: condition icon on the left, the clock centred. Absolute placement
    // rather than a flex row: the clock stays centred whatever the digits are,
    // and a wider cell can never push into the icon.
    lv_obj_t* row1 = NewBox(face_, LV_PCT(100), kIconSize);
    icon_box_ = NewBox(row1, kIconSize, kIconSize);
    lv_obj_align(icon_box_, LV_ALIGN_LEFT_MID, 0, 0);

    clock_box_ = NewBox(row1, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(clock_box_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(clock_box_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    const int32_t big_cell = MaxDigitWidth(&lv_font_montserrat_48);
    const int32_t small_cell = MaxDigitWidth(&lv_font_montserrat_20);
    lv_point_t colon_size;
    lv_txt_get_size(&colon_size, ":", &lv_font_montserrat_48, 0, 0, LV_COORD_MAX,
                    LV_TEXT_FLAG_NONE);

    clock_cells_[0] = MakeDigitCell(clock_box_, &lv_font_montserrat_48, big_cell, fg_);
    clock_cells_[1] = MakeDigitCell(clock_box_, &lv_font_montserrat_48, big_cell, fg_);
    lv_obj_t* colon = MakeDigitCell(clock_box_, &lv_font_montserrat_48, colon_size.x, fg_);
    lv_label_set_text(colon, ":");
    clock_cells_[2] = MakeDigitCell(clock_box_, &lv_font_montserrat_48, big_cell, fg_);
    clock_cells_[3] = MakeDigitCell(clock_box_, &lv_font_montserrat_48, big_cell, fg_);
    NewBox(clock_box_, 6, 1);  // gap before the seconds
    second_cells_[0] = MakeDigitCell(clock_box_, &lv_font_montserrat_20, small_cell, fg_);
    second_cells_[1] = MakeDigitCell(clock_box_, &lv_font_montserrat_20, small_cell, fg_);

    // Bottom-aligned boxes still put the small digits lower: LVGL's base_line is the
    // baseline measured UP from the box bottom, so the small font's baseline sits
    // (base_line_20 - base_line_48) px below the big one. Lift the seconds by exactly
    // that, and the two glyph bottoms land on one line.
    const int32_t lift = lv_font_montserrat_48.base_line - lv_font_montserrat_20.base_line;
    if (lift != 0) {
        for (lv_obj_t* cell : second_cells_) {
            lv_obj_set_style_translate_y(cell, -lift, 0);
        }
    }

    // Divider, as in the reference clock face.
    lv_obj_t* divider = NewBox(face_, LV_PCT(94), 2);
    lv_obj_set_style_bg_color(divider, fg_, 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_20, 0);
    lv_obj_set_style_radius(divider, 1, 0);

    // Row 2: date on the left, PSI badge on the right.
    lv_obj_t* row2 = NewBox(face_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    date_label_ = lv_label_create(row2);
    lv_obj_set_style_text_font(date_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(date_label_, fg_, 0);
    lv_label_set_text(date_label_, "--");

    // PSI sits at the top right, directly under the battery icon, as a short pill.
    badge_ = NewBox(face_, 54, 22);
    lv_obj_add_flag(badge_, LV_OBJ_FLAG_FLOATING);  // out of the flex column's flow
    lv_obj_align(badge_, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(badge_, 11, 0);
    lv_obj_set_style_bg_color(badge_, PsiColor(0), 0);
    lv_obj_set_style_bg_opa(badge_, LV_OPA_COVER, 0);
    badge_label_ = lv_label_create(badge_);
    lv_obj_set_style_text_font(badge_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(badge_label_, lv_color_white(), 0);
    lv_label_set_text(badge_label_, "PSI --");
    lv_obj_center(badge_label_);
    lv_obj_add_flag(badge_, LV_OBJ_FLAG_HIDDEN);

    // Row 3: nowcast text on the left, outdoor temperature on the right.
    lv_obj_t* row3 = NewBox(face_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row3, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row3, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Left side of row 3: the area on one line, the nowcast on the next, so a
    // long condition can never collide with the temperature on the right.
    lv_obj_t* weather_text = NewBox(row3, LV_PCT(72), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(weather_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(weather_text, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(weather_text, 0, 0);

    area_label_ = lv_label_create(weather_text);
    lv_obj_set_style_text_font(area_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(area_label_, fg_, 0);
    lv_obj_set_width(area_label_, LV_PCT(100));
    lv_label_set_text(area_label_, kArea);

    condition_label_ = lv_label_create(weather_text);
    lv_obj_set_style_text_font(condition_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(condition_label_, fg_, 0);
    lv_obj_set_width(condition_label_, LV_PCT(100));
    // Two rows is the whole budget: elide rather than wrap into a third line.
    lv_label_set_long_mode(condition_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(condition_label_, "");

    lv_obj_t* reading_box = NewBox(row3, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(reading_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(reading_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(reading_box, 0, 0);

    temp_label_ = lv_label_create(reading_box);
    lv_obj_set_style_text_font(temp_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(temp_label_, fg_, 0);
    lv_label_set_text(temp_label_, "--");

    // NEA read time: directly under the temperature, same colour as it.
    updated_label_ = lv_label_create(reading_box);
    lv_obj_set_style_text_font(updated_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(updated_label_, fg_, 0);
    lv_label_set_text(updated_label_, "");

    // Bottom row: the latest headlines. A fixed-width clipping box with a label that
    // is as wide as its text, and the offset animated by hand - LVGL's own
    // SCROLL_CIRCULAR speed kept running away, and this is exact px/second.
    news_box_ = NewBox(face_, LV_PCT(100), 20);  // fixed height: SIZE_CONTENT collapses in a flex column
    {
        Settings settings("idle_face", false);
        speed_.store(ClampSpeed(settings.GetInt("news_speed", kNewsScrollPxPerSecond)));
    }

    news_label_ = lv_label_create(news_box_);
    lv_obj_set_style_text_font(news_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(news_label_, fg_, 0);
    lv_label_set_long_mode(news_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(news_label_, LV_SIZE_CONTENT);
    lv_obj_set_pos(news_label_, 0, 1);
    lv_obj_set_height(news_label_, 18);
    lv_label_set_text(news_label_, "");

    BuildIcon(Icon::kNone);
    ESP_LOGI(TAG, "idle face ready (%dx%d below y=%d), clock cells %dpx/%dpx", (int)width,
             (int)(height - top_offset), (int)top_offset, (int)big_cell, (int)small_cell);
}

void IdleFace::SetVisible(bool visible) {
    if (face_ == nullptr || visible == visible_) {
        return;
    }
    visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(face_);
        memset(shown_digits_, 0, sizeof(shown_digits_));  // force a full repaint
        shown_date_[0] = '\0';
    } else {
        lv_obj_add_flag(face_, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t* IdleFace::MakeDigitCell(lv_obj_t* parent, const lv_font_t* font, int32_t width,
                                  lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    // A fixed cell with centred text: the digit may be narrower than the cell,
    // but the cell itself never changes width, so nothing on screen moves.
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, "0");
    return label;
}

int32_t IdleFace::MaxDigitWidth(const lv_font_t* font) {
    int32_t widest = 0;
    for (char digit = '0'; digit <= '9'; digit++) {
        const char text[2] = {digit, '\0'};
        lv_point_t size;
        lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > widest) {
            widest = size.x;
        }
    }
    return widest;
}

void IdleFace::SetDigit(lv_obj_t* cell, int index, char digit) {
    if (cell == nullptr || shown_digits_[index] == digit) {
        return;
    }
    shown_digits_[index] = digit;
    const char text[2] = {digit, '\0'};
    lv_label_set_text(cell, text);
}

// Move the headline line at a fixed px/second inside its clipping box, looping.
void IdleFace::StartNewsScroll() {
    if (news_label_ == nullptr || news_box_ == nullptr) {
        return;
    }
    lv_anim_delete(news_label_, nullptr);
    lv_obj_update_layout(news_label_);
    const int32_t text_w = lv_obj_get_width(news_label_);
    const int32_t box_w = lv_obj_get_width(news_box_);
    if (text_w <= 0 || box_w <= 0 || text_w <= box_w) {
        lv_obj_set_x(news_label_, 0);
        return;
    }

    const int32_t speed = speed_.load();
    if (speed <= 0) {
        lv_obj_set_x(news_label_, 0);  // frozen: park the line at the left edge
        return;
    }
    const int32_t start = 0;           // visible from the first frame
    const int32_t end = -text_w - 24;  // a small gap before it wraps round
    static lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, news_label_);
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        lv_obj_set_x(static_cast<lv_obj_t*>(var), value);
    });
    lv_anim_set_values(&anim, start, end);
    const uint32_t duration = lv_anim_speed_to_time(speed, start, end);
    lv_anim_set_duration(&anim, duration);
    ESP_LOGI(TAG, "news ticker: text %d px, box %d px at (%d,%d) %dx%d, %u s per pass at %d px/s",
             (int)text_w, (int)box_w, (int)lv_obj_get_x(news_label_),
             (int)lv_obj_get_y(news_label_), (int)lv_obj_get_width(news_label_),
             (int)lv_obj_get_height(news_label_), (unsigned)(duration / 1000), (int)speed);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

int32_t IdleFace::ClampSpeed(int32_t px_per_second) {
    if (px_per_second < 0) {
        return 0;
    }
    return px_per_second > 60 ? 60 : px_per_second;
}

void IdleFace::RequestNewsScrollSpeed(int32_t px_per_second) {
    const int32_t clamped = ClampSpeed(px_per_second);
    if (clamped == speed_.load()) {
        return;  // nothing to do, and no need to touch NVS
    }
    speed_.store(clamped);
    pending_speed_.store(clamped);
    Settings settings("idle_face", true);
    settings.SetInt("news_speed", clamped);
    ESP_LOGI(TAG, "news scroll speed requested: %d px/s", (int)clamped);
}

void IdleFace::ApplyPendingSpeed() {
    const int32_t requested = pending_speed_.exchange(-1);
    if (requested < 0 || news_label_ == nullptr) {
        return;
    }
    StartNewsScroll();  // re-reads speed_ and restarts the offset animation
}

void IdleFace::Refresh() {
    if (face_ == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    const bool night = local.tm_hour < 7 || local.tm_hour >= 19;

    if (local.tm_year >= 2025 - 1900) {
        const char digits[6] = {
            static_cast<char>('0' + local.tm_hour / 10),
            static_cast<char>('0' + local.tm_hour % 10),
            static_cast<char>('0' + local.tm_min / 10),
            static_cast<char>('0' + local.tm_min % 10),
            static_cast<char>('0' + local.tm_sec / 10),
            static_cast<char>('0' + local.tm_sec % 10),
        };
        for (int i = 0; i < 4; i++) {
            SetDigit(clock_cells_[i], i, digits[i]);
        }
        for (int i = 0; i < 2; i++) {
            SetDigit(second_cells_[i], 4 + i, digits[4 + i]);
        }

        char date[32];
        strftime(date, sizeof(date), "%a %d %b", &local);
        if (strcmp(date, shown_date_) != 0) {
            strcpy(shown_date_, date);
            lv_label_set_text(date_label_, date);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            SetDigit(clock_cells_[i], i, '-');
        }
        for (int i = 0; i < 2; i++) {
            SetDigit(second_cells_[i], 4 + i, '-');
        }
    }

    Weather latest;
    std::string latest_news;
    uint32_t version = 0;
    uint32_t news_stamp = 0;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        version = weather_version_;
        news_stamp = news_version_;
        latest = weather_;
        latest_news = news_;
    }
    if (version != shown_weather_version_) {
        shown_weather_version_ = version;
        ApplyWeather(latest, night);
    }
    if (news_stamp != shown_news_version_) {
        shown_news_version_ = news_stamp;
        lv_label_set_text(news_label_, latest_news.c_str());
        StartNewsScroll();
    }
    ApplyPendingSpeed();

    idle_ticks_++;
    if (kDumpIdleShotToSerial) {
        // Debug only: two frames six seconds apart prove the clock does not move,
        // and the geometry trace shows where the block sits.
        bool have_weather;
        {
            std::lock_guard<std::mutex> lock(weather_mutex_);
            have_weather = weather_.valid && !news_.empty();
        }
        if (have_weather && idle_ticks_ >= 3 && debug_dumps_ == 0) {
            ESP_LOGI(TAG, "clock box x=%d w=%d", (int)lv_obj_get_x(clock_box_),
                     (int)lv_obj_get_width(clock_box_));
            DumpIdleShotToSerial();
            debug_dumps_ = 1;
            debug_first_tick_ = idle_ticks_;
        } else if (have_weather && debug_dumps_ == 1 && idle_ticks_ >= debug_first_tick_ + 6) {
            ESP_LOGI(TAG, "clock box x=%d w=%d", (int)lv_obj_get_x(clock_box_),
                     (int)lv_obj_get_width(clock_box_));
            DumpIdleShotToSerial();
            debug_dumps_ = 2;
        }
    }
}

// Base64 over ESP_LOGI: the only way out of this board without a network port
// of our own (the host firewall only lets the published service ports through).
void IdleFace::DumpIdleShotToSerial() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    std::string jpeg;
    if (!display->SnapshotToJpeg(jpeg, 85)) {
        ESP_LOGW(TAG, "debug snapshot failed");
        return;
    }
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    ESP_LOGI(TAG, "SHOT begin %u bytes", (unsigned)jpeg.size());
    std::string line;
    line.reserve(kDebugChunk + 16);
    size_t position = 0;
    while (position + 2 < jpeg.size() || position < jpeg.size()) {
        const uint32_t triple = (static_cast<uint8_t>(jpeg[position]) << 16) |
                                (position + 1 < jpeg.size()
                                     ? static_cast<uint8_t>(jpeg[position + 1]) << 8
                                     : 0) |
                                (position + 2 < jpeg.size()
                                     ? static_cast<uint8_t>(jpeg[position + 2])
                                     : 0);
        const int remaining = static_cast<int>(jpeg.size() - position);
        line += kAlphabet[(triple >> 18) & 0x3F];
        line += kAlphabet[(triple >> 12) & 0x3F];
        line += remaining > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        line += remaining > 2 ? kAlphabet[triple & 0x3F] : '=';
        position += 3;
        if (line.size() >= static_cast<size_t>(kDebugChunk)) {
            ESP_LOGI(TAG, "SHOT %s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) {
        ESP_LOGI(TAG, "SHOT %s", line.c_str());
    }
    ESP_LOGI(TAG, "SHOT end");
}

void IdleFace::ApplyWeather(const Weather& weather, bool night) {
    if (!weather.valid) {
        lv_label_set_text(temp_label_, "--");
        lv_label_set_text(condition_label_, "no weather data");
        lv_label_set_text(updated_label_, "");
        lv_obj_add_flag(badge_, LV_OBJ_FLAG_HIDDEN);
        BuildIcon(Icon::kNone);
        return;
    }

    char temperature[16];
    snprintf(temperature, sizeof(temperature), "%.1fC", weather.temperature_c);
    lv_label_set_text(temp_label_, temperature);

    const std::string condition = weather.condition.empty() ? std::string("--") : weather.condition;
    lv_label_set_text(area_label_, kArea);
    lv_label_set_text(condition_label_, condition.c_str());

    const std::string lowered = Lower(condition);
    Icon icon = Icon::kSun;
    if (lowered.find("thunder") != std::string::npos) {
        icon = Icon::kStorm;
    } else if (lowered.find("rain") != std::string::npos ||
               lowered.find("shower") != std::string::npos ||
               lowered.find("drizzle") != std::string::npos) {
        icon = Icon::kRain;
    } else if (lowered.find("partly") != std::string::npos) {
        icon = Icon::kPartly;
    } else if (lowered.find("cloud") != std::string::npos ||
               lowered.find("overcast") != std::string::npos ||
               lowered.find("windy") != std::string::npos ||
               lowered.find("hazy") != std::string::npos ||
               lowered.find("mist") != std::string::npos ||
               lowered.find("fog") != std::string::npos) {
        icon = Icon::kCloud;
    } else if (lowered.find("sunny") != std::string::npos ||
               lowered.find("fair") != std::string::npos ||
               lowered.find("clear") != std::string::npos) {
        icon = night ? Icon::kMoon : Icon::kSun;
    } else {
        icon = night ? Icon::kMoon : Icon::kSun;
    }
    BuildIcon(icon);

    if (weather.psi >= 0) {
        char psi[16];
        snprintf(psi, sizeof(psi), "PSI %d", weather.psi);
        lv_label_set_text(badge_label_, psi);
        lv_obj_set_style_bg_color(badge_, PsiColor(weather.psi), 0);
        lv_obj_remove_flag(badge_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(badge_, LV_OBJ_FLAG_HIDDEN);
    }

    if (weather.fetched_at > 0) {
        time_t stamp = static_cast<time_t>(weather.fetched_at);
        struct tm local;
        localtime_r(&stamp, &local);
        char updated[32];
        strftime(updated, sizeof(updated), "NEA %H:%M", &local);
        lv_label_set_text(updated_label_, updated);
    }
}

void IdleFace::BuildIcon(Icon icon) {
    if (icon_box_ == nullptr) {
        return;
    }
    lv_obj_clean(icon_box_);

    switch (icon) {
        case Icon::kSun: {
            // Dot rays: no transforms, so it renders identically on every LVGL build.
            for (int i = 0; i < 8; i++) {
                const float angle = i * (float)M_PI / 4.0f;
                const int32_t x = 28 + static_cast<int32_t>(lroundf(21.0f * cosf(angle))) - 3;
                const int32_t y = 28 + static_cast<int32_t>(lroundf(21.0f * sinf(angle))) - 3;
                Shape(icon_box_, x, y, 6, 6, SunColor(), LV_RADIUS_CIRCLE);
            }
            Shape(icon_box_, 15, 15, 26, 26, SunColor(), LV_RADIUS_CIRCLE);
            break;
        }
        case Icon::kMoon: {
            // A disc with the background punched out of it.
            Shape(icon_box_, 10, 12, 32, 32, SunColor(), LV_RADIUS_CIRCLE);
            Shape(icon_box_, 24, 6, 30, 30, bg_, LV_RADIUS_CIRCLE);
            break;
        }
        case Icon::kPartly: {
            Shape(icon_box_, 6, 6, 24, 24, SunColor(), LV_RADIUS_CIRCLE);
            Shape(icon_box_, 20, 26, 18, 18, fg_, LV_RADIUS_CIRCLE);
            Shape(icon_box_, 28, 18, 22, 22, fg_, LV_RADIUS_CIRCLE);
            Shape(icon_box_, 40, 28, 14, 14, fg_, LV_RADIUS_CIRCLE);
            Shape(icon_box_, 20, 34, 34, 10, fg_, 5);
            break;
        }
        case Icon::kCloud: {
            CloudShape(icon_box_, fg_, 2);
            break;
        }
        case Icon::kRain: {
            CloudShape(icon_box_, fg_, -6);
            Shape(icon_box_, 18, 38, 4, 14, RainColor(), 2);
            Shape(icon_box_, 29, 42, 4, 14, RainColor(), 2);
            Shape(icon_box_, 40, 38, 4, 14, RainColor(), 2);
            break;
        }
        case Icon::kStorm: {
            CloudShape(icon_box_, fg_, -6);
            static lv_point_precise_t bolt[] = {{32, 32}, {23, 44}, {29, 44}, {22, 56}};
            lv_obj_t* line = lv_line_create(icon_box_);
            lv_line_set_points(line, bolt, 4);
            lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_line_width(line, 4, 0);
            lv_obj_set_style_line_color(line, SunColor(), 0);
            lv_obj_set_style_line_rounded(line, true, 0);
            break;
        }
        case Icon::kNone:
        default:
            break;
    }
}

void IdleFace::StartWeatherFetch() {
    if (weather_task_started_ || face_ == nullptr) {
        return;
    }
    weather_task_started_ = true;
    BaseType_t created = xTaskCreate(
        [](void* arg) {
            static_cast<IdleFace*>(arg)->WeatherLoop();
            vTaskDelete(nullptr);
        },
        "idle_weather", 10240, this, 1, nullptr);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "weather task create failed");
        weather_task_started_ = false;
    }
}

void IdleFace::WeatherLoop() {
    int64_t last_weather = esp_timer_get_time() - (int64_t)kRefreshSeconds * 1000000;
    int64_t last_news = esp_timer_get_time() - (int64_t)kNewsRefreshSeconds * 1000000;
    int32_t weather_gap = kRefreshSeconds;
    int32_t news_gap = kNewsRefreshSeconds;

    while (true) {
        const int64_t now = esp_timer_get_time();
        if (Application::GetInstance().GetDeviceState() != kDeviceStateWifiConfiguring) {
            if (now - last_weather >= (int64_t)weather_gap * 1000000) {
                Weather fetched;
                const bool ok = FetchWeather(fetched);
                if (ok) {
                    std::lock_guard<std::mutex> lock(weather_mutex_);
                    weather_ = fetched;
                    weather_version_++;
                }
                last_weather = now;
                weather_gap = ok ? kRefreshSeconds : kRetrySeconds;
            }
            if (now - last_news >= (int64_t)news_gap * 1000000) {
                std::string headlines;
                const bool ok = FetchNews(headlines);
                if (ok) {
                    std::lock_guard<std::mutex> lock(weather_mutex_);
                    news_ = headlines;
                    news_version_++;
                }
                last_news = now;
                news_gap = ok ? kNewsRefreshSeconds : kNewsRetrySeconds;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

namespace {

bool HttpGet(const char* url, std::string& body) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    auto http = network->CreateHttp(4);
    if (!http) {
        return false;
    }
    http->SetTimeout(8000);
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGW(TAG, "GET %s: %s", url, opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGW(TAG, "GET %s: %s", url, status.error().ToString().c_str());
        http->Close();
        return false;
    }
    if (*status != 200) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, *status);
        http->Close();
        return false;
    }
    body = http->ReadAll();
    http->Close();
    return !body.empty();
}

}  // namespace

bool IdleFace::FetchWeather(Weather& out) {
    std::string body;

    // Outdoor temperature: the Sembawang meteorological station (S80) is the
    // closest one to the house; fall back to any reading if it is missing.
    if (HttpGet(kTempUrl, body)) {
        cJSON* root = cJSON_Parse(body.c_str());
        const cJSON* list = Path(FirstItem(root), "readings");
        if (cJSON_IsArray(list)) {
            const cJSON* candidate = nullptr;
            const cJSON* reading = nullptr;
            cJSON_ArrayForEach(reading, list) {
                if (candidate == nullptr) {
                    candidate = reading;
                }
                const cJSON* id = Path(reading, "station_id");
                if (cJSON_IsString(id) && strcmp(id->valuestring, "S80") == 0) {
                    candidate = reading;
                    break;
                }
            }
            const cJSON* value = Path(candidate, "value");
            if (cJSON_IsNumber(value)) {
                out.temperature_c = static_cast<float>(value->valuedouble);
            }
        }
        cJSON_Delete(root);
    }

    // 2-hour nowcast text for the home area -> the icon and the caption.
    if (HttpGet(kForecastUrl, body)) {
        cJSON* root = cJSON_Parse(body.c_str());
        const cJSON* list = Path(FirstItem(root), "forecasts");
        if (cJSON_IsArray(list)) {
            const cJSON* entry = nullptr;
            cJSON_ArrayForEach(entry, list) {
                const cJSON* area = Path(entry, "area");
                if (cJSON_IsString(area) && strcmp(area->valuestring, kArea) == 0) {
                    const cJSON* forecast = Path(entry, "forecast");
                    if (cJSON_IsString(forecast)) {
                        out.condition = forecast->valuestring;
                    }
                    break;
                }
            }
        }
        cJSON_Delete(root);
    }

    // 24-hour PSI for the north region -> the badge.
    if (HttpGet(kPsiUrl, body)) {
        cJSON* root = cJSON_Parse(body.c_str());
        const cJSON* psi =
            Path(FirstItem(root), "readings", "psi_twenty_four_hourly", "north");
        if (cJSON_IsNumber(psi)) {
            out.psi = psi->valueint;
        }
        cJSON_Delete(root);
    }

    out.valid = out.temperature_c != 0.0f || !out.condition.empty() || out.psi >= 0;
    out.fetched_at = static_cast<int64_t>(time(nullptr));
    if (out.valid) {
        ESP_LOGI(TAG, "weather: %.1fC, %s, PSI %d", out.temperature_c, out.condition.c_str(),
                 out.psi);
    }
    return out.valid;
}

namespace {

std::string DecodeEntities(const std::string& text) {
    static const struct {
        const char* entity;
        const char* plain;
    } kMap[] = {{"&amp;", "&"},  {"&quot;", "\""}, {"&#039;", "'"}, {"&#39;", "'"},
                {"&apos;", "'"}, {"&lt;", "<"},   {"&gt;", ">"},   {"&nbsp;", " "}};
    std::string out = text;
    for (const auto& entry : kMap) {
        size_t pos = 0;
        while ((pos = out.find(entry.entity, pos)) != std::string::npos) {
            out.replace(pos, strlen(entry.entity), entry.plain);
            pos += strlen(entry.plain);
        }
    }
    return out;
}

// Keep only what the built-in font can draw (ASCII); CNA sends curly quotes and em
// dashes, which LVGL would render as empty boxes.
std::string ToFontSafe(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool last_space = true;
    for (unsigned char c : text) {
        if (c < 0x20) {
            continue;
        }
        if (c >= 0x80) {
            continue;
        }
        if (c == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }
        out.push_back(static_cast<char>(c));
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace

bool IdleFace::FetchNews(std::string& out) {
    std::string body;
    if (!HttpGet(kNewsUrl, body)) {
        return false;
    }

    // Minimal RSS scrape: the first <title> is the channel name, the rest are items.
    std::string joined;
    size_t cursor = 0;
    int taken = 0;
    while (taken < kNewsHeadlines) {
        const size_t start = body.find("<title>", cursor);
        if (start == std::string::npos) {
            break;
        }
        const size_t end = body.find("</title>", start);
        if (end == std::string::npos) {
            break;
        }
        std::string title = body.substr(start + 7, end - start - 7);
        cursor = end + 8;
        if (title == "Latest News") {
            continue;  // channel title
        }
        title = ToFontSafe(DecodeEntities(title));
        if (title.size() < 8) {
            continue;
        }
        if (!joined.empty()) {
            joined += "   |   ";
        }
        joined += title;
        taken++;
    }

    if (joined.empty()) {
        ESP_LOGW(TAG, "news: no headlines parsed (%u bytes)", (unsigned)body.size());
        return false;
    }
    if (joined.size() > 240) {
        joined.resize(240);
    }
    out = joined;
    ESP_LOGI(TAG, "news: %s", out.c_str());
    return true;
}

// --------------------------------------------------------------------------- //
// Device-side tool: let the model retune the ticker when the user asks
// --------------------------------------------------------------------------- //

void RegisterIdleFaceTools() {
    McpServer::GetInstance().AddTool(
        "self.display.set_news_scroll_speed",
        "Change how fast the news ticker on the idle screen scrolls. Use it whenever the user asks "
        "to speed up, slow down, stop or restart the news scrolling (e.g. 'the news is too fast', "
        "'scroll it faster', 'stop the news').\n"
        "Values are pixels per second of the 320 px wide screen: 0 freezes the ticker, 6 is the "
        "default calm pace, 12 is twice as fast, the maximum is 60. When the user gives no number, "
        "move in steps of about 3 px/s (halve or double for 'much faster/slower').\n"
        "Parameters: px_per_second (absolute target, 0-60, 0 means 'not specified'), delta "
        "(relative change, e.g. +3 or -2). The new speed is applied within a second and is saved, "
        "so it survives a reboot.\n"
        "Return value: JSON with the applied and previous px_per_second.",
        PropertyList({Property("px_per_second", kPropertyTypeInteger, 0, 0, 60),
                      Property("delta", kPropertyTypeInteger, 0, -60, 60)}),
        [](const PropertyList& properties) -> ReturnValue {
            if (g_face == nullptr) {
                return std::string("{\"success\": false, \"error\": \"idle screen not ready\"}");
            }
            IdleFace& face = *g_face;
            const int32_t previous = face.NewsScrollSpeed();
            int32_t target = properties["px_per_second"].value<int>();
            const int32_t delta = properties["delta"].value<int>();
            if (target <= 0) {
                target = previous + delta;
            }
            face.RequestNewsScrollSpeed(target);
            const int32_t applied = face.NewsScrollSpeed();
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"success\": true, \"px_per_second\": %d, \"previous\": %d}",
                     (int)applied, (int)previous);
            return std::string(buf);
        });
}

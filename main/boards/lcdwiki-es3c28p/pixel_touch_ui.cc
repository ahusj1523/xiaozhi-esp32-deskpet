#include "pixel_touch_ui.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "settings.h"

#include <material_symbols.h>

LV_FONT_DECLARE(font_noto_sans_basic_14_1);
LV_FONT_DECLARE(font_noto_sans_basic_16_4);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
#if CONFIG_LCDWIKI_ES3C28P_LANDSCAPE
LV_FONT_DECLARE(es3c28p_ui_clock);
LV_FONT_DECLARE(es3c28p_ui_heading);
#endif

#if CONFIG_WEATHER_DASHBOARD
#include "almanac_service.h"
#include "weather_service.h"
#endif

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <algorithm>
#include <cstdint>
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
#include <esp_log.h>
#endif

namespace {
constexpr uint32_t kBg = 0x071326;
constexpr uint32_t kPanel = 0x10263B;
constexpr uint32_t kBorder = 0x345C74;
constexpr uint32_t kText = 0xEBF9FE;
constexpr uint32_t kMuted = 0xACC8D6;
constexpr uint32_t kAccent = 0x80E9DD;

int DaysInMonth(int year, int month) {
    static constexpr int lengths[12] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) return 29;
    return lengths[month - 1];
}
int WeekdayOfFirst(int year, int month) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = 1;
    value.tm_hour = 12;
    value.tm_isdst = -1;
    std::mktime(&value);
    return value.tm_wday;
}

void StylePanel(lv_obj_t* obj, uint32_t bg, uint32_t border) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, int x, int y, int w, int h,
                uint32_t color = kText, const lv_font_t* font = nullptr) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, h);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font != nullptr)
        lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
    const auto* actual_font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (actual_font != nullptr && h < actual_font->line_height) {
        ESP_LOGW("ES3C28P_UI", "label clipped: text=%s box_h=%d font_h=%d", text, h,
                 static_cast<int>(actual_font->line_height));
    }
#endif
    return label;
}

lv_obj_t* Button(lv_obj_t* parent, const char* text, int x, int y, int w, int h,
                 lv_event_cb_t callback, void* user_data, const char* icon = nullptr) {
    lv_obj_t* button = lv_obj_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, h);
    StylePanel(button, kPanel, kBorder);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(kText), 0);
    if (icon != nullptr) {
        auto* glyph = lv_label_create(button);
        lv_label_set_text(glyph, icon);
        auto* theme = static_cast<LvglTheme*>(Board::GetInstance().GetDisplay()->GetTheme());
        lv_obj_set_style_text_font(glyph, theme->icon_font()->font(), 0);
        lv_obj_set_style_text_color(glyph, lv_color_hex(kAccent), 0);
        lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 7, 0);
        if (text[0] != '\0') {
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 30, 0);
        } else {
            lv_obj_center(glyph);
        }
    } else {
        lv_obj_center(label);
    }
    return button;
}

// Original V1.1 #book SVG drawn as three native line segments.
// Only the 18px action glyph and 39px player cover are needed; no bitmap
// buffers, runtime SVG parser or additional font component.
void DrawPrototypeBook(lv_obj_t* parent, bool large, int x, int y, uint32_t color) {
    static const lv_point_precise_t small_left[] = {
        {9, 4}, {6, 3}, {2, 4}, {2, 15}, {5, 14}, {7, 15}, {9, 17},
    };
    static const lv_point_precise_t small_right[] = {
        {9, 4}, {11, 3}, {14, 3}, {16, 4}, {16, 15}, {13, 14}, {11, 15}, {9, 17},
    };
    static const lv_point_precise_t small_spine[] = {{9, 4}, {9, 17}};
    static const lv_point_precise_t big_left[] = {
        {19, 10}, {15, 7}, {8, 7}, {4, 8}, {4, 31}, {9, 29}, {14, 29}, {19, 33},
    };
    static const lv_point_precise_t big_right[] = {
        {19, 10}, {24, 7}, {31, 7}, {35, 8}, {35, 31}, {30, 29}, {24, 29}, {19, 33},
    };
    static const lv_point_precise_t big_spine[] = {{19, 10}, {19, 33}};
    const lv_point_precise_t* paths[] = {
        large ? big_left : small_left,
        large ? big_right : small_right,
        large ? big_spine : small_spine,
    };
    const uint32_t lengths[] = {
        large ? 8u : 7u, 8u, 2u,
    };
    for (int i = 0; i < 3; ++i) {
        auto* line = lv_line_create(parent);
        lv_line_set_points(line, paths[i], lengths[i]);
        lv_obj_set_pos(line, x, y);
        lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
        lv_obj_set_style_line_width(line, large ? 2 : 1, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Use the existing LVGL slider with a 22px touch footprint and 7px
// painted track. The previous 22px *track* plus 5px knob padding created
// a 32px white block that covered adjacent settings text on real hardware.
lv_obj_t* CompactSlider(lv_obj_t* parent, int x, int y, int width) {
    auto* field = lv_obj_create(parent);
    lv_obj_remove_style_all(field);
    lv_obj_set_pos(field, x, y);
    lv_obj_set_size(field, width, 22);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_CLICKABLE);

    auto* slider = lv_slider_create(field);
    lv_obj_remove_style_all(slider);
    lv_obj_set_pos(slider, 0, 7);
    lv_obj_set_size(slider, width, 7);
    lv_obj_set_ext_click_area(slider, 8);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2e5066), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(slider, lv_color_hex(0x83a7b9), LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x7be7d7), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 1, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xa4f9eb), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(0xf0fffc), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);  // 7 + 2*5 = 17px
    return slider;
}

void SetIfChanged(lv_obj_t* label, const char* value) {
    if (label != nullptr && strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}
}  // namespace

PixelTouchUI::PixelTouchUI(lv_obj_t* screen, lv_obj_t* chat_content,
                           SetRobotCompact set_robot_compact, void* robot_context)
    : chat_content_(chat_content),
      set_robot_compact_(set_robot_compact),
      robot_context_(robot_context) {
    auto* theme = static_cast<LvglTheme*>(Board::GetInstance().GetDisplay()->GetTheme());
    RefreshTextFonts(theme->text_font()->font());
    Settings settings("pixel_ui");
    child_mode_ = settings.GetBool("child_mode", true);
    root_ = lv_obj_create(screen);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, 320, 240);
    StylePanel(root_, 0x0a1525, 0x0a1525);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_bg_grad_color(root_, lv_color_hex(0x111f30), 0);
    lv_obj_set_style_bg_grad_dir(root_, LV_GRAD_DIR_VER, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    body_ = lv_obj_create(root_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_pos(body_, 0, 0);
    lv_obj_set_size(body_, 320, 240);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(body_, OnGesture, LV_EVENT_GESTURE, this);

    refresh_timer_ = lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<PixelTouchUI*>(lv_timer_get_user_data(timer));
        self->Refresh();
    }, 5000, this);
    lv_timer_pause(refresh_timer_);
}

void PixelTouchUI::RefreshTextFonts(const lv_font_t* text_font, const lv_font_t* muted,
                                    const lv_font_t* player_title) {
    muted_font_ = muted ? *muted : font_noto_sans_basic_14_1;
    muted_font_.fallback = text_font;
#if CONFIG_LCDWIKI_ES3C28P_LANDSCAPE
    heading_font_ = es3c28p_ui_heading;
#else
    heading_font_ = font_noto_sans_basic_16_4;
#endif
    heading_font_.fallback = text_font;
    player_title_font_ = player_title ? *player_title : font_noto_sans_basic_20_4;
    player_title_font_.fallback = text_font;

    // Digits and the 月/日 units fit inside the 19px calendar cell.
    // Preserve the glyph baseline and trim the unused descender space.
    calendar_digit_font_ = font_noto_sans_basic_16_4;
    calendar_digit_font_.fallback = text_font;
    if (calendar_digit_font_.line_height > 19) {
        calendar_digit_font_.base_line -= calendar_digit_font_.line_height - 19;
        calendar_digit_font_.line_height = 19;
    }
}

PixelTouchUI::~PixelTouchUI() {
    // LVGL queues only the raw this pointer. Cancel callbacks before deletion
    // to avoid use-after-free during screen teardown or reboot.
    lv_async_call_cancel(RunQueuedNavigation, this);
    // The robot is owned by the existing chat display, not by these pages.
    DetachVoiceMessages();
    DetachRobot();
    if (refresh_timer_ != nullptr) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
}

void PixelTouchUI::Show(lv_obj_t* robot_area, lv_obj_t* chat_container) {
    if (robot_area == nullptr || chat_container == nullptr) {
        return;
    }
    robot_area_ = robot_area;
    chat_container_ = chat_container;
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(root_, lv_obj_get_child_count(lv_obj_get_parent(root_)) - 1);
    voice_compact_ = false;
    voice_waiting_response_ = false;
    Navigate(Page::Home);
    lv_timer_resume(refresh_timer_);
}

void PixelTouchUI::ShowPlayer(lv_obj_t* robot_area, lv_obj_t* chat_container) {
    if (robot_area == nullptr || chat_container == nullptr) return;
    robot_area_ = robot_area;
    chat_container_ = chat_container;
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(root_, lv_obj_get_child_count(lv_obj_get_parent(root_)) - 1);
    Navigate(Page::Player);
    lv_timer_resume(refresh_timer_);
}

void PixelTouchUI::ShowVoice(lv_obj_t* robot_area, lv_obj_t* chat_container) {
    if (robot_area == nullptr || chat_container == nullptr) return;
    robot_area_ = robot_area;
    chat_container_ = chat_container;
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(root_, lv_obj_get_child_count(lv_obj_get_parent(root_)) - 1);
    if (page_ != Page::Voice || robot_slot_ == nullptr) {
        Navigate(Page::Voice);
    }
    RefreshVoice();
    lv_timer_resume(refresh_timer_);
}

bool PixelTouchUI::IsPlayerVisible() const {
    return page_ == Page::Player && root_ != nullptr &&
           !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

bool PixelTouchUI::IsVoiceVisible() const {
    return page_ == Page::Voice && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void PixelTouchUI::Hide() {
    if (voice_ring_ != nullptr)
        lv_anim_delete(voice_ring_, nullptr);
    DetachVoiceMessages();
    DetachRobot();
    voice_waiting_response_ = false;
    lv_timer_pause(refresh_timer_);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void PixelTouchUI::AttachRobot() {
    if (robot_area_ == nullptr || robot_slot_ == nullptr) {
        return;
    }
    lv_obj_set_parent(robot_area_, robot_slot_);
    const bool compact = page_ == Page::Voice && voice_compact_;
    set_robot_compact_(robot_context_, compact);
    lv_obj_set_size(robot_area_, compact ? 40 : 104, compact ? 40 : 124);
    lv_obj_set_pos(robot_area_, page_ == Page::Voice && !compact ? -3 : 0, 0);
    // Animate only when visible on the V1.1 pages. Style translation avoids
    // the off-screen layer allocations caused by transform_scale on ESP32.
    auto* robot = lv_obj_get_child(robot_area_, 0);
    if (robot != nullptr) {
        lv_anim_delete(robot, nullptr);
        lv_obj_set_style_translate_y(robot, 0, 0);
        if (page_ == Page::Home) {
            lv_anim_t float_anim;
            lv_anim_init(&float_anim);
            lv_anim_set_var(&float_anim, robot);
            lv_anim_set_exec_cb(&float_anim, [](void* obj, int32_t y) {
                lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), y, 0);
            });
            lv_anim_set_values(&float_anim, 0, -3);
            lv_anim_set_duration(&float_anim, 1500);
            lv_anim_set_playback_duration(&float_anim, 1500);
            lv_anim_set_repeat_count(&float_anim, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&float_anim);
        }
        if (auto* orbit = lv_obj_get_child(robot, 0)) {
            if (page_ == Page::Voice)
                lv_obj_add_flag(orbit, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_remove_flag(orbit, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // The single shared character includes a home-only caption.
    if (robot != nullptr && lv_obj_get_child_count(robot) > 0) {
        auto* caption = lv_obj_get_child(robot, lv_obj_get_child_count(robot) - 1);
        if (page_ == Page::Voice) lv_obj_add_flag(caption, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(caption, LV_OBJ_FLAG_HIDDEN);
    }
}

void PixelTouchUI::DetachRobot() {
    if (robot_area_ == nullptr || chat_container_ == nullptr) {
        return;
    }
    if (lv_obj_get_parent(robot_area_) != chat_container_) {
        set_robot_compact_(robot_context_, false);
        lv_obj_set_parent(robot_area_, chat_container_);
        lv_obj_set_size(robot_area_, 320, 100);
        auto* robot = lv_obj_get_child(robot_area_, 0);
        if (robot != nullptr) {
            lv_anim_delete(robot, nullptr);
            lv_obj_set_style_translate_y(robot, 0, 0);
            auto* orbit = lv_obj_get_child(robot, 0);
            if (orbit != nullptr) {
                lv_obj_remove_flag(orbit, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (robot != nullptr && lv_obj_get_child_count(robot) > 0) {
            lv_obj_remove_flag(lv_obj_get_child(robot, lv_obj_get_child_count(robot) - 1),
                               LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_move_to_index(robot_area_, 1);
    }
    robot_slot_ = nullptr;
}

void PixelTouchUI::Navigate(Page page) {
    DetachVoiceMessages();
    DetachRobot();
    lv_obj_clean(body_);
    page_ = page;
    voice_ring_ = nullptr;
    voice_title_ = nullptr;
    voice_caption_ = nullptr;
    voice_primary_ = nullptr;
    voice_stop_ = nullptr;
    voice_messages_ = nullptr;
    home_day_label_ = nullptr;
    home_hint_label_ = nullptr;
    home_network_icon_ = nullptr;
    home_battery_case_ = nullptr;
    home_battery_fill_ = nullptr;
    time_label_ = nullptr;
    temperature_label_ = nullptr;
    weather_label_ = nullptr;
    lunar_label_ = nullptr;
    calendar_date_label_ = nullptr;
    calendar_detail_label_ = nullptr;
    calendar_yi_row_ = nullptr;
    calendar_ji_row_ = nullptr;
    brightness_slider_ = nullptr;
    volume_slider_ = nullptr;
    player_title_ = nullptr;
    player_duration_ = nullptr;
    player_volume_popup_ = nullptr;
    player_volume_slider_ = nullptr;
    player_volume_value_ = nullptr;
    player_status_ = nullptr;
    player_position_ = nullptr;
    player_seek_slider_ = nullptr;
    player_toggle_ = nullptr;
    player_prev_button_ = nullptr;
    player_next_button_ = nullptr;
    playlist_panel_ = nullptr;
    playlist_page_label_ = nullptr;
    for (auto& button : playlist_buttons_) button = nullptr;
    calendar_month_label_ = nullptr;
    calendar_selected_label_ = nullptr;
    calendar_lunar_label_ = nullptr;
    calendar_details_popup_ = nullptr;
    for (auto& cell : calendar_cells_) cell = nullptr;
    child_mode_switch_ = nullptr;
    settings_foot_ = nullptr;
    settings_layout_detail_ = nullptr;
    refresh_ticks_ = 0;

    switch (page) {
        case Page::Home:
            BuildHome();
            AttachRobot();
            break;
        case Page::Voice:
            BuildVoice();
            UpdateVoiceLayout();
            break;
        case Page::Calendar:
            BuildCalendar();
            break;
        case Page::Player:
            BuildPlayer();
            break;
        case Page::Settings:
            BuildSettings();
            break;
    }
    if (page == Page::Home) RefreshHome();
    if (page == Page::Voice) RefreshVoice();
    if (page == Page::Calendar) RefreshCalendar();
    if (page == Page::Player) {
        UpdatePlayer();
        Application::GetInstance().SendMediaCommand("get_state");
    }
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
    LogUiGeometry();
#endif
}


#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
void PixelTouchUI::LogUiGeometry() const {
    // LVGL's actual post-layout bounds, not guessed C++ source constants.
    lv_obj_update_layout(body_);
    const char* state = "home";
    switch (page_) {
        case Page::Home: state = "home"; break;
        case Page::Voice:
            state = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking ?
                    "voice.speaking" :
                    (voice_waiting_response_ ? "voice.thinking" : "voice.listening");
            break;
        case Page::Player:
            state = player_song_mode_ ? "player.song.playing" :
                    (player_volume_popup_ != nullptr &&
                     !lv_obj_has_flag(player_volume_popup_, LV_OBJ_FLAG_HIDDEN) ?
                     "player.story.volume" :
                     (media_status_ == "paused" ? "player.story.paused" :
                     (media_status_ == "playing" ? "player.story.playing" :
                                                   "player.story.idle")));
            break;
        case Page::Calendar:
            state = calendar_details_popup_ != nullptr &&
                     !lv_obj_has_flag(calendar_details_popup_, LV_OBJ_FLAG_HIDDEN) ?
                     "calendar.detail" : "calendar.default";
            break;
        case Page::Settings:
            state = child_mode_ ? "settings.child-on" : "settings.child-off";
            break;
    }
    // Serial parser clears any earlier snapshot on this explicit boundary.
    ESP_LOGI("ES3C28P_UI", "state=%s begin=1", state);
    auto log = [state](const char* key, lv_obj_t* obj) {
        if (obj == nullptr) return;
        // A visible child inside a hidden popup is not real on-screen
        // evidence. Check ancestor visibility before logging geometry.
        for (auto* p = obj; p != nullptr; p = lv_obj_get_parent(p)) {
            if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) return;
        }
        lv_area_t a{};
        lv_obj_get_coords(obj, &a);
        ESP_LOGI("ES3C28P_UI",
                 "state=%s anchor=%s x=%ld y=%ld w=%ld h=%ld",
                 state, key, static_cast<long>(a.x1), static_cast<long>(a.y1),
                 static_cast<long>(a.x2 - a.x1 + 1),
                 static_cast<long>(a.y2 - a.y1 + 1));
    };
    log(".page.active", body_);
    if (page_ == Page::Home) {
        log(".small-date", home_day_label_);
        log(".clock", time_label_);
        log("home-lunar", lunar_label_);
        log("home-hint-text", home_hint_label_);
        log("@talk", robot_slot_);
    } else if (page_ == Page::Voice) {
        log(".header", lv_obj_get_child(body_, 0));
        log("@voice-face", robot_slot_);
        log("voice-messages", voice_messages_);
        log("voice-title", voice_title_);
        log("voice-caption", voice_caption_);
        log("voice-cancel", voice_stop_);
        log("voice-send", voice_primary_);
    } else if (page_ == Page::Player) {
        log(".header", lv_obj_get_child(body_, 0));
        log("track-title", player_title_);
        log("play-status", player_status_);
        log("progress", player_seek_slider_ ? lv_obj_get_parent(player_seek_slider_) : nullptr);
        log("current-time", player_position_);
        log("total-time", player_duration_);
        log("@prev", player_prev_button_);
        log("pause-button", player_toggle_);
        log("@next", player_next_button_);
        log("volume-popup", player_volume_popup_);
        log("volume-slider", player_volume_slider_ ? lv_obj_get_parent(player_volume_slider_) : nullptr);
    } else if (page_ == Page::Calendar) {
        log(".header", lv_obj_get_child(body_, 0));
        log("cal-month", calendar_month_label_);
        log("cal-picked", calendar_selected_label_);
        log("cal-lunar", calendar_lunar_label_);
        log("@cal-detail", calendar_selected_label_ ?
                             lv_obj_get_parent(calendar_selected_label_) : nullptr);
        log("cal-detail", calendar_details_popup_);
        log("detail-date", calendar_date_label_);
    } else if (page_ == Page::Settings) {
        log(".header", lv_obj_get_child(body_, 0));
        log("kid-toggle", child_mode_switch_);
        log("brightness", brightness_slider_ ? lv_obj_get_parent(brightness_slider_) : nullptr);
        log("setting-volume", volume_slider_ ? lv_obj_get_parent(volume_slider_) : nullptr);
        log("setting-foot", settings_foot_);
        log("settings-layout-detail", settings_layout_detail_);
    }
}
#endif

void PixelTouchUI::RunQueuedNavigation(void* data) {
    auto* self = static_cast<PixelTouchUI*>(data);
    if (self->root_ != nullptr &&
        !lv_obj_has_flag(self->root_, LV_OBJ_FLAG_HIDDEN)) {
        self->Navigate(self->queued_page_);
    }
}

void PixelTouchUI::QueueNavigate(Page page) {
    queued_page_ = page;
    // Only one deferred transition is needed after an LVGL click callback.
    // Cancel previous requests so fast taps cannot rebuild the page repeatedly.
    lv_async_call_cancel(RunQueuedNavigation, this);
    lv_async_call(RunQueuedNavigation, this);
}

void PixelTouchUI::BuildHeader(const char* title, bool back) {
    // Approved V1.1 header: 320x35, transparent bar and compact back arrow.
    auto* bar = lv_obj_create(body_);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 320, 35);
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x293b4d), 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    if (back) {
        auto* button = Button(bar, "", 12, 2, 28, 30, OnHome, this,
                              MATERIAL_SYMBOLS_ARROW_BACK);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, 0, 0);
    }
    auto* label = Label(bar, title, back ? 49 : 12, 6, back ? 220 : 290, 25);
    lv_obj_set_style_text_color(label, lv_color_hex(kText), 0);
}

void PixelTouchUI::BuildHome() {
    // V1.1 browser baseline: no legacy 32px banner; compact date/status row.
    home_day_label_ = Label(body_, "--月--日 · 星期--", 14, 10, 195, 21, kMuted, &muted_font_);
    home_network_icon_ = Label(body_, "", 226, 8, 19, 24, 0xb2c5d6, &muted_font_);
    auto* theme = static_cast<LvglTheme*>(Board::GetInstance().GetDisplay()->GetTheme());
    lv_obj_set_style_text_font(home_network_icon_, theme->icon_font()->font(), 0);
    home_battery_case_ = lv_obj_create(body_);
    lv_obj_remove_style_all(home_battery_case_);
    lv_obj_set_pos(home_battery_case_, 253, 15);
    lv_obj_set_size(home_battery_case_, 19, 10);
    lv_obj_set_style_radius(home_battery_case_, 2, 0);
    lv_obj_set_style_border_color(home_battery_case_, lv_color_hex(0xa4b6c8), 0);
    lv_obj_set_style_border_width(home_battery_case_, 1, 0);
    lv_obj_clear_flag(home_battery_case_, LV_OBJ_FLAG_CLICKABLE);
    home_battery_fill_ = lv_obj_create(home_battery_case_);
    lv_obj_remove_style_all(home_battery_fill_);
    lv_obj_set_pos(home_battery_fill_, 1, 1);
    lv_obj_set_size(home_battery_fill_, 0, 6);
    lv_obj_set_style_bg_color(home_battery_fill_, lv_color_hex(0x82e1d6), 0);
    lv_obj_set_style_bg_opa(home_battery_fill_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(home_battery_fill_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(home_battery_case_, LV_OBJ_FLAG_HIDDEN);
    auto* settings = Button(body_, "", 279, 6, 28, 28, OnSettings, this,
                            MATERIAL_SYMBOLS_SETTINGS);
    lv_obj_set_style_bg_opa(settings, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings, 0, 0);

    // Reuse the same round-face character on the home and voice pages.
    robot_slot_ = lv_obj_create(body_);
    lv_obj_remove_style_all(robot_slot_);
    lv_obj_set_pos(robot_slot_, 200, 51);
    lv_obj_set_size(robot_slot_, 104, 124);
    lv_obj_add_flag(robot_slot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(robot_slot_, OnTalk, LV_EVENT_CLICKED, this);

#if CONFIG_LCDWIKI_ES3C28P_LANDSCAPE
    time_label_ = Label(body_, "--#71dbd1 :#--", 17, 52, 192, 54, 0xf1f7fc, &es3c28p_ui_clock);
    lv_label_set_recolor(time_label_, true);
    lv_obj_set_style_text_letter_space(time_label_, -2, 0);
#else
    time_label_ = Label(body_, "--#71dbd1 :#--", 17, 52, 192, 54);
    lv_label_set_recolor(time_label_, true);
#endif
    temperature_label_ = Label(body_, "", 17, 109, 100, 31, 0xebf4fb, &player_title_font_);
    lv_obj_set_width(temperature_label_, LV_SIZE_CONTENT);
    lv_obj_add_flag(temperature_label_, LV_OBJ_FLAG_HIDDEN);
    weather_label_ = Label(body_, "天气暂无数据", 17, 113, 192, 25, 0xb7cad9);
    lv_label_set_long_mode(weather_label_, LV_LABEL_LONG_DOT);
    lunar_label_ = Label(body_, "农历暂无数据", 17, 141, 182, 20, kMuted, &muted_font_);
    lv_label_set_long_mode(lunar_label_, LV_LABEL_LONG_DOT);

    auto* indicator = lv_obj_create(body_);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_pos(indicator, 18, 170);
    lv_obj_set_size(indicator, 7, 7);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(0x70dfcb), 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    home_hint_label_ = Label(body_, "你好，我是天天", 31, 163, 165, 20, kMuted, &muted_font_);

    auto* story = Button(body_, "听故事", 13, 185, 93, 43, OnStory, this);
    lv_obj_align(lv_obj_get_child(story, 0), LV_ALIGN_LEFT_MID, 30, 0);
    DrawPrototypeBook(story, false, 8, 12, 0x78d9d4);
    auto* song = Button(body_, "听儿歌", 114, 185, 93, 43, OnSong, this,
                        MATERIAL_SYMBOLS_MUSIC_NOTE);
    auto* calendar = Button(body_, "万年历", 214, 185, 93, 43, OnCalendar, this,
                            MATERIAL_SYMBOLS_CALENDAR_MONTH);
    for (auto* action : {story, song, calendar}) {
        lv_obj_set_style_radius(action, 11, 0);
        lv_obj_set_style_bg_color(action, lv_color_hex(0x20354e), 0);
        lv_obj_set_style_border_color(action, lv_color_hex(0x2d4660), 0);
    }
}

void PixelTouchUI::BuildVoice() {
    BuildHeader("和天天聊天", true);
    // The same circular mascot is used on Home and Voice. No duplicate face.
    robot_slot_ = lv_obj_create(body_);
    lv_obj_remove_style_all(robot_slot_);
    lv_obj_set_pos(robot_slot_, 111, 47);
    lv_obj_set_size(robot_slot_, 98, 98);
    lv_obj_add_flag(robot_slot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(robot_slot_, OnVoicePrimary, LV_EVENT_CLICKED, this);

    voice_ring_ = lv_obj_create(body_);
    lv_obj_remove_style_all(voice_ring_);
    lv_obj_remove_flag(voice_ring_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(voice_ring_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(voice_ring_, 98, 98);
    lv_obj_align_to(voice_ring_, robot_slot_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_to_index(voice_ring_, lv_obj_get_index(robot_slot_));
    lv_obj_set_style_radius(voice_ring_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(voice_ring_, 2, 0);
    lv_obj_add_event_cb(
        voice_ring_,
        [](lv_event_t* event) {
            auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
            if (self->voice_ring_state_ != VoiceRing::Thinking)
                return;
            lv_area_t bounds;
            lv_obj_get_coords(self->voice_ring_, &bounds);
            lv_draw_arc_dsc_t arc;
            lv_draw_arc_dsc_init(&arc);
            arc.center = {(bounds.x1 + bounds.x2 + 1) / 2, (bounds.y1 + bounds.y2 + 1) / 2};
            arc.radius = lv_area_get_width(&bounds) / 2;
            arc.width = 2;
            arc.color = lv_color_hex(0x6ce0d6);
            arc.opa = 111;
            for (int angle = self->voice_ring_angle_; angle < self->voice_ring_angle_ + 360;
                 angle += 30) {
                arc.start_angle = angle % 360;
                arc.end_angle = (angle + 20) % 360;
                lv_draw_arc(lv_event_get_layer(event), &arc);
            }
        },
        LV_EVENT_DRAW_MAIN, this);

    auto* header = lv_obj_get_child(body_, 0);
    auto* media =
        Button(header, "", 210, 2, 28, 30, OnVoicePlayer, this, MATERIAL_SYMBOLS_MUSIC_NOTE);
    lv_obj_set_style_bg_opa(media, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(media, 0, 0);
    auto* assistant = Label(header, "AI 助手", 239, 9, 69, 18, kMuted, &muted_font_);
    lv_obj_set_style_text_align(assistant, LV_TEXT_ALIGN_RIGHT, 0);
    voice_title_ = Label(body_, "正在聆听…", 8, 150, 304, 24, kText, &heading_font_);
    lv_obj_set_style_text_align(voice_title_, LV_TEXT_ALIGN_CENTER, 0);
    voice_caption_ = Label(body_, "我在听，你想聊什么？", 20, 176, 280, 18, 0x9ab5ca, &muted_font_);
    lv_obj_set_style_text_align(voice_caption_, LV_TEXT_ALIGN_CENTER, 0);

    voice_messages_ = lv_obj_create(body_);
    lv_obj_remove_style_all(voice_messages_);
    lv_obj_set_pos(voice_messages_, 12, 87);
    lv_obj_set_size(voice_messages_, 296, 104);
    lv_obj_remove_flag(voice_messages_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(voice_messages_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(voice_messages_, LV_OBJ_FLAG_HIDDEN);

    voice_stop_ = Button(body_, "结束对话", 12, 197, 128, 33, OnVoiceStop, this);
    voice_primary_ = Button(body_, "说话中", 148, 197, 160, 33, OnVoicePrimary, this);
    lv_obj_set_style_radius(voice_stop_, 9, 0);
    lv_obj_set_style_radius(voice_primary_, 9, 0);
    lv_obj_set_style_bg_color(voice_stop_, lv_color_hex(0x1a2e44), 0);
    lv_obj_set_style_border_color(voice_stop_, lv_color_hex(0x2f4962), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(voice_stop_, 0), lv_color_hex(0xd2e1ec), 0);
    lv_obj_set_style_bg_color(voice_primary_, lv_color_hex(0x80e4d7), 0);
    lv_obj_set_style_border_color(voice_primary_, lv_color_hex(0x80e4d7), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(voice_primary_, 0), lv_color_hex(0x092b34), 0);
    RefreshVoice();
}

void PixelTouchUI::AttachVoiceMessages() {
    if (lv_obj_get_parent(chat_content_) == voice_messages_)
        return;
    // Let the original flex layout release its computed height before reparenting.
    lv_obj_set_flex_grow(chat_content_, 0);
    lv_obj_update_layout(chat_content_);
    lv_obj_set_parent(chat_content_, voice_messages_);
    lv_obj_set_pos(chat_content_, 0, 0);
    lv_obj_set_size(chat_content_, 296, 104);
    lv_obj_set_scroll_dir(chat_content_, LV_DIR_VER);
    lv_obj_set_style_pad_all(chat_content_, 1, 0);
    lv_obj_set_style_pad_right(chat_content_, 5, 0);
    lv_obj_set_style_pad_row(chat_content_, 7, 0);
    lv_obj_set_style_bg_color(chat_content_, lv_color_hex(0x42617b), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(chat_content_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(chat_content_, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(chat_content_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(chat_content_, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scrollbar_mode(chat_content_, LV_SCROLLBAR_MODE_ACTIVE);
}

void PixelTouchUI::DetachVoiceMessages() {
    if (voice_messages_ == nullptr || lv_obj_get_parent(chat_content_) != voice_messages_)
        return;
    lv_obj_set_parent(chat_content_, chat_container_);
    lv_obj_set_width(chat_content_, LV_HOR_RES);
    lv_obj_remove_local_style_prop(chat_content_, LV_STYLE_HEIGHT, 0);
    lv_obj_set_flex_grow(chat_content_, 1);
    auto* theme = static_cast<LvglTheme*>(Board::GetInstance().GetDisplay()->GetTheme());
    lv_obj_set_style_pad_all(chat_content_, theme->spacing(4), 0);
    lv_obj_set_style_pad_row(chat_content_, theme->spacing(4), 0);
    lv_obj_set_style_bg_opa(chat_content_, LV_OPA_COVER, 0);
    lv_obj_remove_local_style_prop(chat_content_, LV_STYLE_BG_COLOR, LV_PART_SCROLLBAR);
    lv_obj_remove_local_style_prop(chat_content_, LV_STYLE_BG_OPA, LV_PART_SCROLLBAR);
    lv_obj_remove_local_style_prop(chat_content_, LV_STYLE_WIDTH, LV_PART_SCROLLBAR);
    lv_obj_add_flag(chat_content_, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scrollbar_mode(chat_content_, LV_SCROLLBAR_MODE_OFF);
}

void PixelTouchUI::UpdateVoiceLayout() {
    if (voice_compact_) {
        lv_obj_set_pos(robot_slot_, 12, 42);
        lv_obj_set_size(robot_slot_, 40, 40);
        lv_obj_set_size(voice_ring_, 40, 40);
        lv_obj_align_to(voice_ring_, robot_slot_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_pos(voice_title_, 62, 42);
        lv_obj_set_size(voice_title_, 246, 40);
        lv_obj_set_style_pad_top(voice_title_, 11, 0);
        lv_obj_set_style_text_align(voice_title_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_add_flag(voice_caption_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(voice_messages_, LV_OBJ_FLAG_HIDDEN);
        AttachVoiceMessages();
        ScrollVoiceToEnd();
    }
    AttachRobot();
}

void PixelTouchUI::ScrollVoiceToEnd() {
    lv_obj_update_layout(chat_content_);
    const auto end = lv_obj_get_scroll_y(chat_content_) + lv_obj_get_scroll_bottom(chat_content_);
    lv_obj_scroll_to_y(chat_content_, std::max<int32_t>(0, end), LV_ANIM_OFF);
}

void PixelTouchUI::OnChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') return;
    const auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateListening && state != kDeviceStateSpeaking)
        return;
    const bool user = std::strcmp(role, "user") == 0;
    if (!user && std::strcmp(role, "assistant") != 0)
        return;
    voice_waiting_response_ = user;
    if (!IsVoiceVisible()) {
        voice_compact_ = true;
        return;
    }
    if (!voice_compact_) {
        voice_compact_ = true;
        UpdateVoiceLayout();
    } else {
        ScrollVoiceToEnd();
    }
    RefreshVoice();
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
    LogUiGeometry();
#endif
}

void PixelTouchUI::RefreshVoice() {
    if (voice_title_ == nullptr || voice_primary_ == nullptr) return;
    const auto state = Application::GetInstance().GetDeviceState();
    const char* title = "等待对话";
    const char* caption = "点击开始说话";
    const char* primary = "开始说话";
    bool enabled = true;
    if (state == kDeviceStateListening) {
        title = voice_waiting_response_ ? "正在思考…" : "正在聆听…";
        caption = voice_waiting_response_ ? "正在理解你的问题" : "我在听，你想聊什么？";
        primary = voice_waiting_response_ ? "处理中…" : "正在聆听";
        enabled = false;
    } else if (state == kDeviceStateSpeaking) {
        voice_waiting_response_ = false;
        title = "天天正在回答";
        caption = "点击停止播报";
        primary = "再问一次";
    } else if (state == kDeviceStateConnecting) {
        voice_waiting_response_ = false;
        title = "正在连接…";
        caption = "正在连接语音服务";
        primary = "连接中";
        enabled = false;
    }
    if (state == kDeviceStateIdle) voice_waiting_response_ = false;
    const bool thinking = state == kDeviceStateListening && voice_waiting_response_;
    const auto ring = state == kDeviceStateSpeaking
                          ? VoiceRing::Speaking
                          : (thinking ? VoiceRing::Thinking : VoiceRing::Listening);
    UpdateVoiceRing(ring);
    SetIfChanged(voice_title_, title);
    SetIfChanged(voice_caption_, caption);
    SetIfChanged(lv_obj_get_child(voice_primary_, 0), primary);
    SetIfChanged(lv_obj_get_child(voice_stop_, 0),
                 state == kDeviceStateSpeaking ? "停止播报" : "结束对话");
    if (enabled) lv_obj_remove_state(voice_primary_, LV_STATE_DISABLED);
    else lv_obj_add_state(voice_primary_, LV_STATE_DISABLED);
    if (state == kDeviceStateConnecting)
        lv_obj_add_state(voice_stop_, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(voice_stop_, LV_STATE_DISABLED);
}

void PixelTouchUI::UpdateVoiceRing(VoiceRing state) {
    if (state == voice_ring_state_ && lv_anim_get(voice_ring_, nullptr) != nullptr)
        return;
    voice_ring_state_ = state;
    lv_anim_delete(voice_ring_, nullptr);
    lv_obj_set_style_transform_width(voice_ring_, 0, 0);
    lv_obj_set_style_transform_height(voice_ring_, 0, 0);
    const bool speaking = state == VoiceRing::Speaking;
    lv_obj_set_style_text_color(voice_title_, lv_color_hex(speaking ? 0xf5d39b : kText), 0);
    lv_obj_set_style_border_color(voice_ring_, lv_color_hex(speaking ? 0xf9cf88 : 0x6ce0d6), 0);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, voice_ring_);
    lv_anim_set_user_data(&animation, this);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    if (state == VoiceRing::Thinking) {
        lv_obj_set_style_border_opa(voice_ring_, LV_OPA_TRANSP, 0);
        lv_anim_set_custom_exec_cb(&animation, [](lv_anim_t* current, int32_t angle) {
            auto* self = static_cast<PixelTouchUI*>(lv_anim_get_user_data(current));
            self->voice_ring_angle_ = angle;
            lv_obj_invalidate(self->voice_ring_);
        });
        lv_anim_set_values(&animation, 0, 360);
        lv_anim_set_duration(&animation, 3000);
    } else {
        // Expand the native border without scaling a render layer or the face.
        lv_anim_set_custom_exec_cb(&animation, [](lv_anim_t* current, int32_t progress) {
            auto* self = static_cast<PixelTouchUI*>(lv_anim_get_user_data(current));
            auto* ring = self->voice_ring_;
            const auto outset = (lv_obj_get_width(ring) * progress + 1000) / 2000;
            const int32_t peak =
                self->voice_ring_state_ == VoiceRing::Speaking ? LV_OPA_COVER : 111;
            lv_obj_set_style_transform_width(ring, outset, 0);
            lv_obj_set_style_transform_height(ring, outset, 0);
            lv_obj_set_style_border_opa(ring, peak * (70 + 30 * progress / 100) / 100, 0);
        });
        lv_anim_set_values(&animation, 0, 100);
        lv_anim_set_duration(&animation, 825);
        lv_anim_set_playback_duration(&animation, 825);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    }
    lv_anim_start(&animation);
}

void PixelTouchUI::BuildCalendar() {
    BuildHeader("万年历", true);
    auto* today_button = Button(body_, "今天", 260, 4, 48, 27, OnCalendarToday, this);
    lv_obj_set_style_radius(today_button, 6, 0);
    lv_obj_set_style_bg_color(today_button, lv_color_hex(0x263f51), 0);
    auto* previous = Button(body_, "", 13, 35, 35, 23, OnCalendarPrevious, this,
                            MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT);
    auto* next = Button(body_, "", 272, 35, 35, 23, OnCalendarNext, this,
                        MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT);
    for (auto* button : {previous, next}) {
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, 0, 0);
    }
    calendar_month_label_ = Label(body_, "", 89, 37, 142, 22, kText, &heading_font_);
    lv_obj_set_style_text_align(calendar_month_label_, LV_TEXT_ALIGN_CENTER, 0);

    static constexpr const char* week[] = {"日", "一", "二", "三", "四", "五", "六"};
    for (int col = 0; col < 7; ++col) {
        auto* label = Label(body_, week[col], 13 + col * 42, 62, 42, 16, 0xa5b9ce, &muted_font_);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    if (calendar_year_ == 0 && local.tm_year >= 125) ResetCalendarToday();
    if (calendar_year_ == 0) {
        Label(body_, "设备时间未同步", 20, 104, 278, 35, kMuted);
        return;
    }

    char month_text[32];
    std::snprintf(month_text, sizeof(month_text), "%d年%d月",
                  calendar_year_, calendar_month_);
    SetIfChanged(calendar_month_label_, month_text);

    const int first = WeekdayOfFirst(calendar_year_, calendar_month_);
    const int days = DaysInMonth(calendar_year_, calendar_month_);
    const bool six_rows = (first + days) > 35;
    const int row_height = six_rows ? 19 : 22;
    for (int day = 1; day <= days; ++day) {
        const int index = first + day - 1;
        const int col = index % 7;
        const int row = index / 7;
        auto* cell = lv_label_create(body_);
        calendar_cells_[day] = cell;
        lv_obj_set_pos(cell, 18 + col * 42, 78 + row * row_height);
        lv_obj_set_size(cell, 32, six_rows ? 19 : 21);
        lv_obj_set_style_border_post(cell, true, 0);
        lv_obj_set_style_text_font(cell, &calendar_digit_font_, 0);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(cell, lv_color_hex(kText), 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(day)));
        lv_obj_add_event_cb(cell, OnCalendarDay, LV_EVENT_CLICKED, this);
        char day_text[4];
        std::snprintf(day_text, sizeof(day_text), "%d", day);
        lv_label_set_text(cell, day_text);
        const bool selected = selected_year_ == calendar_year_ &&
            selected_month_ == calendar_month_ && selected_day_ == day;
        const bool today = local.tm_year + 1900 == calendar_year_ &&
            local.tm_mon + 1 == calendar_month_ && local.tm_mday == day;
        if (selected) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x7ddfd6), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(cell, lv_color_hex(0x0a2533), 0);
        } else if (today) {
            lv_obj_set_style_border_color(cell, lv_color_hex(0x4c8f91), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
        }
    }

    auto* footer = lv_obj_create(body_);
    lv_obj_set_pos(footer, 12, 195);
    lv_obj_set_size(footer, 296, 37);
    StylePanel(footer, 0x1a2d44, 0x2b4660);
    lv_obj_set_style_radius(footer, 9, 0);
    lv_obj_add_flag(footer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(footer, OnCalendarDetail, LV_EVENT_CLICKED, this);
    calendar_selected_label_ = Label(footer, "", 11, 0, 174, 19, kText, &calendar_digit_font_);
    calendar_lunar_label_ = Label(footer, "农历待查询", 11, 19, 174, 16, 0x94abbf, &muted_font_);
    lv_label_set_long_mode(calendar_lunar_label_, LV_LABEL_LONG_DOT);
    Label(footer, "查看黄历详情 >", 182, 8, 104, 19, 0x80dfd2, &muted_font_);

    calendar_details_popup_ = lv_obj_create(body_);
    lv_obj_set_pos(calendar_details_popup_, 9, 60);
    lv_obj_set_size(calendar_details_popup_, 302, 173);
    StylePanel(calendar_details_popup_, 0x1b314b, 0x42647a);
    lv_obj_set_style_radius(calendar_details_popup_, 12, 0);
    calendar_date_label_ =
        Label(calendar_details_popup_, "", 16, 12, 238, 22, kText, &heading_font_);
    auto* close = Button(calendar_details_popup_, "", 258, 11, 26, 24, OnCalendarClose, this,
                         MATERIAL_SYMBOLS_CLOSE);
    lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(close, 0, 0);

    auto* content = lv_obj_create(calendar_details_popup_);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 16, 39);
    lv_obj_set_size(content, 268, 121);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_style_pad_right(content, 3, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ACTIVE);

    calendar_detail_label_ = Label(content, "", 0, 0, 265, 18, 0x93adbc, &muted_font_);
    lv_label_set_long_mode(calendar_detail_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(calendar_detail_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(calendar_detail_label_, 19, 0);
    auto make_row = [&](const char* text, uint32_t bg, uint32_t color) {
        auto* row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        auto* badge = Label(row, text, 0, 0, 20, 20, color, &muted_font_);
        lv_obj_set_style_bg_color(badge, lv_color_hex(bg), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 6, 0);
        lv_obj_set_style_pad_top(badge, 2, 0);
        lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
        auto* value = Label(row, "", 32, 0, 233, 25, 0xebf4fb);
        lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(value, LV_SIZE_CONTENT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        return row;
    };
    calendar_yi_row_ = make_row("宜", 0x2c6b5f, 0xb0f2d4);
    calendar_ji_row_ = make_row("忌", 0x673d50, 0xffd0dc);
    lv_obj_add_flag(calendar_details_popup_, LV_OBJ_FLAG_HIDDEN);
    RefreshCalendar();
}

void PixelTouchUI::BuildPlayer() {
    BuildHeader(player_song_mode_ ? "儿歌播放" : "故事播放", true);
    auto* volume_button = Button(body_, "", 280, 2, 28, 30,
                                 OnVolumePopup, this, MATERIAL_SYMBOLS_VOLUME_UP);
    lv_obj_set_style_border_width(volume_button, 0, 0);
    lv_obj_set_style_bg_opa(volume_button, LV_OPA_TRANSP, 0);

    // V1.1 player cover 77x77 at (21,48).
    auto* cover = lv_obj_create(body_);
    lv_obj_set_pos(cover, 21, 48);
    lv_obj_set_size(cover, 77, 77);
    StylePanel(cover, player_song_mode_ ? 0xbd8dba : 0x719b8c, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 14, 0);
    lv_obj_set_style_bg_grad_color(cover, lv_color_hex(player_song_mode_ ? 0x393d75 : 0x1b4359), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
#if LV_USE_DRAW_SW_COMPLEX_GRADIENTS && LV_GRADIENT_MAX_STOPS >= 3
    const auto make_gradient = [](uint32_t highlight, uint32_t middle, uint32_t edge, int x,
                                  int y) {
        lv_grad_dsc_t gradient{};
        const lv_color_t colors[] = {lv_color_hex(highlight), lv_color_hex(middle),
                                     lv_color_hex(edge)};
        const uint8_t stops[] = {0, 140, 255};
        lv_grad_init_stops(&gradient, colors, nullptr, stops, 3);
        lv_grad_radial_init(&gradient, x, y, 76, 76, LV_GRAD_EXTEND_PAD);
        return gradient;
    };
    static const auto story_gradient = make_gradient(0x719b8c, 0x355f65, 0x1b4359, 20, 9);
    static const auto song_gradient = make_gradient(0xbd8dba, 0x765d9b, 0x393d75, 19, 12);
    lv_obj_set_style_bg_grad(cover, player_song_mode_ ? &song_gradient : &story_gradient, 0);
#endif
    lv_obj_set_style_shadow_width(cover, 12, 0);
    lv_obj_set_style_shadow_offset_y(cover, 8, 0);
    lv_obj_set_style_shadow_color(cover, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(cover, 68, 0);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    // Retained legacy queue opens by long-press on the cover.
    lv_obj_add_event_cb(cover, OnPlaylistOpen, LV_EVENT_LONG_PRESSED, this);
    if (player_song_mode_) {
        auto* cover_icon = lv_label_create(cover);
        lv_label_set_text(cover_icon, MATERIAL_SYMBOLS_MUSIC_NOTE);
        auto* theme = static_cast<LvglTheme*>(Board::GetInstance().GetDisplay()->GetTheme());
        lv_obj_set_style_text_font(cover_icon, theme->icon_font()->font(), 0);
        lv_obj_set_style_text_color(cover_icon, lv_color_hex(0xffe2f0), 0);
        lv_obj_center(cover_icon);
    } else {
        DrawPrototypeBook(cover, true, 19, 19, 0xe2f2d6);
    }

    player_status_ = Label(body_, "等待播放", 127, 52, 168, 18, 0x83cfc3, &muted_font_);
    player_title_ = Label(body_, "暂无播放内容", 115, 73, 175, 31, kText, &player_title_font_);
    lv_label_set_long_mode(player_title_, LV_LABEL_LONG_DOT);
    Label(body_, "Jellyfin", 115, 105, 164, 18, 0x89a1b8, &muted_font_);

    player_seek_slider_ = CompactSlider(body_, 22, 132, 276);
    lv_slider_set_range(player_seek_slider_, 0, 1000);
    lv_obj_add_state(player_seek_slider_, LV_STATE_DISABLED);
    lv_obj_add_event_cb(player_seek_slider_, OnMediaSeek, LV_EVENT_RELEASED, this);

    player_position_ = Label(body_, "--:--", 22, 157, 65, 18, 0x8fa7bd, &muted_font_);
    player_duration_ = Label(body_, "--:--", 235, 157, 63, 18, 0x8fa7bd, &muted_font_);
    lv_obj_set_style_text_align(player_duration_, LV_TEXT_ALIGN_RIGHT, 0);

    player_prev_button_ =
        Button(body_, "", 65, 185, 41, 41, OnMediaPrev, this, MATERIAL_SYMBOLS_SKIP_PREVIOUS);
    player_toggle_ =
        Button(body_, "", 136, 181, 49, 49, OnMediaToggle, this, MATERIAL_SYMBOLS_PLAY_ARROW);
    lv_obj_add_event_cb(player_toggle_, OnMediaPressed, LV_EVENT_PRESSED, this);
    player_next_button_ =
        Button(body_, "", 214, 185, 41, 41, OnMediaNext, this, MATERIAL_SYMBOLS_SKIP_NEXT);
    for (auto* button : {player_prev_button_, player_toggle_, player_next_button_}) {
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_bg_color(player_toggle_, lv_color_hex(0x81e4d8), 0);
    lv_obj_set_style_bg_opa(player_toggle_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(player_toggle_, 1),
                                lv_color_hex(0x0a3041), 0);
    if (!child_mode_) {
        // The old stop capability remains available without adding
        // an extra visible button that violates V1.1.
        lv_obj_add_event_cb(player_toggle_, OnMediaStop, LV_EVENT_LONG_PRESSED, this);
    }

    player_volume_popup_ = lv_obj_create(body_);
    lv_obj_set_pos(player_volume_popup_, 91, 37);
    lv_obj_set_size(player_volume_popup_, 220, 47);
    StylePanel(player_volume_popup_, 0x20364d, 0x35556b);
    lv_obj_set_style_radius(player_volume_popup_, 10, 0);
    Label(player_volume_popup_, "音量", 10, 13, 35, 18, kText, &muted_font_);
    player_volume_slider_ = CompactSlider(player_volume_popup_, 41, 13, 110);
    lv_slider_set_range(player_volume_slider_, 0, 100);
    lv_obj_add_event_cb(player_volume_slider_, OnSliderChange, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(player_volume_slider_, OnSliderRelease, LV_EVENT_RELEASED, this);
    int initial_volume = 50;
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        initial_volume = codec->output_volume();
    }
    lv_slider_set_value(player_volume_slider_, initial_volume, LV_ANIM_OFF);
    player_volume_value_ = Label(player_volume_popup_, "", 161, 13, 48, 19, kText, &muted_font_);
    char percent[8];
    std::snprintf(percent, sizeof(percent), "%d%%", initial_volume);
    SetIfChanged(player_volume_value_, percent);
    lv_obj_add_flag(player_volume_popup_, LV_OBJ_FLAG_HIDDEN);

    // Preserve real queue/selection control from the tested Go server.
    playlist_panel_ = lv_obj_create(body_);
    lv_obj_set_pos(playlist_panel_, 10, 35);
    lv_obj_set_size(playlist_panel_, 300, 201);
    StylePanel(playlist_panel_, 0x1b314b, 0x42647a);
    lv_obj_set_style_radius(playlist_panel_, 12, 0);
    Label(playlist_panel_, "播放列表", 10, 4, 230, 25, kAccent);
    Button(playlist_panel_, "", 251, 1, 44, 29, OnPlaylistClose, this,
           MATERIAL_SYMBOLS_CLOSE);
    for (int i = 0; i < 6; ++i) {
        playlist_buttons_[i] = Button(playlist_panel_, "", 8, 29 + i * 23,
                                      284, 23, OnPlaylistSelect, this);
        lv_obj_set_style_text_font(lv_obj_get_child(playlist_buttons_[i], 0), &muted_font_, 0);
        lv_obj_add_flag(playlist_buttons_[i], LV_OBJ_FLAG_HIDDEN);
    }
    Button(playlist_panel_, "上页", 9, 170, 81, 27, OnPlaylistPrevious, this);
    playlist_page_label_ = Label(playlist_panel_, "", 108, 173, 91, 24, kMuted, &muted_font_);
    Button(playlist_panel_, "下页", 210, 170, 81, 27, OnPlaylistNext, this);
    lv_obj_add_flag(playlist_panel_, LV_OBJ_FLAG_HIDDEN);
}

void PixelTouchUI::SetMediaState(const char* state, const char* title,
                                  int64_t position_ms, int64_t duration_ms,
                                  int current_index, int playlist_length, const char* error) {
    // Clear old-session data before storing the first state of a new session.
    SyncMediaAvailability();
    if (!media_available_)
        return;
    media_status_ = state != nullptr ? state : "";
    media_title_ = title != nullptr ? title : "";
    media_error_ = error != nullptr ? error : "";
    media_position_ms_ = position_ms >= 0 ? position_ms : 0;
    media_duration_ms_ = duration_ms > 0 ? duration_ms : 0;
    media_current_index_ = current_index;
    media_playlist_length_ = playlist_length;
    if (page_ == Page::Player && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
        UpdatePlayer();
        UpdatePlaylist();
    }
}

void PixelTouchUI::SetMediaPlaylist(int offset, const std::vector<std::string>& titles) {
    SyncMediaAvailability();
    // Ignore stale responses after a rapid page change or closing the overlay.
    if (page_ != Page::Player || playlist_panel_ == nullptr ||
        lv_obj_has_flag(playlist_panel_, LV_OBJ_FLAG_HIDDEN) ||
        offset != playlist_offset_) return;
    playlist_count_ = std::min(static_cast<int>(titles.size()), 6);
    for (int i = 0; i < 6; ++i) {
        playlist_titles_[i] = i < playlist_count_ ? titles[i] : "";
    }
    UpdatePlaylist();
}

void PixelTouchUI::UpdatePlaylist() {
    if (playlist_panel_ == nullptr) return;
    char label[56];
    std::snprintf(label, sizeof(label), "%d-%d / %d",
                  playlist_count_ == 0 ? 0 : playlist_offset_ + 1,
                  playlist_offset_ + playlist_count_,
                  media_playlist_length_);
    SetIfChanged(playlist_page_label_, label);
    for (int i = 0; i < 6; ++i) {
        auto* button = playlist_buttons_[i];
        if (button == nullptr) continue;
        if (i >= playlist_count_) {
            lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
        // LVGL wraps/truncates by glyph, keeping UTF-8 intact.
        std::string caption = std::to_string(playlist_offset_ + i + 1) +
                              ". " + playlist_titles_[i];
        auto* label = lv_obj_get_child(button, 0);
        lv_obj_set_size(label, 266, 23);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        SetIfChanged(label, caption.c_str());
        lv_obj_set_user_data(button,
            reinterpret_cast<void*>(static_cast<intptr_t>(playlist_offset_ + i + 1)));
        const bool selected = media_current_index_ == playlist_offset_ + i;
        lv_obj_set_style_border_color(button,
            lv_color_hex(selected ? kAccent : kBorder), 0);
    }
}

void PixelTouchUI::SyncMediaAvailability() {
    auto& app = Application::GetInstance();
    const bool supported = app.IsMediaControlAvailable();
    const uint32_t generation = app.GetMediaControlGeneration();
    if (supported != media_available_ || (supported && generation != media_generation_)) {
        // Do not reuse playback/queue data across a reconnect or downgrade.
        media_available_ = supported;
        media_status_.clear();
        media_title_.clear();
        media_error_.clear();
        media_position_ms_ = 0;
        media_duration_ms_ = 0;
        media_current_index_ = -1;
        media_playlist_length_ = 0;
        playlist_count_ = 0;
        playlist_offset_ = 0;
        if (playlist_panel_ != nullptr) {
            lv_obj_add_flag(playlist_panel_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    media_generation_ = generation;
}

void PixelTouchUI::UpdatePlayer() {
    if (player_title_ == nullptr) return;
    SyncMediaAvailability();
    const auto connection = Application::GetInstance().GetMediaControlState();
    const bool supported = connection == MediaControlState::kAvailable;
    for (auto* control : {player_prev_button_, player_toggle_, player_next_button_}) {
        if (control == nullptr) continue;
        if (supported) lv_obj_remove_state(control, LV_STATE_DISABLED);
        else lv_obj_add_state(control, LV_STATE_DISABLED);
    }
    if (!supported) {
        SetIfChanged(player_title_, "请先语音播放");
        SetIfChanged(player_status_, connection == MediaControlState::kDisconnected
                                       ? "服务未连接"
                                       : "当前服务不支持触摸控制");
        SetIfChanged(player_position_, "--:--");
        SetIfChanged(player_duration_, "--:--");
        if (player_seek_slider_ != nullptr) {
            lv_obj_add_state(player_seek_slider_, LV_STATE_DISABLED);
            lv_slider_set_value(player_seek_slider_, 0, LV_ANIM_OFF);
        }
        return;
    }
    SetIfChanged(player_title_, media_title_.empty() ? "暂无播放内容" : media_title_.c_str());
    const char* status = "等待播放";
    if (media_status_ == "playing") status = "正在播放";
    else if (media_status_ == "paused") status = "已暂停";
    else if (media_status_ == "stopped") status = "已停止";
    else if (media_status_ == "error") status = "播放发生错误";
    SetIfChanged(player_status_, media_error_.empty() ? status : media_error_.c_str());
    const int64_t seconds = media_position_ms_ / 1000;
    char current[32];
    std::snprintf(current, sizeof(current), "%02lld:%02lld",
                  static_cast<long long>(seconds / 60),
                  static_cast<long long>(seconds % 60));
    SetIfChanged(player_position_, current);
    if (media_duration_ms_ > 0) {
        const int64_t total = media_duration_ms_ / 1000;
        char duration[32];
        std::snprintf(duration, sizeof(duration), "%02lld:%02lld",
                      static_cast<long long>(total / 60),
                      static_cast<long long>(total % 60));
        SetIfChanged(player_duration_, duration);
        if (player_seek_slider_ != nullptr) {
            if (media_duration_ms_ <= 2LL * 60 * 60 * 1000)
                lv_obj_remove_state(player_seek_slider_, LV_STATE_DISABLED);
            else
                lv_obj_add_state(player_seek_slider_, LV_STATE_DISABLED);
            if (!lv_obj_has_state(player_seek_slider_, LV_STATE_PRESSED)) {
                const int ratio = static_cast<int>(std::min<int64_t>(
                    1000, media_position_ms_ * 1000 / media_duration_ms_));
                lv_slider_set_value(player_seek_slider_, ratio, LV_ANIM_OFF);
            }
        }
    } else {
        SetIfChanged(player_duration_, "--:--");
        if (player_seek_slider_ != nullptr) {
            lv_obj_add_state(player_seek_slider_, LV_STATE_DISABLED);
            lv_slider_set_value(player_seek_slider_, 0, LV_ANIM_OFF);
        }
    }
    if (player_toggle_ != nullptr) {
        // Button child 0 is text, child 1 is the approved icon glyph.
        SetIfChanged(lv_obj_get_child(player_toggle_, 1),
                     media_status_ == "playing" ?
                         MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW);
    }
}

void PixelTouchUI::BuildSettings() {
    BuildHeader("家长设置", true);
    Label(body_, "本机", 273, 9, 32, 18, 0x829fb7, &muted_font_);

    // V1.1 rows: x=14..306, top=39, each 43px high; no enclosing card.
    for (int i = 1; i <= 4; ++i) {
        auto* divider = lv_obj_create(body_);
        lv_obj_remove_style_all(divider);
        lv_obj_set_pos(divider, 14, 39 + i * 43 - 1);
        lv_obj_set_size(divider, 292, 1);
        lv_obj_set_style_bg_color(divider, lv_color_hex(0x22384e), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);
    }
    Label(body_, "儿童模式", 14, 38, 160, 25);
    Label(body_, "减少误触和复杂操作", 14, 63, 190, 18, 0x829eb5, &muted_font_);
    child_mode_switch_ = lv_switch_create(body_);
    lv_obj_set_pos(child_mode_switch_, 271, 51);
    lv_obj_set_size(child_mode_switch_, 32, 19);
    lv_obj_set_style_radius(child_mode_switch_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(child_mode_switch_, lv_color_hex(0x34445a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(child_mode_switch_, lv_color_hex(0x4caea9), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(child_mode_switch_, lv_color_hex(0xffffff), LV_PART_KNOB);
    if (child_mode_) lv_obj_add_state(child_mode_switch_, LV_STATE_CHECKED);
    lv_obj_add_event_cb(child_mode_switch_, OnChildModeChanged, LV_EVENT_VALUE_CHANGED, this);

    Label(body_, "屏幕亮度", 14, 91, 150, 25);
    Label(body_, "默认音量", 14, 134, 150, 25);
    brightness_slider_ = CompactSlider(body_, 184, 92, 120);
    lv_slider_set_range(brightness_slider_, 10, 100);
    if (auto* backlight = Board::GetInstance().GetBacklight()) {
        lv_slider_set_value(brightness_slider_, backlight->brightness(), LV_ANIM_OFF);
    }
    volume_slider_ = CompactSlider(body_, 184, 135, 120);
    lv_slider_set_range(volume_slider_, 0, 100);
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        lv_slider_set_value(volume_slider_, codec->output_volume(), LV_ANIM_OFF);
    }
    for (auto* slider : {brightness_slider_, volume_slider_}) {
        lv_obj_add_event_cb(slider, OnSliderChange, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(slider, OnSliderRelease, LV_EVENT_RELEASED, this);
    }
    Label(body_, "当前布局", 14, 167, 120, 25);
    settings_layout_detail_ =
        Label(body_, "320 × 240 · 横屏", 14, 192, 164, 18, 0x829eb5, &muted_font_);
    Label(body_, "V1.1", 271, 182, 36, 18, 0x8bd7c7, &muted_font_);
    settings_foot_ = Label(body_, child_mode_ ? "儿童模式已开启" : "儿童模式已关闭", 15, 215, 290,
                           18, 0x829eb5, &muted_font_);
}

void PixelTouchUI::RefreshHome() {
    if (home_network_icon_ != nullptr) {
        const char* symbol = Board::GetInstance().GetNetworkStateIcon();
        SetIfChanged(home_network_icon_, symbol != nullptr ? symbol : "");
    }
    if (home_battery_case_ != nullptr && home_battery_fill_ != nullptr) {
        int level = 0;
        bool charging = false, discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            level = std::max(0, std::min(100, level));
            lv_obj_set_width(home_battery_fill_, 16 * level / 100);
            lv_obj_remove_flag(home_battery_case_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(home_battery_case_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char output[64];
    if (local.tm_year >= 125) {
        static const char* weekday[] = {"星期日", "星期一", "星期二", "星期三",
                                        "星期四", "星期五", "星期六"};
        std::snprintf(output, sizeof(output), "%d月%d日 · %s",
                      local.tm_mon + 1, local.tm_mday, weekday[local.tm_wday]);
        SetIfChanged(home_day_label_, output);
        std::snprintf(output, sizeof(output), "%02d#71dbd1 :#%02d", local.tm_hour, local.tm_min);
        SetIfChanged(time_label_, output);
    }
#if CONFIG_WEATHER_DASHBOARD
    const auto weather = WeatherService::GetInstance().GetSnapshot();
    if (weather.valid) {
        std::snprintf(output, sizeof(output), "%d°C", weather.temperature);
        SetIfChanged(temperature_label_, output);
        lv_obj_remove_flag(temperature_label_, LV_OBJ_FLAG_HIDDEN);
        const auto* font = lv_obj_get_style_text_font(temperature_label_, LV_PART_MAIN);
        lv_point_t text_size;
        lv_text_get_size(&text_size, output, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const auto offset = std::min<int32_t>(text_size.x + 6, 191);
        lv_obj_set_x(weather_label_, 17 + offset);
        lv_obj_set_width(weather_label_, 192 - offset);
        std::string description = weather.city;
        if (!description.empty() && !weather.weather_text.empty())
            description += " · ";
        description += weather.weather_text;
        SetIfChanged(weather_label_, description.c_str());
    } else {
        lv_obj_add_flag(temperature_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(weather_label_, 17);
        lv_obj_set_width(weather_label_, 192);
        SetIfChanged(weather_label_, weather.no_key ? "天气未配置" : "天气暂无数据");
    }
    const auto almanac = AlmanacService::GetInstance().GetSnapshot();
    if (almanac.valid) {
        std::string lunar = "农历 " + almanac.lunar_date;
        if (!almanac.solar_term.empty()) lunar += " · " + almanac.solar_term;
        SetIfChanged(lunar_label_, lunar.c_str());
    } else {
        SetIfChanged(lunar_label_, "农历暂无数据");
    }
#endif
}

void PixelTouchUI::RefreshCalendar() {
    if (calendar_year_ == 0) return;
    char date[64];
    std::snprintf(date, sizeof(date), "%d年%d月%d日",
                  selected_year_, selected_month_, selected_day_);
    SetIfChanged(calendar_date_label_, date);
    std::snprintf(date, sizeof(date), "%d月%d日", selected_month_, selected_day_);
    SetIfChanged(calendar_selected_label_, date);
#if CONFIG_WEATHER_DASHBOARD
    AlmanacSnapshot info;
    if (IsSelectedToday()) {
        info = AlmanacService::GetInstance().GetSnapshot();
    } else {
        if (calendar_lookup_unavailable_) {
            UpdateCalendarDetails("黄历服务不可用或所选日期不支持");
            return;
        }
        const auto lookup = AlmanacService::GetInstance().GetDateResult();
        if (lookup.year != selected_year_ || lookup.month != selected_month_ ||
            lookup.day != selected_day_) {
            UpdateCalendarDetails("点击详情查询选中日期");
            return;
        }
        if (lookup.loading) {
            UpdateCalendarDetails("正在查询选中日期…");
            return;
        }
        if (!lookup.ready) {
            UpdateCalendarDetails(lookup.quota_exceeded        ? "日历 API 今日配额已用完"
                                  : lookup.credentials_invalid ? "万年历密钥无效"
                                                               : "查询失败，请检查网络和密钥");
            return;
        }
        info = lookup.snapshot;
    }
    if (!info.valid) {
        UpdateCalendarDetails(info.no_credentials ? "请先配置万年历密钥" : "今日黄历暂无数据");
        return;
    }
    const std::string lunar = "农历 " + info.lunar_date;
    SetIfChanged(calendar_lunar_label_, lunar.c_str());
    std::string summary = lunar;
    if (!info.solar_term.empty())
        summary += " · " + info.solar_term;
    const auto join = [](const std::vector<std::string>& entries) {
        std::string result;
        for (const auto& entry : entries) {
            if (entry.empty())
                continue;
            if (!result.empty())
                result += " · ";
            result += entry;
        }
        return result.empty() ? std::string("暂无数据") : result;
    };
    const auto yi = join(info.yi);
    const auto ji = join(info.ji);
    UpdateCalendarDetails(summary.c_str(), yi.c_str(), ji.c_str());
#else
    UpdateCalendarDetails("当前固件未启用黄历服务");
#endif
}

void PixelTouchUI::UpdateCalendarDetails(const char* lunar, const char* yi, const char* ji) {
    SetIfChanged(calendar_detail_label_, lunar);
    if (yi == nullptr || ji == nullptr) {
        SetIfChanged(calendar_lunar_label_, lunar);
        lv_obj_add_flag(calendar_yi_row_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(calendar_ji_row_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    SetIfChanged(lv_obj_get_child(calendar_yi_row_, 1), yi);
    SetIfChanged(lv_obj_get_child(calendar_ji_row_, 1), ji);
    lv_obj_remove_flag(calendar_yi_row_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(calendar_ji_row_, LV_OBJ_FLAG_HIDDEN);
}

void PixelTouchUI::UpdateCalendarSelection() {
    // Day selection updates two label styles instead of deleting and
    // rebuilding 31 LVGL objects (avoids heap churn on repeated touches).
    for (int day = 1; day <= 31; ++day) {
        auto* cell = calendar_cells_[day];
        if (cell == nullptr) continue;
        const bool picked = selected_year_ == calendar_year_ &&
                            selected_month_ == calendar_month_ &&
                            selected_day_ == day;
        lv_obj_set_style_bg_opa(cell, picked ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(cell, lv_color_hex(picked ? kBg : kText), 0);
        if (picked) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(kAccent), 0);
        }
    }
}

void PixelTouchUI::ResetCalendarToday() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    if (local.tm_year < 125) return;
    calendar_year_ = selected_year_ = local.tm_year + 1900;
    calendar_month_ = selected_month_ = local.tm_mon + 1;
    selected_day_ = local.tm_mday;
    calendar_lookup_unavailable_ = false;
}

bool PixelTouchUI::IsSelectedToday() const {
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return selected_year_ == local.tm_year + 1900 &&
           selected_month_ == local.tm_mon + 1 &&
           selected_day_ == local.tm_mday;
}

void PixelTouchUI::NavigateCalendarMonth(int delta) {
    int year = calendar_year_;
    int month = calendar_month_ + delta;
    if (month < 1) { month = 12; --year; }
    if (month > 12) { month = 1; ++year; }
    if (year < 2025 || year > 2099) return;
    // Match the approved HTML: paging changes the viewed month, not the
    // selected date shown in the footer until the user taps another day.
    calendar_year_ = year;
    calendar_month_ = month;
    calendar_lookup_unavailable_ = false;
    QueueNavigate(Page::Calendar);
}

void PixelTouchUI::Refresh() {
    if (lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) return;
    // If the user opened calendar before SNTP, rebuild the month once
    // the clock is ready. A failed first read must not leave an empty page.
    if (page_ == Page::Calendar && calendar_year_ == 0) {
        ResetCalendarToday();
        if (calendar_year_ != 0) {
            Navigate(Page::Calendar);
        }
        return;
    }
    if (page_ == Page::Voice) {
        RefreshVoice();
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
        LogUiGeometry();
#endif
        return;
    }
    if (page_ == Page::Player) {
        UpdatePlayer();  // Recheck support on reconnect or server downgrade.
        // Only the new Go server explicitly negotiates this command.
        if (Application::GetInstance().IsMediaControlAvailable()) {
            Application::GetInstance().SendMediaCommand("get_state");
        }
    } else if (page_ == Page::Calendar && calendar_details_popup_ != nullptr &&
               !lv_obj_has_flag(calendar_details_popup_, LV_OBJ_FLAG_HIDDEN)) {
        RefreshCalendar(); // Read the one-slot asynchronous date lookup result.
    } else if (++refresh_ticks_ >= 6) {
        refresh_ticks_ = 0;
        if (page_ == Page::Home) RefreshHome();
        if (page_ == Page::Calendar) RefreshCalendar();
    }
}

void PixelTouchUI::UpdateSliderValues() {
    if (player_volume_slider_ != nullptr) {
        char value[8];
        std::snprintf(value, sizeof(value), "%d%%",
                      static_cast<int>(lv_slider_get_value(player_volume_slider_)));
        SetIfChanged(player_volume_value_, value);
    }
}

void PixelTouchUI::OnGesture(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    auto* indev = lv_indev_active();
    if (indev == nullptr) return;
    const auto direction = lv_indev_get_gesture_dir(indev);
    if (self->page_ == Page::Home && direction == LV_DIR_LEFT) {
        self->QueueNavigate(Page::Calendar);
    } else if (self->page_ == Page::Calendar && direction == LV_DIR_RIGHT) {
        if (self->calendar_details_popup_ != nullptr &&
            !lv_obj_has_flag(self->calendar_details_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(self->calendar_details_popup_, LV_OBJ_FLAG_HIDDEN);
        } else {
            self->QueueNavigate(Page::Home);
        }
    }
}

void PixelTouchUI::OnSettings(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->QueueNavigate(Page::Settings);
}
void PixelTouchUI::OnHome(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->QueueNavigate(Page::Home);
}
void PixelTouchUI::OnCalendar(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->QueueNavigate(Page::Calendar);
}
void PixelTouchUI::OnStory(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    self->player_song_mode_ = false;
    self->QueueNavigate(Page::Player);
}
void PixelTouchUI::OnSong(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    self->player_song_mode_ = true;
    self->QueueNavigate(Page::Player);
}
void PixelTouchUI::OnVolumePopup(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    if (self->player_volume_popup_ == nullptr) return;
    if (lv_obj_has_flag(self->player_volume_popup_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(self->player_volume_popup_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(self->player_volume_popup_,
                             lv_obj_get_child_count(self->body_) - 1);
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
        self->LogUiGeometry();
#endif
    } else {
        lv_obj_add_flag(self->player_volume_popup_, LV_OBJ_FLAG_HIDDEN);
    }
}
void PixelTouchUI::OnTalk(lv_event_t*) {
    Application::GetInstance().ToggleChatState();
}
void PixelTouchUI::OnVoiceStop(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    const auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        Application::GetInstance().ToggleChatState();
    } else if (state == kDeviceStateIdle) {
        self->QueueNavigate(Page::Home);
    }
}
void PixelTouchUI::OnVoicePlayer(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->QueueNavigate(Page::Player);
}
void PixelTouchUI::OnVoicePrimary(lv_event_t*) {
    const auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        Application::GetInstance().StartListening();
    } else if (state == kDeviceStateIdle) {
        Application::GetInstance().ToggleChatState();
    }
}
void PixelTouchUI::OnSliderChange(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->UpdateSliderValues();
}
void PixelTouchUI::OnSliderRelease(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    auto* target = lv_event_get_target_obj(event);
    if (target == self->brightness_slider_) {
        if (auto* backlight = Board::GetInstance().GetBacklight()) {
            backlight->SetBrightness(lv_slider_get_value(self->brightness_slider_), true);
        }
    } else if (target == self->volume_slider_ ||
               target == self->player_volume_slider_) {
        if (auto* codec = Board::GetInstance().GetAudioCodec()) {
            codec->SetOutputVolume(lv_slider_get_value(target));
        }
    }
}

void PixelTouchUI::OnMediaPrev(lv_event_t*) {
    Application::GetInstance().SendMediaCommand("prev");
}
void PixelTouchUI::OnMediaNext(lv_event_t*) {
    Application::GetInstance().SendMediaCommand("next");
}
void PixelTouchUI::OnMediaPressed(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->player_stop_long_press_ = false;
}
void PixelTouchUI::OnMediaStop(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    self->player_stop_long_press_ = true;
    Application::GetInstance().SendMediaCommand("stop");
}
void PixelTouchUI::OnMediaToggle(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    // LVGL may emit CLICKED after LONG_PRESSED: never follow stop with resume.
    if (self->player_stop_long_press_) return;
    Application::GetInstance().SendMediaCommand(
        self->media_status_ == "playing" ? "pause" : "resume");
}

void PixelTouchUI::OnCalendarPrevious(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->NavigateCalendarMonth(-1);
}
void PixelTouchUI::OnCalendarNext(lv_event_t* event) {
    static_cast<PixelTouchUI*>(lv_event_get_user_data(event))->NavigateCalendarMonth(1);
}
void PixelTouchUI::OnCalendarToday(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    self->ResetCalendarToday();
    self->QueueNavigate(Page::Calendar);
}
void PixelTouchUI::OnCalendarDay(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    auto* obj = lv_event_get_target_obj(event);
    int day = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
    if (day < 1 || day > DaysInMonth(self->calendar_year_, self->calendar_month_)) return;
    self->selected_year_ = self->calendar_year_;
    self->selected_month_ = self->calendar_month_;
    self->selected_day_ = day;
    self->calendar_lookup_unavailable_ = false;
    self->UpdateCalendarSelection();
    self->RefreshCalendar();
}
void PixelTouchUI::OnCalendarDetail(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
#if CONFIG_WEATHER_DASHBOARD
    if (!self->IsSelectedToday()) {
        self->calendar_lookup_unavailable_ =
            !AlmanacService::GetInstance().RequestDate(
                self->selected_year_, self->selected_month_, self->selected_day_);
        self->RefreshCalendar();
    } else {
        self->RefreshCalendar();
    }
#else
    self->RefreshCalendar();
#endif
    if (self->calendar_details_popup_ != nullptr) {
        lv_obj_clear_flag(self->calendar_details_popup_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(lv_obj_get_parent(self->calendar_detail_label_), 0, LV_ANIM_OFF);
        lv_obj_move_to_index(self->calendar_details_popup_,
            lv_obj_get_child_count(self->body_) - 1);
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
        self->LogUiGeometry();
#endif
    }
}
void PixelTouchUI::OnCalendarClose(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    if (self->calendar_details_popup_ != nullptr) {
        lv_obj_add_flag(self->calendar_details_popup_, LV_OBJ_FLAG_HIDDEN);
    }
}

void PixelTouchUI::OnChildModeChanged(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    self->child_mode_ = lv_obj_has_state(self->child_mode_switch_, LV_STATE_CHECKED);
    Settings settings("pixel_ui", true);
    settings.SetBool("child_mode", self->child_mode_);
    SetIfChanged(self->settings_foot_,
                 self->child_mode_ ? "儿童模式已开启" : "儿童模式已关闭");
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
    self->LogUiGeometry();
#endif
}

void PixelTouchUI::OnPlaylistOpen(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    if (self->playlist_panel_ == nullptr) return;
    self->playlist_offset_ = 0;
    self->playlist_count_ = 0;
    self->UpdatePlaylist();
    lv_obj_clear_flag(self->playlist_panel_, LV_OBJ_FLAG_HIDDEN);
    Application::GetInstance().SendMediaCommand("get_playlist", 0);
}
void PixelTouchUI::OnPlaylistClose(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    if (self->playlist_panel_ != nullptr) {
        lv_obj_add_flag(self->playlist_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}
void PixelTouchUI::OnPlaylistPrevious(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    if (self->playlist_offset_ < 6) return;
    self->playlist_offset_ -= 6;
    self->playlist_count_ = 0;
    self->UpdatePlaylist();
    Application::GetInstance().SendMediaCommand("get_playlist", self->playlist_offset_);
}
void PixelTouchUI::OnPlaylistNext(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    if (self->playlist_offset_ + 6 >= self->media_playlist_length_) return;
    self->playlist_offset_ += 6;
    self->playlist_count_ = 0;
    self->UpdatePlaylist();
    Application::GetInstance().SendMediaCommand("get_playlist", self->playlist_offset_);
}
void PixelTouchUI::OnPlaylistSelect(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    auto* button = lv_event_get_target_obj(event);
    int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button))) - 1;
    if (index < 0 || index >= self->media_playlist_length_) return;
    Application::GetInstance().SendMediaCommand("select", index);
    if (self->playlist_panel_ != nullptr) {
        lv_obj_add_flag(self->playlist_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void PixelTouchUI::OnMediaSeek(lv_event_t* event) {
    auto* self = static_cast<PixelTouchUI*>(lv_event_get_user_data(event));
    if (self->player_seek_slider_ == nullptr || self->media_duration_ms_ <= 0) return;
    int ratio = lv_slider_get_value(self->player_seek_slider_);
    int64_t targetMs = self->media_duration_ms_ * ratio / 1000;
    Application::GetInstance().SendMediaCommand("seek", -1, targetMs);
}

#include "pixel_robot_display.h"
#include "pixel_touch_ui.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <material_symbols.h>

#if CONFIG_LCDWIKI_ES3C28P_LANDSCAPE && CONFIG_USE_WECHAT_MESSAGE_STYLE

#include <algorithm>
#include <cstring>
#include "application.h"
#include "assets.h"
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#endif

namespace {
constexpr uint32_t kBackground = 0x071326;
constexpr uint32_t kText = 0xd4e5e6;
constexpr uint32_t kUserBubble = 0x203d48;
constexpr uint32_t kAssistantBubble = 0x1a2e44;

void DrawChatBubble(lv_event_t* event) {
    auto* bubble = lv_event_get_target_obj(event);
    auto* layer = lv_event_get_layer(event);
    const bool user = lv_event_get_user_data(event) != nullptr;
    lv_area_t bounds;
    lv_obj_get_coords(bubble, &bounds);
    const auto clip = layer->_clip_area;

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.base.obj = bubble;
    rect.bg_color = lv_obj_get_style_bg_color(bubble, LV_PART_MAIN);
    rect.border_color = lv_obj_get_style_border_color(bubble, LV_PART_MAIN);
    rect.border_width = 1;

    // Draw the same rectangle in non-overlapping clips so the small corner
    // shares the exact fill and border with the rest of the LVGL bubble.
    const auto draw = [&](const lv_area_t& region, int radius) {
        layer->_clip_area = {std::max(clip.x1, region.x1), std::max(clip.y1, region.y1),
                             std::min(clip.x2, region.x2), std::min(clip.y2, region.y2)};
        if (layer->_clip_area.x1 <= layer->_clip_area.x2 &&
            layer->_clip_area.y1 <= layer->_clip_area.y2) {
            rect.radius = radius;
            lv_draw_rect(layer, &rect, &bounds);
        }
    };
    const int32_t middle_x = bounds.x1 + lv_area_get_width(&bounds) / 2;
    const int32_t lower_y = bounds.y1 + lv_area_get_height(&bounds) / 2;
    draw({bounds.x1, bounds.y1, bounds.x2, lower_y - 1}, 10);
    draw({bounds.x1, lower_y, middle_x - 1, bounds.y2}, user ? 10 : 3);
    draw({middle_x, lower_y, bounds.x2, bounds.y2}, user ? 3 : 10);
    layer->_clip_area = clip;
}

std::unique_ptr<LvglCBinFont> LoadUiFont(const char* name) {
    void* data = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(name, data, size) || size < sizeof(lv_font_t)) {
        return nullptr;
    }
    auto font = std::make_unique<LvglCBinFont>(data);
    if (font->font() == nullptr) {
        return nullptr;
    }
    return font;
}

// One existing LVGL line changes its point set; no expression sprites,
// additional images, canvases or per-frame LVGL object allocations.
static const lv_point_precise_t kHappy[] = {{0, 0}, {8, 12}, {16, 15}, {24, 12}, {32, 0}};
static const lv_point_precise_t kThinking[] = {{0, 5}, {8, 4}, {16, 4}, {24, 4}, {32, 5}};
static const lv_point_precise_t kSad[] = {{0, 10}, {8, 2}, {16, 0}, {24, 2}, {32, 10}};

lv_obj_t* PixelRect(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t* rect = lv_obj_create(parent);
    lv_obj_remove_style_all(rect);
    lv_obj_set_pos(rect, x, y);
    lv_obj_set_size(rect, w, h);
    lv_obj_set_style_bg_color(rect, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rect, LV_OBJ_FLAG_CLICKABLE);
    return rect;
}
}  // namespace

PixelRobotDisplay::~PixelRobotDisplay() {
    if (chat_player_button_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_obj_delete(chat_player_button_);
        chat_player_button_ = nullptr;
    }
    if (touch_ui_) {
        DisplayLockGuard lock(this);
        touch_ui_.reset();
    }
    if (blink_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_delete(blink_timer_);
        lv_anim_delete(this, nullptr);
        blink_timer_ = nullptr;
    }
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
    if (resource_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_delete(resource_timer_);
        resource_timer_ = nullptr;
    }
#endif
}

void PixelRobotDisplay::SetupUI() {
    if (setup_ui_called_)
        return;
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);

    // Insert a fixed-height portrait between the existing status bar and chat.
    robot_area_ = lv_obj_create(container_);
    lv_obj_remove_style_all(robot_area_);
    lv_obj_set_size(robot_area_, 320, 100);
    lv_obj_clear_flag(robot_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(robot_area_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(robot_area_, 1);

    // V1.1 mascot: 96px orbit, 78px circular face, soft eyes and smile.
    // Keep one reusable robot object across chat and idle pages.
    lv_obj_t* robot = lv_obj_create(robot_area_);
    lv_obj_remove_style_all(robot);
    lv_obj_set_size(robot, 104, 124);
    lv_obj_clear_flag(robot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(robot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(robot);

    auto* orbit = robot_orbit_ = lv_obj_create(robot);
    lv_obj_remove_style_all(orbit);
    lv_obj_set_pos(orbit, 4, 0);
    lv_obj_set_size(orbit, 96, 96);
    lv_obj_set_style_radius(orbit, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(orbit, 1, 0);
    lv_obj_set_style_border_color(orbit, lv_color_hex(0x70e1d4), 0);
    lv_obj_set_style_border_opa(orbit, 0x44, 0);
    lv_obj_set_style_shadow_color(orbit, lv_color_hex(0x44b3bd), 0);
    lv_obj_set_style_shadow_width(orbit, 28, 0);
    lv_obj_set_style_shadow_opa(orbit, 0x22, 0);
#if LV_USE_DRAW_SW_COMPLEX_GRADIENTS && LV_GRADIENT_MAX_STOPS >= 3
    static const auto orbit_glow = [] {
        lv_grad_dsc_t gradient{};
        const auto teal = lv_color_hex(0x44b3bd);
        const lv_color_t colors[] = {teal, teal, teal};
        const lv_opa_t opacity[] = {0, 4, 0x22};
        const uint8_t stops[] = {140, 195, 255};
        lv_grad_init_stops(&gradient, colors, opacity, stops, 3);
        lv_grad_radial_init(&gradient, LV_GRAD_CENTER, LV_GRAD_CENTER, LV_GRAD_RIGHT,
                            LV_GRAD_CENTER, LV_GRAD_EXTEND_PAD);
        return gradient;
    }();
    lv_obj_set_style_bg_grad(orbit, &orbit_glow, 0);
    lv_obj_set_style_bg_opa(orbit, LV_OPA_COVER, 0);
#endif
    lv_obj_clear_flag(orbit, LV_OBJ_FLAG_CLICKABLE);

    auto* face = robot_face_ = PixelRect(robot, 13, 9, 78, 78, 0x356d8f);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
#if LV_USE_DRAW_SW_COMPLEX_GRADIENTS && LV_GRADIENT_MAX_STOPS >= 3
    static const auto face_gradient = [] {
        lv_grad_dsc_t gradient{};
        const lv_color_t colors[] = {lv_color_hex(0x356d8f), lv_color_hex(0x183b59),
                                     lv_color_hex(0x122b43)};
        const uint8_t stops[] = {0, 166, 255};
        lv_grad_init_stops(&gradient, colors, nullptr, stops, 3);
        lv_grad_radial_init(&gradient, lv_pct(35), lv_pct(23), LV_GRAD_RIGHT, LV_GRAD_BOTTOM,
                            LV_GRAD_EXTEND_PAD);
        return gradient;
    }();
    lv_obj_set_style_bg_grad(face, &face_gradient, 0);
#else
    lv_obj_set_style_bg_grad_color(face, lv_color_hex(0x183b59), 0);
    lv_obj_set_style_bg_grad_dir(face, LV_GRAD_DIR_VER, 0);
#endif
    lv_obj_set_style_border_width(face, 1, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(0x9ae7f4), 0);
    lv_obj_set_style_border_opa(face, 0x38, 0);

    for (int i = 0; i < 2; ++i) {
        cheeks_[i] = PixelRect(robot, i == 0 ? 25 : 70, 58, 9, 4, 0xffa4a5);
        lv_obj_set_style_radius(cheeks_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_width(cheeks_[i], 4, 0);
        lv_obj_set_style_shadow_color(cheeks_[i], lv_color_hex(0xffa4a5), 0);
        lv_obj_set_style_shadow_opa(cheeks_[i], LV_OPA_30, 0);
        eyes_[i] = PixelRect(robot, i == 0 ? 34 : 61, 36, 9, 13, 0xaffbf4);
        lv_obj_set_style_radius(eyes_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_color(eyes_[i], lv_color_hex(0x66f3e5), 0);
        lv_obj_set_style_shadow_width(eyes_[i], 9, 0);
        lv_obj_set_style_shadow_opa(eyes_[i], 0x99, 0);
    }
    // Use LVGL's arc renderer for the smile and its full-ring border path for speech.
    curved_mouth_ = lv_obj_create(robot);
    lv_obj_remove_style_all(curved_mouth_);
    lv_obj_remove_flag(curved_mouth_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(curved_mouth_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        curved_mouth_,
        [](lv_event_t* event) {
            auto* self = static_cast<PixelRobotDisplay*>(lv_event_get_user_data(event));
            const bool speaking = self->expression_ == RobotExpression::Speaking;
            lv_area_t bounds;
            lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
            const int32_t diameter = lv_area_get_width(&bounds);
            lv_draw_arc_dsc_t arc;
            lv_draw_arc_dsc_init(&arc);
            arc.center = {bounds.x1 + diameter / 2, bounds.y1 + (speaking ? diameter / 2 : 0)};
            arc.radius = diameter / 2 - (speaking ? 0 : 1);
            arc.width = self->robot_compact_ ? 2 : 3;
            arc.start_angle = speaking ? 0 : 30;
            arc.end_angle = speaking ? 360 : 150;
            arc.rounded = !speaking;
            arc.color = lv_color_hex(0xb0f8f5);
            lv_draw_arc(lv_event_get_layer(event), &arc);
        },
        LV_EVENT_DRAW_MAIN, this);
    mouth_ = lv_line_create(robot);
    lv_obj_set_pos(mouth_, 36, 53);
    lv_obj_set_style_line_color(mouth_, lv_color_hex(0xb0f8f5), 0);
    lv_obj_set_style_line_width(mouth_, 3, 0);
    lv_obj_set_style_line_rounded(mouth_, true, 0);
    lv_obj_clear_flag(mouth_, LV_OBJ_FLAG_CLICKABLE);

    auto* caption = lv_label_create(robot);
    lv_label_set_text(caption, "点我说话");
    lv_obj_set_style_text_color(caption, lv_color_hex(0xa5f2e9), 0);
    lv_obj_set_width(caption, 104);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(caption, 0, 104);
    // The boot-time robot_area_ is only 100px tall while the caption sits at
    // y=104, so it would be clipped until the first AttachRobot() resize.
    // Start hidden; AttachRobot shows it per page (home/voice rules).
    lv_obj_add_flag(caption, LV_OBJ_FLAG_HIDDEN);

    HideDefaultEmotion();
    ApplyChatColors();
    ApplyExpression();
    blink_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<PixelRobotDisplay*>(lv_timer_get_user_data(timer));
            lv_timer_set_period(timer, 5000);
            lv_anim_t blink;
            lv_anim_init(&blink);
            lv_anim_set_var(&blink, self);
            lv_anim_set_exec_cb(&blink, [](void* obj, int32_t percent) {
                auto* display = static_cast<PixelRobotDisplay*>(obj);
                display->blink_percent_ = percent;
                display->ApplyEyes();
            });
            lv_anim_set_values(&blink, 100, 12);
            lv_anim_set_duration(&blink, 100);
            lv_anim_set_playback_duration(&blink, 100);
            lv_anim_set_path_cb(&blink, lv_anim_path_ease_in_out);
            lv_anim_start(&blink);
        },
        2150, this);
    touch_ui_ = std::make_unique<PixelTouchUI>(
        lv_screen_active(), content_,
        [](void* context, bool compact) {
            static_cast<PixelRobotDisplay*>(context)->SetRobotCompact(compact);
        },
        this);

    // Keep a media entry on the fallback chat surface as well as the voice header.
    chat_player_button_ = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(chat_player_button_);
    lv_obj_set_pos(chat_player_button_, 273, 32);
    lv_obj_set_size(chat_player_button_, 42, 34);
    lv_obj_set_style_bg_color(chat_player_button_, lv_color_hex(0x10263B), 0);
    lv_obj_set_style_bg_opa(chat_player_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chat_player_button_, lv_color_hex(0x80E9DD), 0);
    lv_obj_set_style_border_width(chat_player_button_, 1, 0);
    lv_obj_add_flag(chat_player_button_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chat_player_button_, LV_OBJ_FLAG_SCROLLABLE);
    auto* music_icon = lv_label_create(chat_player_button_);
    lv_label_set_text(music_icon, MATERIAL_SYMBOLS_MUSIC_NOTE);
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    lv_obj_set_style_text_font(music_icon, theme->icon_font()->font(), 0);
    lv_obj_set_style_text_color(music_icon, lv_color_hex(0x80E9DD), 0);
    lv_obj_center(music_icon);
    lv_obj_add_event_cb(chat_player_button_, [](lv_event_t* event) {
        auto* self = static_cast<PixelRobotDisplay*>(lv_event_get_user_data(event));
        if (self->touch_ui_ != nullptr) {
            self->touch_ui_->ShowPlayer(self->robot_area_, self->container_);
        }
    }, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(chat_player_button_, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_LCDWIKI_ES3C28P_RESOURCE_DIAGNOSTICS
    // Diagnostic-only: sample in the existing LVGL task; no extra task or
    // full-screen refresh. Heap minimums are historical low-water marks.
    auto sample_resources = [](lv_timer_t*) {
        const auto state = Application::GetInstance().GetDeviceState();
        constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        ESP_LOGI("ES3C28P_RES",
                 "uptime_ms=%llu state=%s internal_free=%u internal_min=%u "
                 "internal_largest=%u psram_free=%u psram_min=%u psram_largest=%u",
                 static_cast<unsigned long long>(esp_timer_get_time() / 1000),
                 DeviceStateMachine::GetStateName(state),
                 static_cast<unsigned>(heap_caps_get_free_size(internal)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
                 static_cast<unsigned>(heap_caps_get_free_size(psram)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(psram)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(psram)));
    };
    sample_resources(nullptr);
    resource_timer_ = lv_timer_create(sample_resources, 3000, nullptr);
#endif
}

void PixelRobotDisplay::ApplyEyes() {
    const bool happy = expression_ == RobotExpression::Happy;
    const bool thinking = expression_ == RobotExpression::Thinking;
    const bool sad = expression_ == RobotExpression::Sad;
    const int32_t open_height = happy ? 7 : (thinking ? 9 : 13);
    const int32_t open_y = happy ? 39 : (sad ? 38 : 36);
    const int32_t height = robot_compact_ ? (open_height * 2 + 3) / 5 : open_height;
    const int32_t y = robot_compact_ ? (open_y * 2 + 1) / 5 : open_y;
    const int32_t visible_height = std::max<int32_t>(1, (height * blink_percent_ + 50) / 100);
    for (int i = 0; i < 2; ++i) {
        const int32_t x = robot_compact_ ? (i == 0 ? 12 : 24) : (i == 0 ? 34 : 61);
        lv_obj_set_pos(eyes_[i], x, y + (height - visible_height) / 2);
        lv_obj_set_size(eyes_[i], robot_compact_ ? 4 : 9, visible_height);
    }
}

void PixelRobotDisplay::SetRobotCompact(bool compact) {
    if (robot_compact_ == compact)
        return;
    robot_compact_ = compact;
    auto* robot = lv_obj_get_parent(robot_face_);
    lv_obj_set_size(robot, compact ? 40 : 104, compact ? 40 : 124);
    lv_obj_set_pos(robot_orbit_, compact ? 0 : 4, 0);
    lv_obj_set_size(robot_orbit_, compact ? 40 : 96, compact ? 40 : 96);
    lv_obj_set_pos(robot_face_, compact ? 4 : 13, compact ? 4 : 9);
    lv_obj_set_size(robot_face_, compact ? 32 : 78, compact ? 32 : 78);
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_style_shadow_width(eyes_[i], compact ? 4 : 9, 0);
        if (compact)
            lv_obj_add_flag(cheeks_[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(cheeks_[i], LV_OBJ_FLAG_HIDDEN);
    }
    ApplyExpression();
}

void PixelRobotDisplay::ApplyExpression() {
    if (eyes_[0] == nullptr || mouth_ == nullptr) return;
    const bool happy = expression_ == RobotExpression::Happy;
    const bool sad = expression_ == RobotExpression::Sad;
    const bool thinking = expression_ == RobotExpression::Thinking;
    const bool speaking = expression_ == RobotExpression::Speaking;
    const auto position = [this](int point) {
        return robot_compact_ ? (point * 2 + 2) / 5 : point;
    };
    ApplyEyes();
    for (auto* cheek : cheeks_) {
        lv_obj_set_style_bg_opa(cheek, happy || speaking ? LV_OPA_60 : LV_OPA_30, 0);
    }
    if (speaking || expression_ == RobotExpression::Neutral) {
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        if (speaking) {
            const int32_t diameter = robot_compact_ ? 8 : 16;
            lv_obj_set_pos(curved_mouth_, robot_compact_ ? 16 : 44, robot_compact_ ? 20 : 51);
            lv_obj_set_size(curved_mouth_, diameter, diameter);
        } else {
            lv_obj_set_pos(curved_mouth_, robot_compact_ ? 13 : 38, robot_compact_ ? 22 : 53);
            lv_obj_set_size(curved_mouth_, robot_compact_ ? 14 : 28, robot_compact_ ? 8 : 16);
        }
        lv_obj_remove_flag(curved_mouth_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(curved_mouth_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);

    const auto* points = sad ? kSad : (thinking ? kThinking : kHappy);
    constexpr int count = 5;
    if (robot_compact_) {
        for (int i = 0; i < count; ++i) {
            mouth_points_[i] = {position(points[i].x), position(points[i].y)};
        }
        points = mouth_points_;
    }
    lv_line_set_points(mouth_, points, count);
    lv_obj_set_style_line_color(mouth_, lv_color_hex(sad ? 0x96c1d6 : 0xb0f8f5), 0);
    lv_obj_set_style_line_width(mouth_, robot_compact_ ? 2 : (happy ? 4 : 3), 0);
    lv_obj_set_pos(mouth_, position(36) - (robot_compact_ ? 1 : 0), position(53));
}

void PixelRobotDisplay::HideDefaultEmotion() {
    if (emoji_label_ != nullptr)
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_image_ != nullptr)
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
}

void PixelRobotDisplay::ApplyChatColors() {
    if (container_ == nullptr)
        return;
    lv_obj_set_style_bg_image_src(container_, nullptr, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_color(top_bar_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_color(content_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(
        content_, lv_obj_get_parent(content_) == container_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    for (auto* label :
         {network_label_, status_label_, notification_label_, mute_label_, battery_label_}) {
        if (label != nullptr)
            lv_obj_set_style_text_color(label, lv_color_hex(kText), 0);
    }
    StyleMessages();
}

void PixelRobotDisplay::StyleMessages() {
    if (content_ == nullptr)
        return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(content_); ++i) {
        StyleMessage(lv_obj_get_child(content_, i));
    }
}

void PixelRobotDisplay::StyleMessage(lv_obj_t* row) {
    if (row == nullptr)
        return;
    auto* bubble = row;
    const char* role = static_cast<const char*>(lv_obj_get_user_data(bubble));
    if (role == nullptr && lv_obj_get_child_count(row) > 0) {
        bubble = lv_obj_get_child(row, 0);
        role = static_cast<const char*>(lv_obj_get_user_data(bubble));
    }
    if (role == nullptr || (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0 &&
                            strcmp(role, "system") != 0)) {
        return;
    }
    const bool user = strcmp(role, "user") == 0;
    const bool system = strcmp(role, "system") == 0;
    lv_obj_set_style_bg_color(bubble, lv_color_hex(user ? kUserBubble : kAssistantBubble), 0);
    lv_obj_set_style_bg_opa(bubble, system ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(user ? 0x386669 : 0x2f4962), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_radius(bubble, 10, 0);
    if (!system) {
        // The first style pass installs the painter. A theme refresh reuses it.
        if (lv_obj_get_style_border_opa(bubble, LV_PART_MAIN) != LV_OPA_TRANSP) {
            lv_obj_add_event_cb(bubble, DrawChatBubble, LV_EVENT_DRAW_MAIN, user ? row : nullptr);
        }
        lv_obj_set_style_border_opa(bubble, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_pad_hor(bubble, 9, 0);
    lv_obj_set_style_pad_ver(bubble, 6, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    if (row != bubble) {
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bubble, system ? LV_ALIGN_CENTER : LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (auto* label = lv_obj_get_child(bubble, 0)) {
        // The same list also serves the fallback screen. Keep bubbles inside the
        // narrowest viewport and let LVGL wrap to the actual content width.
        lv_obj_set_width(label, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(label, 226, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(user ? 0xbdf7ec : 0xebf4fb), 0);
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    }
}

void PixelRobotDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    SpiLcdDisplay::SetTheme(theme);
    if (touch_ui_ != nullptr) {
        if (theme_font_shared_ == nullptr) {
            theme_font_shared_ =
                std::shared_ptr<LvglCBinFont>(LoadUiFont("font_noto_sans_common_18_4.bin"));
        }
        if (theme_font_shared_ != nullptr && !theme_font_applied_) {
            // Set before SetTextFont: it re-enters SetTheme on the rebound
            // styles, and the flag stops the recursion here.
            theme_font_applied_ = true;
            SetTextFont(theme_font_shared_);
        }
        if (muted_asset_font_ == nullptr) {
            muted_asset_font_ = LoadUiFont("font_noto_sans_common_16_4.bin");
        }
        if (player_title_asset_font_ == nullptr) {
            player_title_asset_font_ = LoadUiFont("font_noto_sans_common_20_4.bin");
        }
        touch_ui_->RefreshTextFonts(
            theme->GetTextFont()->font(), muted_asset_font_ ? muted_asset_font_->font() : nullptr,
            player_title_asset_font_ ? player_title_asset_font_->font() : nullptr);
    }
    if (robot_area_ != nullptr) {
        ApplyChatColors();
        HideDefaultEmotion();
    }
}

void PixelRobotDisplay::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr)
        return;
    DisplayLockGuard lock(this);
    SpiLcdDisplay::SetChatMessage(role, content);
    if (content_ != nullptr && content[0] != '\0') {
        StyleMessage(lv_obj_get_child(content_, -1));
    }
    HideDefaultEmotion();
    if (touch_ui_ != nullptr) touch_ui_->OnChatMessage(role, content);
    if (content[0] != '\0') {
        if (strcmp(role, "user") == 0) expression_ = RobotExpression::Thinking;
        else if (strcmp(role, "assistant") == 0) expression_ = RobotExpression::Speaking;
        ApplyExpression();
    }
}

void PixelRobotDisplay::RemoveChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::RemoveChatMessage(role, content);
    DisplayLockGuard lock(this);
    HideDefaultEmotion();
}

void PixelRobotDisplay::ClearChatMessages() {
    SpiLcdDisplay::ClearChatMessages();
    DisplayLockGuard lock(this);
    HideDefaultEmotion();
}

bool PixelRobotDisplay::SetEmotion(const char* emotion) {
    if (robot_area_ == nullptr || emotion == nullptr) return false;
    RobotExpression selected;
    if (strcmp(emotion, "thinking") == 0) selected = RobotExpression::Thinking;
    else if (strcmp(emotion, "sad") == 0) selected = RobotExpression::Sad;
    else if (strcmp(emotion, "happy") == 0) selected = RobotExpression::Happy;
    else if (strcmp(emotion, "speaking") == 0) selected = RobotExpression::Speaking;
    else if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "robot_2") == 0)
        selected = RobotExpression::Neutral;
    else return false;
    DisplayLockGuard lock(this);
    expression_ = selected;
    ApplyExpression();
    HideDefaultEmotion();
    return true;
}

void PixelRobotDisplay::SetMediaState(const char* state, const char* title,
                                      int64_t position_ms, int64_t duration_ms,
                                      int current_index, int playlist_length,
                                      const char* error, std::optional<bool> shuffle) {
    DisplayLockGuard lock(this);
    if (touch_ui_ != nullptr) {
        touch_ui_->SetMediaState(state, title, position_ms, duration_ms,
                                 current_index, playlist_length, error, shuffle);
    }
}

void PixelRobotDisplay::SetMediaPlaylist(int offset, const std::vector<std::string>& titles) {
    DisplayLockGuard lock(this);
    if (touch_ui_) touch_ui_->SetMediaPlaylist(offset, titles);
}

#if CONFIG_WEATHER_DASHBOARD
void PixelRobotDisplay::ShowDashboard() {
    if (touch_ui_ == nullptr || robot_area_ == nullptr) {
        SpiLcdDisplay::ShowDashboard();
        return;
    }
    DisplayLockGuard lock(this);
    // Preserve existing weather/almanac services; replace only the idle view.
    expression_ = RobotExpression::Neutral;
    ApplyExpression();
    if (!touch_ui_->IsPlayerVisible()) {
        touch_ui_->Show(robot_area_, container_);
    }
    if (chat_player_button_ != nullptr) {
        lv_obj_add_flag(chat_player_button_, LV_OBJ_FLAG_HIDDEN);
    }
    // Keep the existing low-battery warning above the idle touch surface.
    if (low_battery_popup_ != nullptr) {
        auto* screen = lv_obj_get_parent(low_battery_popup_);
        lv_obj_move_to_index(low_battery_popup_, lv_obj_get_child_count(screen) - 1);
    }
}

void PixelRobotDisplay::HideDashboard() {
    if (touch_ui_ == nullptr) {
        SpiLcdDisplay::HideDashboard();
        return;
    }
    DisplayLockGuard lock(this);
    const auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        expression_ = RobotExpression::Speaking;
        ApplyExpression();
    } else if (state == kDeviceStateConnecting) {
        expression_ = RobotExpression::Neutral;
        ApplyExpression();
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        // Global wake-word response: whenever the device enters a voice state
        // (e.g. "ni hao xiao zhi" detected on ANY touch page), always switch
        // to the voice screen so the user gets a visible response. Previously
        // an open player page suppressed this, which made wake-up look dead.
        // Audio playback keeps running in the background; only the UI yields.
        ESP_LOGI("ES3C28P_UI", "voice state %d: forcing voice screen", (int)state);
        touch_ui_->ShowVoice(robot_area_, container_);
        // The media shortcut lives in the voice header so it cannot cover the status or bubbles.
        if (chat_player_button_ != nullptr) {
            lv_obj_add_flag(chat_player_button_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    touch_ui_->Hide();
    if (chat_player_button_ != nullptr) {
        lv_obj_clear_flag(chat_player_button_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

#endif

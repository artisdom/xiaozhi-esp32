#include "music_display.h"

#include <cstdio>

#include "lvgl_theme.h"

void MusicDisplay::UpdateMusic(const MusicSnapshot& state, bool visible, int volume) {
    if (!IsSetupUICalled())
        return;
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    if (!music_panel_ && !visible)
        return;
    if (!music_panel_) {
        music_panel_ = lv_obj_create(lv_display_get_screen_active(display_));
        lv_obj_set_size(music_panel_, width_, height_ - 45);
        lv_obj_align(music_panel_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(music_panel_, 0, 0);
        lv_obj_set_style_border_width(music_panel_, 0, 0);
        lv_obj_set_style_pad_all(music_panel_, 10, 0);
        lv_obj_set_style_pad_row(music_panel_, 6, 0);
        lv_obj_set_flex_flow(music_panel_, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(music_panel_, LV_OBJ_FLAG_SCROLLABLE);
        auto label = [this]() {
            auto* obj = lv_label_create(music_panel_);
            lv_obj_set_width(obj, width_ - 20);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
            return obj;
        };
        title_ = label();
        artist_ = label();
        album_ = label();
        progress_ = lv_bar_create(music_panel_);
        lv_obj_set_size(progress_, width_ - 20, 10);
        lv_bar_set_range(progress_, 0, 1000);
        time_ = label();
        phase_ = label();
        mode_ = label();
        help_ = label();
        lv_label_set_long_mode(help_, LV_LABEL_LONG_WRAP);
    }
    if (!visible) {
        lv_obj_add_flag(music_panel_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(music_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(music_panel_);
    if (auto* theme = static_cast<LvglTheme*>(GetTheme())) {
        lv_obj_set_style_bg_color(music_panel_, theme->background_color(), 0);
        lv_obj_set_style_text_color(music_panel_, theme->text_color(), 0);
        lv_obj_set_style_text_font(music_panel_, theme->text_font()->font(), 0);
    }
    // Leave marquee labels unchanged between ticks so scrolling does not restart.
    auto text = [](lv_obj_t* label, const std::string& value) {
        if (value != lv_label_get_text(label))
            lv_label_set_text(label, value.c_str());
    };
    text(title_, state.title.empty() ? "SD Music" : state.title);
    text(artist_, state.artist.empty() ? "Unknown artist" : state.artist);
    text(album_, state.album.empty() ? state.filename : state.album);
    char line[128];
    snprintf(
        line, sizeof(line), "%lu:%02lu / %lu:%02lu", (unsigned long)(state.position_ms / 60000),
        (unsigned long)(state.position_ms / 1000 % 60), (unsigned long)(state.duration_ms / 60000),
        (unsigned long)(state.duration_ms / 1000 % 60));
    text(time_, line);
    lv_bar_set_value(progress_,
                     state.duration_ms ? uint64_t(state.position_ms) * 1000 / state.duration_ms : 0,
                     LV_ANIM_OFF);
    snprintf(line, sizeof(line), "%s  %u/%u  Vol %d", state.phase.c_str(),
             unsigned(state.total ? state.index + 1 : 0), unsigned(state.total), volume);
    text(phase_, state.error.empty() ? std::string(line) : state.error);
    text(mode_, "Repeat: " + state.repeat + (state.shuffle ? "  Shuffle" : "  In order"));
    text(help_, "M: play/pause\n2x L/R: prev/next\n2x M: stop/chat");
}

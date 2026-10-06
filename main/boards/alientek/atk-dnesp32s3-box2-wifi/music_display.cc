#include "music_display.h"

#include <cstdio>

#include "lvgl_theme.h"

void MusicDisplay::UpdateMusic(const MusicSnapshot& state, bool visible, int volume,
                               const std::vector<MusicListRow>& rows) {
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
        progress_ = lv_bar_create(music_panel_);
        lv_obj_set_size(progress_, width_ - 20, 10);
        lv_bar_set_range(progress_, 0, 1000);
        time_ = label();
        phase_ = label();
        for (auto& row : rows_) {
            row = label();
            lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
            lv_obj_set_style_pad_hor(row, 4, 0);
            lv_obj_set_style_radius(row, 4, 0);
        }
        help_ = label();
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
    std::string byline = state.artist.empty() ? "Unknown artist" : state.artist;
    if (!state.album.empty())
        byline += " - " + state.album;
    text(artist_, byline);
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

    auto* theme = static_cast<LvglTheme*>(GetTheme());
    for (int i = 0; i < kListRows; ++i) {
        if (i >= int(rows.size())) {
            text(rows_[i], i == 0 && state.library_scanning ? "Scanning SD card..." : "");
            lv_obj_set_style_bg_opa(rows_[i], LV_OPA_TRANSP, 0);
            continue;
        }
        text(rows_[i], (rows[i].playing ? "> " : "  ") + rows[i].text);
        lv_obj_set_style_bg_opa(rows_[i], rows[i].selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (theme) {
            lv_obj_set_style_bg_color(rows_[i], theme->text_color(), 0);
            lv_obj_set_style_text_color(
                rows_[i], rows[i].selected ? theme->background_color() : theme->text_color(), 0);
        }
    }
    text(help_, "L/R:select  M:play  2xL/R:skip  2xM:exit");
}

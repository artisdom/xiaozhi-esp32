#include "music_display.h"

#include <algorithm>
#include <cstdio>

#include "lvgl_theme.h"

namespace {
constexpr int kPanelPadding = 10;
constexpr int kRowGap = 6;
constexpr int kStatusBarHeight = 45;
}  // namespace

int MusicDisplay::VisibleListRows() {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    int line = (theme ? lv_font_get_line_height(theme->text_font()->font()) : 20) + kRowGap;
    // Path header, state line and hint line take three lines besides the rows.
    int rows = (height_ - kStatusBarHeight - 2 * kPanelPadding) / line - 3;
    return std::clamp(rows, 3, kMaxListRows);
}

void MusicDisplay::UpdateMusic(const MusicSnapshot& state, bool visible, int volume,
                               const MusicBrowseView& view) {
    if (!IsSetupUICalled())
        return;
    int visible_rows = VisibleListRows();
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    if (!music_panel_ && !visible)
        return;
    if (!music_panel_) {
        music_panel_ = lv_obj_create(lv_display_get_screen_active(display_));
        lv_obj_set_size(music_panel_, width_, height_ - kStatusBarHeight);
        lv_obj_align(music_panel_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_radius(music_panel_, 0, 0);
        lv_obj_set_style_border_width(music_panel_, 0, 0);
        lv_obj_set_style_pad_all(music_panel_, kPanelPadding, 0);
        lv_obj_set_style_pad_row(music_panel_, kRowGap, 0);
        lv_obj_set_flex_flow(music_panel_, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(music_panel_, LV_OBJ_FLAG_SCROLLABLE);
        auto label = [this](lv_label_long_mode_t mode = LV_LABEL_LONG_SCROLL_CIRCULAR) {
            auto* obj = lv_label_create(music_panel_);
            lv_obj_set_width(obj, width_ - 2 * kPanelPadding);
            lv_label_set_long_mode(obj, mode);
            return obj;
        };
        // Creation order is the on-screen order; each view hides the other's widgets.
        path_ = label();
        for (auto& row : rows_) {
            row = label(LV_LABEL_LONG_DOT);
            lv_obj_set_style_pad_hor(row, 4, 0);
            lv_obj_set_style_radius(row, 4, 0);
        }
        title_ = label();
        artist_ = label();
        progress_ = lv_bar_create(music_panel_);
        lv_obj_set_size(progress_, width_ - 2 * kPanelPadding, 10);
        lv_bar_set_range(progress_, 0, 1000);
        time_ = label();
        mode_ = label();
        phase_ = label();
        help_ = label();
    }
    if (!visible) {
        lv_obj_add_flag(music_panel_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(music_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(music_panel_);
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    if (theme) {
        lv_obj_set_style_bg_color(music_panel_, theme->background_color(), 0);
        lv_obj_set_style_text_color(music_panel_, theme->text_color(), 0);
        lv_obj_set_style_text_font(music_panel_, theme->text_font()->font(), 0);
    }
    auto show = [](lv_obj_t* obj, bool shown) {
        if (shown)
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    };
    // Leave marquee labels unchanged between ticks so scrolling does not restart.
    auto text = [](lv_obj_t* label, const std::string& value) {
        if (value != lv_label_get_text(label))
            lv_label_set_text(label, value.c_str());
    };

    char line[128];
    snprintf(line, sizeof(line), "%s  %u/%u  Vol %d", state.phase.c_str(),
             unsigned(state.total ? state.index + 1 : 0), unsigned(state.total), volume);
    text(phase_, state.error.empty() ? std::string(line) : state.error);
    show(phase_, true);
    show(help_, true);

    show(path_, view.browsing);
    for (int i = 0; i < kMaxListRows; ++i)
        show(rows_[i], view.browsing && i < visible_rows);
    for (auto* obj : {title_, artist_, progress_, time_, mode_})
        show(obj, !view.browsing);

    if (view.browsing) {
        text(path_, "SD:/" + view.path);
        for (int i = 0; i < visible_rows; ++i) {
            if (i >= int(view.rows.size())) {
                text(rows_[i], i == 0 && view.rows.empty()
                                   ? (state.library_scanning ? "Scanning SD card..." : "No MP3 files")
                                   : "");
                lv_obj_set_style_bg_opa(rows_[i], LV_OPA_TRANSP, 0);
                continue;
            }
            const auto& row = view.rows[i];
            text(rows_[i], (row.playing ? "> " : "  ") + row.text);
            lv_obj_set_style_bg_opa(rows_[i], row.selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            if (theme) {
                lv_obj_set_style_bg_color(rows_[i], theme->text_color(), 0);
                lv_obj_set_style_text_color(
                    rows_[i], row.selected ? theme->background_color() : theme->text_color(), 0);
            }
        }
        text(help_, "L/R:move  M:open/play  2xL:back  2xM:exit  hold L:player");
        return;
    }

    text(title_, state.title.empty() ? "SD Music" : state.title);
    std::string byline = state.artist.empty() ? "Unknown artist" : state.artist;
    if (!state.album.empty())
        byline += " - " + state.album;
    text(artist_, byline);
    snprintf(
        line, sizeof(line), "%lu:%02lu / %lu:%02lu", (unsigned long)(state.position_ms / 60000),
        (unsigned long)(state.position_ms / 1000 % 60), (unsigned long)(state.duration_ms / 60000),
        (unsigned long)(state.duration_ms / 1000 % 60));
    text(time_, line);
    lv_bar_set_value(progress_,
                     state.duration_ms ? uint64_t(state.position_ms) * 1000 / state.duration_ms : 0,
                     LV_ANIM_OFF);
    text(mode_, "Repeat: " + state.repeat + (state.shuffle ? "  Shuffle" : "  In order"));
    text(help_, "L/R:vol  2xL/R:skip  M:pause  2xM:exit  hold L:files");
}

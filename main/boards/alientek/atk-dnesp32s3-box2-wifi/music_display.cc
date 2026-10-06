#include "music_display.h"

#include <algorithm>
#include <cstdio>

#include "lvgl_theme.h"

namespace {
constexpr int kPanelPadding = 10;
constexpr int kRowGap = 6;
constexpr int kStatusBarHeight = 45;
constexpr int kWaveBarWidth = 3;
constexpr int kWaveBarGap = 1;
using box2_music::DjEngine;

int LineHeight(LvglTheme* theme) {
    return (theme ? lv_font_get_line_height(theme->text_font()->font()) : 20) + kRowGap;
}
}  // namespace

int MusicDisplay::VisibleListRows() {
    // Path header, state line and hint line take three lines besides the rows.
    int rows = (height_ - kStatusBarHeight - 2 * kPanelPadding) / LineHeight(static_cast<LvglTheme*>(GetTheme())) - 3;
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
    const int inner_width = width_ - 2 * kPanelPadding;
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
        auto label = [this, inner_width](lv_label_long_mode_t mode = LV_LABEL_LONG_SCROLL_CIRCULAR) {
            auto* obj = lv_label_create(music_panel_);
            lv_obj_set_width(obj, inner_width);
            lv_label_set_long_mode(obj, mode);
            return obj;
        };
        // Creation order is the on-screen order; each screen hides the other screens' widgets.
        path_ = label();
        for (auto& row : rows_) {
            row = label(LV_LABEL_LONG_DOT);
            lv_obj_set_style_pad_hor(row, 4, 0);
            lv_obj_set_style_radius(row, 4, 0);
        }
        title_ = label();
        artist_ = label();
        progress_ = lv_bar_create(music_panel_);
        lv_obj_set_size(progress_, inner_width, 10);
        lv_bar_set_range(progress_, 0, 1000);
        time_ = label();
        mode_ = label();
        phase_ = label();
        help_ = label();

        // DJ screen: the two visualisers share the height left after the text lines.
        auto* theme = static_cast<LvglTheme*>(GetTheme());
        int line = LineHeight(theme);
        int inner_height = height_ - kStatusBarHeight - 2 * kPanelPadding;
        // title, info, wave, spectrum, progress(8), pads, help; six gaps.
        int box_height = std::clamp((inner_height - 4 * line - 8 - 6 * kRowGap - 4) / 2, 30, 90);
        dj_title_ = label();
        dj_info_ = label();
        auto make_box = [this, inner_width, box_height]() {
            auto* box = lv_obj_create(music_panel_);
            lv_obj_set_size(box, inner_width, box_height);
            lv_obj_set_style_pad_all(box, 0, 0);
            lv_obj_set_style_border_width(box, 0, 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
            lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            return box;
        };
        auto make_bar = [](lv_obj_t* parent, int x, int w, int h, lv_color_t color) {
            auto* bar = lv_bar_create(parent);
            lv_obj_set_pos(bar, x, 0);
            lv_obj_set_size(bar, w, h);
            lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 1, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
            return bar;
        };
        dj_wave_box_ = make_box();
        int wave_x0 = (inner_width - DjEngine::kWave * (kWaveBarWidth + kWaveBarGap)) / 2;
        for (int i = 0; i < DjEngine::kWave; ++i) {
            dj_wave_[i] = make_bar(dj_wave_box_, wave_x0 + i * (kWaveBarWidth + kWaveBarGap),
                                   kWaveBarWidth, box_height, lv_palette_main(LV_PALETTE_CYAN));
            // The envelope is drawn mirrored around the centre line.
            lv_bar_set_mode(dj_wave_[i], LV_BAR_MODE_RANGE);
            lv_bar_set_range(dj_wave_[i], -100, 100);
            lv_bar_set_start_value(dj_wave_[i], 0, LV_ANIM_OFF);
            lv_bar_set_value(dj_wave_[i], 0, LV_ANIM_OFF);
        }
        dj_spec_box_ = make_box();
        int spec_w = inner_width / DjEngine::kBands;
        for (int i = 0; i < DjEngine::kBands; ++i) {
            dj_spec_[i] = make_bar(dj_spec_box_, i * spec_w, spec_w - 3, box_height,
                                   lv_palette_main(i < 4    ? LV_PALETTE_PINK
                                                   : i < 8 ? LV_PALETTE_ORANGE
                                                           : LV_PALETTE_YELLOW));
            lv_bar_set_range(dj_spec_[i], 0, 100);
            lv_obj_set_y(dj_spec_[i], 0);
        }
        dj_progress_ = lv_bar_create(music_panel_);
        lv_obj_set_size(dj_progress_, inner_width, 8);
        lv_bar_set_range(dj_progress_, 0, 1000);
        dj_pad_row_ = lv_obj_create(music_panel_);
        lv_obj_set_size(dj_pad_row_, inner_width, line + 4);
        lv_obj_set_style_pad_all(dj_pad_row_, 0, 0);
        lv_obj_set_style_border_width(dj_pad_row_, 0, 0);
        lv_obj_set_style_bg_opa(dj_pad_row_, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(dj_pad_row_, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < 2; ++i) {
            dj_pad_[i] = lv_label_create(dj_pad_row_);
            lv_obj_set_size(dj_pad_[i], inner_width / 2 - 3, line);
            lv_obj_set_pos(dj_pad_[i], i * (inner_width / 2 + 3), 0);
            lv_label_set_long_mode(dj_pad_[i], LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(dj_pad_[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_radius(dj_pad_[i], 6, 0);
            lv_obj_set_style_border_width(dj_pad_[i], 2, 0);
        }
        dj_help_ = label();
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

    const bool files = view.screen == MusicScreen::Files;
    const bool playing_screen = view.screen == MusicScreen::NowPlaying;
    const bool dj = view.screen == MusicScreen::Dj;
    show(path_, files);
    for (int i = 0; i < kMaxListRows; ++i)
        show(rows_[i], files && i < visible_rows);
    for (auto* obj : {title_, artist_, progress_, time_, mode_})
        show(obj, playing_screen);
    show(phase_, !dj);
    show(help_, !dj);
    for (auto* obj : {dj_title_, dj_info_, dj_wave_box_, dj_spec_box_, dj_progress_, dj_pad_row_,
                      dj_help_})
        show(obj, dj);

    char line[128];
    if (dj) {
        const auto& d = view.dj;
        text(dj_title_, state.title.empty() ? "SD Music" : state.title);
        char bpm[16] = "--";
        if (d.bpm)
            snprintf(bpm, sizeof(bpm), "%d", d.bpm);
        snprintf(line, sizeof(line), "BPM %s   FX %s   Vol %d", bpm, d.fx.c_str(), volume);
        text(dj_info_, line);
        for (int i = 0; i < DjEngine::kWave; ++i) {
            lv_bar_set_start_value(dj_wave_[i], -int(d.wave[i]), LV_ANIM_OFF);
            lv_bar_set_value(dj_wave_[i], d.wave[i], LV_ANIM_OFF);
        }
        for (int i = 0; i < DjEngine::kBands; ++i)
            lv_bar_set_value(dj_spec_[i], d.bands[i], LV_ANIM_OFF);
        lv_bar_set_value(dj_progress_,
                         state.duration_ms ? uint64_t(state.position_ms) * 1000 / state.duration_ms
                                           : 0,
                         LV_ANIM_OFF);
        lv_obj_set_style_bg_color(dj_progress_, lv_palette_main(d.beat ? LV_PALETTE_RED : LV_PALETTE_CYAN),
                                  LV_PART_INDICATOR);
        for (int i = 0; i < 2; ++i) {
            snprintf(line, sizeof(line), "%s: %s", i == 0 ? "L" : "R", d.pads[i].c_str());
            text(dj_pad_[i], line);
            auto color = lv_palette_main(i == 0 ? LV_PALETTE_PINK : LV_PALETTE_ORANGE);
            lv_obj_set_style_border_color(dj_pad_[i], color, 0);
            lv_obj_set_style_bg_color(dj_pad_[i], color, 0);
            lv_obj_set_style_bg_opa(dj_pad_[i], d.pad_flash[i] ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        text(dj_help_, "L/R:pads  M:effect  hold R:bank  2xM:exit  hold L:files");
        return;
    }

    snprintf(line, sizeof(line), "%s  %u/%u  Vol %d", state.phase.c_str(),
             unsigned(state.total ? state.index + 1 : 0), unsigned(state.total), volume);
    text(phase_, state.error.empty() ? std::string(line) : state.error);

    if (files) {
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
    text(help_, "L/R:vol  2xL/R:skip  M:pause  2xM:exit  hold L:DJ");
}

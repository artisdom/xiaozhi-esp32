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
    if (!music_panel_ && !dj_panel_ && !visible)
        return;
    const bool dj_screen = visible && view.screen == MusicScreen::Dj;
    if (dj_screen) {
        // The DJ screen covers the whole display, status bar included.
        if (music_panel_)
            lv_obj_add_flag(music_panel_, LV_OBJ_FLAG_HIDDEN);
        if (!dj_panel_)
            CreateDjPanel();
        lv_obj_remove_flag(dj_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(dj_panel_);
        UpdateDjPanel(state, volume, view.dj);
        return;
    }
    if (dj_panel_)
        lv_obj_add_flag(dj_panel_, LV_OBJ_FLAG_HIDDEN);
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
    show(path_, files);
    for (int i = 0; i < kMaxListRows; ++i)
        show(rows_[i], files && i < visible_rows);
    for (auto* obj : {title_, artist_, progress_, time_, mode_})
        show(obj, playing_screen);
    show(phase_, true);
    show(help_, true);

    char line[128];
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
        text(help_, "L/R:move  M:open/play  2xM:player  hold L/R:vol  Q:up/exit");
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
    text(help_, "L/R:prev/next  M:pause  2xM:DJ  hold L/R:vol  Q:back");
}

namespace {
constexpr lv_palette_t kPadPalette[DjEngine::kPadCount] = {
    LV_PALETTE_PINK,  LV_PALETTE_ORANGE, LV_PALETTE_YELLOW, LV_PALETTE_GREEN,
    LV_PALETTE_CYAN,  LV_PALETTE_BLUE,   LV_PALETTE_PURPLE, LV_PALETTE_RED};
}  // namespace

void MusicDisplay::CreateDjPanel() {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    const int line = theme ? lv_font_get_line_height(theme->text_font()->font()) : 16;
    constexpr int kPad = 6, kGap = 4;
    const int inner_w = width_ - 2 * kPad;
    const int pad_h = line + 10, fx_h = line + 6;
    // title, info, wave, spectrum, progress(8), pad grid (2 rows), effects, help.
    const int fixed = 3 * line + 8 + 2 * pad_h + kGap + fx_h;
    const int box_h = std::clamp((height_ - 2 * kPad - 7 * kGap - fixed) / 2, 24, 80);

    dj_panel_ = lv_obj_create(lv_display_get_screen_active(display_));
    lv_obj_set_size(dj_panel_, width_, height_);
    lv_obj_align(dj_panel_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(dj_panel_, 0, 0);
    lv_obj_set_style_border_width(dj_panel_, 0, 0);
    lv_obj_set_style_pad_all(dj_panel_, kPad, 0);
    lv_obj_set_style_pad_row(dj_panel_, kGap, 0);
    lv_obj_set_flex_flow(dj_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(dj_panel_, LV_OBJ_FLAG_SCROLLABLE);

    auto label = [this, inner_w]() {
        auto* obj = lv_label_create(dj_panel_);
        lv_obj_set_width(obj, inner_w);
        lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
        return obj;
    };
    auto box = [this, inner_w](int h) {
        auto* obj = lv_obj_create(dj_panel_);
        lv_obj_set_size(obj, inner_w, h);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    };
    auto bar = [](lv_obj_t* parent, int x, int w, int h, lv_color_t color) {
        auto* b = lv_bar_create(parent);
        lv_obj_set_pos(b, x, 0);
        lv_obj_set_size(b, w, h);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(b, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(b, 1, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(b, color, LV_PART_INDICATOR);
        return b;
    };
    // A rounded, labelled cell used for pads and effects.
    auto cell = [](lv_obj_t* parent, int x, int y, int w, int h, lv_obj_t** text) {
        auto* c = lv_obj_create(parent);
        lv_obj_set_pos(c, x, y);
        lv_obj_set_size(c, w, h);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        *text = lv_label_create(c);
        lv_label_set_long_mode(*text, LV_LABEL_LONG_CLIP);
        lv_obj_center(*text);
        return c;
    };

    dj_title_ = label();
    dj_info_ = label();
    auto* wave_box = box(box_h);
    const int wave_x0 = (inner_w - DjEngine::kWave * (kWaveBarWidth + kWaveBarGap)) / 2;
    for (int i = 0; i < DjEngine::kWave; ++i) {
        dj_wave_[i] = bar(wave_box, wave_x0 + i * (kWaveBarWidth + kWaveBarGap), kWaveBarWidth,
                          box_h, lv_palette_main(LV_PALETTE_CYAN));
        // The envelope is drawn mirrored around the centre line.
        lv_bar_set_mode(dj_wave_[i], LV_BAR_MODE_RANGE);
        lv_bar_set_range(dj_wave_[i], -100, 100);
        lv_bar_set_start_value(dj_wave_[i], 0, LV_ANIM_OFF);
        lv_bar_set_value(dj_wave_[i], 0, LV_ANIM_OFF);
    }
    auto* spec_box = box(box_h);
    const int spec_w = inner_w / DjEngine::kBands;
    for (int i = 0; i < DjEngine::kBands; ++i) {
        dj_spec_[i] = bar(spec_box, i * spec_w, spec_w - 3, box_h,
                          lv_palette_main(i < 4 ? LV_PALETTE_PINK
                                          : i < 8 ? LV_PALETTE_ORANGE
                                                  : LV_PALETTE_YELLOW));
        lv_bar_set_range(dj_spec_[i], 0, 100);
    }
    dj_progress_ = lv_bar_create(dj_panel_);
    lv_obj_set_size(dj_progress_, inner_w, 8);
    lv_bar_set_range(dj_progress_, 0, 1000);

    auto* pads = box(2 * pad_h + kGap);
    const int pad_w = (inner_w - (kDjPadColumns - 1) * kGap) / kDjPadColumns;
    for (int i = 0; i < DjEngine::kPadCount; ++i)
        dj_pad_[i] = cell(pads, (i % kDjPadColumns) * (pad_w + kGap),
                          (i / kDjPadColumns) * (pad_h + kGap), pad_w, pad_h, &dj_pad_label_[i]);
    auto* fx = box(fx_h);
    const int fx_w = (inner_w - (kDjFxItems - 1) * kGap) / kDjFxItems;
    for (int i = 0; i < kDjFxItems; ++i)
        dj_fx_[i] = cell(fx, i * (fx_w + kGap), 0, fx_w, fx_h, &dj_fx_label_[i]);
    dj_help_ = label();
}

void MusicDisplay::UpdateDjPanel(const MusicSnapshot& state, int volume, const MusicDjView& d) {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    lv_color_t text_color = theme ? theme->text_color() : lv_color_white();
    lv_color_t back_color = theme ? theme->background_color() : lv_color_black();
    if (theme) {
        lv_obj_set_style_bg_color(dj_panel_, back_color, 0);
        lv_obj_set_style_text_color(dj_panel_, text_color, 0);
        lv_obj_set_style_text_font(dj_panel_, theme->text_font()->font(), 0);
    }
    auto text = [](lv_obj_t* label, const std::string& value) {
        if (value != lv_label_get_text(label))
            lv_label_set_text(label, value.c_str());
    };
    text(dj_title_, state.title.empty() ? "SD Music" : state.title);
    char line[96], bpm[16] = "--";
    if (d.bpm)
        snprintf(bpm, sizeof(bpm), "%d", d.bpm);
    snprintf(line, sizeof(line), "BPM %s   FX %s   Vol %d", bpm, DjEngine::FxName(d.fx), volume);
    text(dj_info_, line);
    for (int i = 0; i < DjEngine::kWave; ++i) {
        lv_bar_set_start_value(dj_wave_[i], -int(d.wave[i]), LV_ANIM_OFF);
        lv_bar_set_value(dj_wave_[i], d.wave[i], LV_ANIM_OFF);
    }
    for (int i = 0; i < DjEngine::kBands; ++i)
        lv_bar_set_value(dj_spec_[i], d.bands[i], LV_ANIM_OFF);
    lv_bar_set_value(dj_progress_,
                     state.duration_ms ? uint64_t(state.position_ms) * 1000 / state.duration_ms : 0,
                     LV_ANIM_OFF);
    lv_obj_set_style_bg_color(dj_progress_,
                              lv_palette_main(d.beat ? LV_PALETTE_RED : LV_PALETTE_CYAN),
                              LV_PART_INDICATOR);

    // Items: a filled cell is triggered/active; a thick light border is the cursor.
    auto style_cell = [&](lv_obj_t* cell, lv_obj_t* label, lv_color_t color, bool filled,
                          bool selected) {
        lv_obj_set_style_bg_color(cell, color, 0);
        lv_obj_set_style_bg_opa(cell, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(cell, selected ? text_color : color, 0);
        lv_obj_set_style_border_width(cell, selected ? 4 : 2, 0);
        lv_obj_set_style_text_color(label, filled ? back_color : text_color, 0);
    };
    for (int i = 0; i < DjEngine::kPadCount; ++i) {
        text(dj_pad_label_[i], DjEngine::PadName(i));
        style_cell(dj_pad_[i], dj_pad_label_[i], lv_palette_main(kPadPalette[i]),
                   d.pad_flash & (1u << i), d.selected == i);
        lv_obj_center(dj_pad_label_[i]);
    }
    for (int i = 0; i < kDjFxItems; ++i) {
        text(dj_fx_label_[i], DjEngine::FxName(i + 1));
        style_cell(dj_fx_[i], dj_fx_label_[i], lv_palette_main(LV_PALETTE_TEAL), d.fx == i + 1,
                   d.selected == DjEngine::kPadCount + i);
        lv_obj_center(dj_fx_label_[i]);
    }
    text(dj_help_, "L/R:select  2xL/R:row  M:fire/toggle  hold L/R:vol  Q:back");
}

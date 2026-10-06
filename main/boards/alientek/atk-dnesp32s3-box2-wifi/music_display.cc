#include "music_display.h"

#include <algorithm>
#include <cstdio>

#include "assets.h"
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
    if (!music_panel_ && !dj_panel_ && !lyrics_panel_ && !visible)
        return;
    // Now playing (lyrics) and the DJ screen cover the whole display, status bar included.
    const bool dj_screen = visible && view.screen == MusicScreen::Dj;
    const bool lyrics_screen = visible && view.screen == MusicScreen::NowPlaying;
    if (dj_screen || lyrics_screen) {
        if (music_panel_)
            lv_obj_add_flag(music_panel_, LV_OBJ_FLAG_HIDDEN);
        if (dj_screen) {
            if (lyrics_panel_)
                lv_obj_add_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
            if (!dj_panel_)
                CreateDjPanel();
            lv_obj_remove_flag(dj_panel_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(dj_panel_);
            UpdateDjPanel(state, volume, view.dj);
        } else {
            if (dj_panel_)
                lv_obj_add_flag(dj_panel_, LV_OBJ_FLAG_HIDDEN);
            if (!lyrics_panel_)
                CreateLyricsPanel();
            lv_obj_remove_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(lyrics_panel_);
            UpdateNowPlaying(state);
        }
        return;
    }
    if (dj_panel_)
        lv_obj_add_flag(dj_panel_, LV_OBJ_FLAG_HIDDEN);
    if (lyrics_panel_)
        lv_obj_add_flag(lyrics_panel_, LV_OBJ_FLAG_HIDDEN);
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
    show(path_, files);
    for (int i = 0; i < kMaxListRows; ++i)
        show(rows_[i], files && i < visible_rows);
    show(phase_, files);
    show(help_, files);

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
    // title, info, wave, spectrum, progress(8), pad grid (2 rows), effect grid (2 rows).
    const int fixed = 2 * line + 8 + (2 * pad_h + kGap) + (2 * fx_h + kGap);
    const int box_h = std::clamp((height_ - 2 * kPad - 6 * kGap - fixed) / 2, 24, 80);

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
    auto* fx = box(2 * fx_h + kGap);
    for (int i = 0; i < kDjFxItems; ++i)
        dj_fx_[i] = cell(fx, (i % kDjPadColumns) * (pad_w + kGap),
                         (i / kDjPadColumns) * (fx_h + kGap), pad_w, fx_h, &dj_fx_label_[i]);
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
}

namespace {
// Colours of the lyric words: sung, being sung, still to come; and other lines.
struct LyricColors {
    lv_color_t sung, current, upcoming, dim;
};
LyricColors MakeLyricColors(LvglTheme* theme) {
    lv_color_t text = theme ? theme->text_color() : lv_color_white();
    lv_color_t back = theme ? theme->background_color() : lv_color_black();
    return {lv_palette_main(LV_PALETTE_GREEN), lv_palette_main(LV_PALETTE_ORANGE), text,
            lv_color_mix(text, back, 110)};
}
}  // namespace

const lv_font_t* MusicDisplay::LyricsFont() {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    const lv_font_t* ui_font = theme ? theme->text_font()->font() : nullptr;
    if (!lyric_font_tried_) {
        lyric_font_tried_ = true;
        void* data = nullptr;
        size_t size = 0;
        if (Assets::GetInstance().GetAssetData("font_noto_sans_common_30_4.bin", data, size)) {
            auto font = std::make_shared<LvglCBinFont>(data);
            if (font->font()) {
                font->SetFallback(ui_font);  // Anything the large font lacks uses the UI font.
                lyric_font_ = font;
            }
        }
    }
    return lyric_font_ ? lyric_font_->font() : ui_font;
}

void MusicDisplay::CreateLyricsPanel() {
    constexpr int kPad = 8;
    const int inner_width = width_ - 2 * kPad;
    lyrics_panel_ = lv_obj_create(lv_display_get_screen_active(display_));
    lv_obj_set_size(lyrics_panel_, width_, height_);
    lv_obj_align(lyrics_panel_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(lyrics_panel_, 0, 0);
    lv_obj_set_style_border_width(lyrics_panel_, 0, 0);
    lv_obj_set_style_pad_all(lyrics_panel_, kPad, 0);
    lv_obj_remove_flag(lyrics_panel_, LV_OBJ_FLAG_SCROLLABLE);

    // The lines sit centred in a box that fills the screen.
    lyrics_box_ = lv_obj_create(lyrics_panel_);
    lv_obj_set_size(lyrics_box_, inner_width, height_ - 2 * kPad);
    lv_obj_set_style_pad_all(lyrics_box_, 0, 0);
    lv_obj_set_style_pad_row(lyrics_box_, 18, 0);
    lv_obj_set_style_border_width(lyrics_box_, 0, 0);
    lv_obj_set_style_bg_opa(lyrics_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(lyrics_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lyrics_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(lyrics_box_, LV_OBJ_FLAG_SCROLLABLE);
    for (int r = 0; r < kLyricRows; ++r) {
        lyric_rows_[r] = lv_spangroup_create(lyrics_box_);
        lv_obj_set_width(lyric_rows_[r], inner_width);
        lv_obj_set_height(lyric_rows_[r], LV_SIZE_CONTENT);
        lv_spangroup_set_mode(lyric_rows_[r], LV_SPAN_MODE_BREAK);
        lv_spangroup_set_align(lyric_rows_[r], LV_TEXT_ALIGN_CENTER);
        lv_obj_add_flag(lyric_rows_[r], LV_OBJ_FLAG_HIDDEN);
        lyric_row_line_[r] = -1;
        lyric_row_word_[r] = -3;
        lyric_row_height_[r] = 0;
        lyric_row_shown_[r] = false;
    }
    no_lyrics_ = lv_label_create(lyrics_box_);
    lv_obj_set_width(no_lyrics_, inner_width);
    lv_label_set_long_mode(no_lyrics_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(no_lyrics_, LV_TEXT_ALIGN_CENTER, 0);
}

// Show line `line_index` of the lyrics in a row; `word` is the highlighted word of the current
// line (-1: none yet). Spans are only rebuilt when the row changes line; recolouring is cheap.
// Visibility is decided by the caller, which knows how much of the screen the lines need.
void MusicDisplay::ShowLyricRow(int row, const box2_music::LyricLine* line, int line_index,
                                bool current, int word) {
    lv_obj_t* group = lyric_rows_[row];
    if (!line) {
        lyric_row_line_[row] = -1;
        lyric_row_height_[row] = 0;
        return;
    }
    const int state_key = current ? word : -3;  // -3: not the current line.
    if (lyric_row_line_[row] == line_index && lyric_row_word_[row] == state_key)
        return;
    if (lyric_row_line_[row] != line_index) {
        while (lv_spangroup_get_span_count(group))
            lv_spangroup_delete_span(group, lv_spangroup_get_child(group, 0));
        for (const auto& w : line->words) {
            lv_span_t* span = lv_spangroup_add_span(group);
            lv_span_set_text(span, (w.text + (w.space_after ? " " : "")).c_str());
        }
        lyric_row_line_[row] = line_index;
    }
    const LyricColors colors = MakeLyricColors(static_cast<LvglTheme*>(GetTheme()));
    for (size_t i = 0; i < line->words.size(); ++i) {
        lv_style_t* style = lv_span_get_style(lv_spangroup_get_child(group, int(i)));
        lv_color_t color = colors.dim;
        lv_text_decor_t decor = LV_TEXT_DECOR_NONE;
        if (current) {
            color = int(i) < word ? colors.sung : colors.upcoming;
            if (int(i) == word) {
                color = colors.current;
                decor = LV_TEXT_DECOR_UNDERLINE;
            }
        }
        lv_style_set_text_color(style, color);
        lv_style_set_text_decor(style, decor);
    }
    lv_spangroup_refresh(group);
    lyric_row_height_[row] = lv_spangroup_get_expand_height(group, lv_obj_get_width(group));
    lyric_row_word_[row] = state_key;
}

void MusicDisplay::UpdateNowPlaying(const MusicSnapshot& state) {
    auto* theme = static_cast<LvglTheme*>(GetTheme());
    if (theme) {
        lv_obj_set_style_bg_color(lyrics_panel_, theme->background_color(), 0);
        lv_obj_set_style_text_color(lyrics_panel_, theme->text_color(), 0);
    }
    const lv_font_t* font = LyricsFont();
    if (font && lv_obj_get_style_text_font(lyrics_panel_, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(lyrics_panel_, font, 0);
        for (int r = 0; r < kLyricRows; ++r)
            lyric_row_line_[r] = -1;  // Rebuild so the spans are measured with the new font.
    }
    if (lyric_source_ != state.lyrics) {  // New track (or lyrics reloaded): start from scratch.
        lyric_source_ = state.lyrics;
        for (int r = 0; r < kLyricRows; ++r)
            lyric_row_line_[r] = -1, lyric_row_word_[r] = -3;
    }
    if (!lyric_source_) {
        // Nothing to sing along to: show the song title so the screen is not blank.
        for (int r = 0; r < kLyricRows; ++r) {
            ShowLyricRow(r, nullptr, -1, false, -1);
            if (lyric_row_shown_[r]) {
                lv_obj_add_flag(lyric_rows_[r], LV_OBJ_FLAG_HIDDEN);
                lyric_row_shown_[r] = false;
            }
        }
        lv_obj_remove_flag(no_lyrics_, LV_OBJ_FLAG_HIDDEN);
        std::string title = state.title.empty() ? "SD Music" : state.title;
        if (title != lv_label_get_text(no_lyrics_))
            lv_label_set_text(no_lyrics_, title.c_str());
        return;
    }
    lv_obj_add_flag(no_lyrics_, LV_OBJ_FLAG_HIDDEN);
    const auto& lines = lyric_source_->lines;
    const int current = lyric_source_->LineAt(state.position_ms);
    // The previous line stays visible above the current one; before the first line, show the
    // opening lines of the song as "coming up".
    const int first = current > 0 ? current - 1 : 0;
    int primary = 0;
    for (int r = 0; r < kLyricRows; ++r) {
        int index = first + r;
        if (index >= int(lines.size())) {
            ShowLyricRow(r, nullptr, -1, false, -1);
            continue;
        }
        const bool is_current = index == current;
        if (is_current)
            primary = r;
        ShowLyricRow(r, &lines[index], index, is_current,
                     is_current ? lines[index].WordAt(state.position_ms) : -1);
    }
    // The large font fills the screen: always show the current line, then the next one, then
    // the previous one, as long as they fit.
    const int budget = lv_obj_get_height(lyrics_box_);
    const int gap = lv_obj_get_style_pad_row(lyrics_box_, LV_PART_MAIN);
    bool wanted[kLyricRows] = {};
    int used = 0;
    for (int candidate : {primary, primary + 1, primary - 1, primary + 2}) {
        if (candidate < 0 || candidate >= kLyricRows || lyric_row_line_[candidate] < 0)
            continue;
        int needed = lyric_row_height_[candidate] + (used ? gap : 0);
        if (used && used + needed > budget)
            continue;
        used += needed;
        wanted[candidate] = true;
    }
    for (int r = 0; r < kLyricRows; ++r) {
        if (wanted[r] == lyric_row_shown_[r])
            continue;
        if (wanted[r])
            lv_obj_remove_flag(lyric_rows_[r], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(lyric_rows_[r], LV_OBJ_FLAG_HIDDEN);
        lyric_row_shown_[r] = wanted[r];
    }
}

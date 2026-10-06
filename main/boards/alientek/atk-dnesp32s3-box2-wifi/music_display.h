#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dj_engine.h"
#include "display/lcd_display.h"
#include "sd_mp3_player.h"

// One line of the folder browser.
struct MusicListRow {
    std::string text;
    bool selected = false;  // Browse cursor.
    bool playing = false;   // Currently loaded track.
};

enum class MusicScreen { Files, NowPlaying, Dj };

// Live values for the DJ screen, already smoothed by the caller.
struct MusicDjView {
    uint8_t bands[box2_music::DjEngine::kBands] = {};
    uint8_t wave[box2_music::DjEngine::kWave] = {};
    int bpm = 0;
    int fx = 0;            // Active effect (DjEngine::Fx).
    int selected = 0;      // Highlighted item: pads first, then the effects (see below).
    uint32_t pad_flash = 0;  // Bit per pad that was just triggered.
    bool beat = false;
};

// What the player screen shows besides the playback state.
struct MusicBrowseView {
    MusicScreen screen = MusicScreen::Files;
    std::string path;  // Files: current folder, "" for the card root.
    std::vector<MusicListRow> rows;
    MusicDjView dj;
};

class MusicDisplay : public SpiLcdDisplay {
public:
    static constexpr int kDjFxItems = box2_music::DjEngine::kFxCount - 1;  // Without OFF.
    static constexpr int kMaxListRows = 10;
    // DJ items are numbered pads 0..kPadCount-1, then effects kPadCount.. (LPF, HPF, ...).
    static constexpr int kDjItems = box2_music::DjEngine::kPadCount + kDjFxItems;
    static constexpr int kDjPadColumns = 4;

    using SpiLcdDisplay::SpiLcdDisplay;
    void UpdateMusic(const MusicSnapshot& state, bool visible, int volume,
                     const MusicBrowseView& view = {});
    // How many browser rows fit on the screen with the current theme font.
    int VisibleListRows();

private:
    lv_obj_t* music_panel_ = nullptr;
    lv_obj_t* path_ = nullptr;
    lv_obj_t* rows_[kMaxListRows] = {};
    lv_obj_t* phase_ = nullptr;
    lv_obj_t* help_ = nullptr;
    // Now playing: title, scrolling lyrics, mini visualiser, progress and a time/BPM line.
    static constexpr int kLyricRows = 5;
    static constexpr int kMiniWave = 24;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* lyrics_box_ = nullptr;
    lv_obj_t* lyric_rows_[kLyricRows] = {};
    lv_obj_t* no_lyrics_ = nullptr;
    lv_obj_t* np_visual_ = nullptr;
    lv_obj_t* np_wave_[kMiniWave] = {};
    lv_obj_t* np_spec_[box2_music::DjEngine::kBands] = {};
    lv_obj_t* progress_ = nullptr;
    lv_obj_t* np_info_ = nullptr;
    lv_obj_t* np_time_ = nullptr;
    lv_obj_t* np_meta_ = nullptr;
    // Which lyric line/word each row currently shows, so spans are only rebuilt on change.
    std::shared_ptr<const box2_music::Lyrics> lyric_source_;
    int lyric_row_line_[kLyricRows] = {};
    int lyric_row_word_[kLyricRows] = {};
    void CreateNowPlayingWidgets(int inner_width);
    void UpdateNowPlaying(const MusicSnapshot& state, int volume, const MusicDjView& visual);
    void UpdateLyrics(const MusicSnapshot& state);
    void ShowLyricRow(int row, const box2_music::LyricLine* line, int line_index, bool current,
                      int word);

    // DJ screen: a full-screen panel of its own.
    lv_obj_t* dj_panel_ = nullptr;
    lv_obj_t* dj_title_ = nullptr;
    lv_obj_t* dj_info_ = nullptr;
    lv_obj_t* dj_wave_[box2_music::DjEngine::kWave] = {};
    lv_obj_t* dj_spec_[box2_music::DjEngine::kBands] = {};
    lv_obj_t* dj_progress_ = nullptr;
    lv_obj_t* dj_pad_[box2_music::DjEngine::kPadCount] = {};
    lv_obj_t* dj_pad_label_[box2_music::DjEngine::kPadCount] = {};
    lv_obj_t* dj_fx_[kDjFxItems] = {};
    lv_obj_t* dj_fx_label_[kDjFxItems] = {};
    void CreateDjPanel();
    void UpdateDjPanel(const MusicSnapshot& state, int volume, const MusicDjView& view);
};

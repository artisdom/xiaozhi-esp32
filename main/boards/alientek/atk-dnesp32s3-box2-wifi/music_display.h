#pragma once

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
    std::string fx;       // Active effect name.
    std::string pads[2];  // Sampler pads on L and R in the current bank.
    bool pad_flash[2] = {false, false};
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
    static constexpr int kMaxListRows = 10;

    using SpiLcdDisplay::SpiLcdDisplay;
    void UpdateMusic(const MusicSnapshot& state, bool visible, int volume,
                     const MusicBrowseView& view = {});
    // How many browser rows fit on the screen with the current theme font.
    int VisibleListRows();

private:
    lv_obj_t* music_panel_ = nullptr;
    lv_obj_t* path_ = nullptr;
    lv_obj_t* rows_[kMaxListRows] = {};
    lv_obj_t* title_ = nullptr;
    lv_obj_t* artist_ = nullptr;
    lv_obj_t* progress_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* mode_ = nullptr;
    lv_obj_t* phase_ = nullptr;
    lv_obj_t* help_ = nullptr;
    // DJ screen.
    lv_obj_t* dj_title_ = nullptr;
    lv_obj_t* dj_info_ = nullptr;
    lv_obj_t* dj_wave_box_ = nullptr;
    lv_obj_t* dj_wave_[box2_music::DjEngine::kWave] = {};
    lv_obj_t* dj_spec_box_ = nullptr;
    lv_obj_t* dj_spec_[box2_music::DjEngine::kBands] = {};
    lv_obj_t* dj_progress_ = nullptr;
    lv_obj_t* dj_pad_row_ = nullptr;
    lv_obj_t* dj_pad_[2] = {};
    lv_obj_t* dj_help_ = nullptr;
};

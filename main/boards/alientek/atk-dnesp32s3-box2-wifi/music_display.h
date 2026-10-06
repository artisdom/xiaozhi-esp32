#pragma once

#include <string>
#include <vector>

#include "display/lcd_display.h"
#include "sd_mp3_player.h"

// One line of the track browser shown below the now-playing block.
struct MusicListRow {
    std::string text;
    bool selected = false;  // Browse cursor.
    bool playing = false;   // Currently loaded track.
};

class MusicDisplay : public SpiLcdDisplay {
public:
    static constexpr int kListRows = 5;

    using SpiLcdDisplay::SpiLcdDisplay;
    void UpdateMusic(const MusicSnapshot& state, bool visible, int volume,
                     const std::vector<MusicListRow>& rows = {});

private:
    lv_obj_t* music_panel_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* artist_ = nullptr;
    lv_obj_t* progress_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* phase_ = nullptr;
    lv_obj_t* rows_[kListRows] = {};
    lv_obj_t* help_ = nullptr;
};

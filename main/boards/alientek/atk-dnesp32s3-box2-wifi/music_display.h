#pragma once

#include <string>
#include <vector>

#include "display/lcd_display.h"
#include "sd_mp3_player.h"

// One line of the folder browser.
struct MusicListRow {
    std::string text;
    bool selected = false;  // Browse cursor.
    bool playing = false;   // Currently loaded track.
};

// What the player screen shows besides the playback state.
struct MusicBrowseView {
    bool browsing = false;  // true: folder browser, false: now playing.
    std::string path;       // Current folder, "" for the card root.
    std::vector<MusicListRow> rows;
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
};

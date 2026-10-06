#pragma once

#include "display/lcd_display.h"
#include "sd_mp3_player.h"

class MusicDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    void UpdateMusic(const MusicSnapshot& state, bool visible, int volume);

private:
    lv_obj_t* music_panel_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* artist_ = nullptr;
    lv_obj_t* album_ = nullptr;
    lv_obj_t* progress_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* mode_ = nullptr;
    lv_obj_t* phase_ = nullptr;
    lv_obj_t* help_ = nullptr;
};

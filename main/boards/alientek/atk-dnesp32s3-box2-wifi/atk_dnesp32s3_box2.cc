#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "codecs/es8389_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "led/single_led.h"
#include "music_display.h"
#include "power_manager.h"
#include "power_save_timer.h"
#include "sd_mp3_player.h"
#include "system_reset.h"
#include "wifi_board.h"

#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include "i2c_device.h"

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include "esp_io_expander_tca95xx_16bit.h"
#include <esp_system.h>
#include <esp_timer.h>
#include "mp3_utils.h"
#include "music_browser.h"
#include <array>
#include <algorithm>
#include <vector>

#define TAG "atk_dnesp32s3_box2_wifi"

class atk_dnesp32s3_box2_wifi : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    MusicDisplay* display_;
    esp_io_expander_handle_t io_exp_handle;
    button_handle_t btns;
    button_driver_t* btn_driver_ = nullptr;
    static atk_dnesp32s3_box2_wifi* instance_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    PowerSupply power_status_;
    esp_timer_handle_t wake_timer_handle_;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    int ticks_ = 0;
    const int kChgCtrlInterval = 5;
    SdMp3Player music_player_;
    bool music_view_ = false;
    MusicScreen music_screen_ = MusicScreen::Files;  // Which player screen is shown.
    int64_t music_last_input_us_ = 0;                // Last button press, for the DJ auto-enter.
    // DJ screen.
    int dj_cursor_ = 0;  // Selected DJ item: pads first, then effects.
    std::atomic<int64_t> dj_pad_time_us_[box2_music::DjEngine::kPadCount] = {};
    int64_t last_volume_hold_us_ = 0;
    std::atomic<bool> music_refresh_pending_{false};  // A periodic refresh is queued.
    uint8_t q_idle_level_ = 1;  // IO-expander level of the Q key while released.
    uint32_t dj_last_frames_ = 0, dj_last_beats_ = 0;
    int dj_idle_ticks_ = 0, dj_beat_ticks_ = 0;
    std::array<float, box2_music::DjEngine::kBands> dj_bands_ = {};
    static constexpr int64_t kDjIdleUs = 3000000;  // Now playing -> DJ screen after 3 s.
    std::string music_dir_;       // Folder shown in the browser, relative to the card root.
    std::vector<box2_music::BrowseEntry> browse_;
    size_t browse_cursor_ = 0;
    size_t browse_top_ = 0;  // First visible browser row.
    size_t browse_total_ = static_cast<size_t>(-1);  // Library size browse_ was built for.

    void InitializeBoardPowerManager() {
        instance_ = this;

        if (IoExpanderGetLevel(XIO_CHRG) == 0) {
            power_status_ = kDeviceTypecSupply;
        } else {
            power_status_ = kDeviceBatterySupply;
        }

        esp_timer_create_args_t wake_display_timer_args = {
            .callback =
                [](void* arg) {
                    atk_dnesp32s3_box2_wifi* self = static_cast<atk_dnesp32s3_box2_wifi*>(arg);

                    self->ticks_++;
                    // The DJ screen animates at 5 fps, the other screens refresh once a
                    // second. Never queue a refresh while the last one is still waiting: a
                    // redraw can take longer than the tick, and a backlog in the main loop
                    // delays button handling by many seconds.
                    if ((self->ticks_ % 10 == 0 ||
                         (self->music_view_ && self->music_screen_ == MusicScreen::Dj &&
                          self->ticks_ % 2 == 0)) &&
                        !self->music_refresh_pending_.exchange(true)) {
                        Application::GetInstance().Schedule([self]() {
                            auto state = self->music_player_.Snapshot();
                            self->AutoEnterDj(state);
                            bool idle =
                                Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
                            if (state.busy && state.phase != "paused")
                                self->power_save_timer_->WakeUp();
                            self->RefreshMusicView(
                                idle && (self->music_view_ || state.phase == "playing" ||
                                         state.phase == "indexing" || state.phase == "waiting" ||
                                         state.phase == "paused"));
                            self->music_refresh_pending_.store(false);
                        });
                    }
                    if (self->ticks_ % self->kChgCtrlInterval == 0) {
                        if (self->IoExpanderGetLevel(XIO_CHRG) == 0) {
                            self->power_status_ = kDeviceTypecSupply;
                        } else {
                            self->power_status_ = kDeviceBatterySupply;
                        }

                        /* 低于某个电量，会自动关机 */
                        if (self->power_manager_->low_voltage_ < 2630 &&
                            self->power_status_ == kDeviceBatterySupply) {
                            esp_timer_stop(self->power_manager_->timer_handle_);

                            esp_io_expander_set_dir(self->io_exp_handle, XIO_CHG_CTRL,
                                                    IO_EXPANDER_OUTPUT);
                            esp_io_expander_set_level(self->io_exp_handle, XIO_CHG_CTRL, 0);
                            vTaskDelay(pdMS_TO_TICKS(100));

                            esp_io_expander_set_dir(self->io_exp_handle, XIO_CHG_CTRL,
                                                    IO_EXPANDER_INPUT);
                            esp_io_expander_set_level(self->io_exp_handle, XIO_CHG_CTRL, 0);
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    }
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wake_update_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&wake_display_timer_args, &wake_timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(wake_timer_handle_, 100000));
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(io_exp_handle);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            if (power_status_ == kDeviceBatterySupply) {
                GetBacklight()->SetBrightness(0);
                esp_timer_stop(power_manager_->timer_handle_);
                esp_io_expander_set_dir(io_exp_handle, XIO_CHG_CTRL, IO_EXPANDER_OUTPUT);
                esp_io_expander_set_level(io_exp_handle, XIO_CHG_CTRL, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_io_expander_set_level(io_exp_handle, XIO_SYS_POW, 0);
            }
        });

        power_save_timer_->SetEnabled(true);
    }

    void audio_volume_change(bool direction) {
        auto codec = GetAudioCodec();
        auto volume = codec->output_volume();

        if (direction) {
            volume += 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
        } else {
            volume -= 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
        }
        GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
    }

    void audio_volume_minimum() {
        GetAudioCodec()->SetOutputVolume(0);
        GetDisplay()->ShowNotification(Lang::Strings::MUTED);
    }

    void audio_volume_maxmum() {
        GetAudioCodec()->SetOutputVolume(100);
        GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
    }

    esp_err_t IoExpanderSetLevel(uint16_t pin_mask, uint8_t level) {
        return esp_io_expander_set_level(io_exp_handle, pin_mask, level);
    }

    uint8_t IoExpanderGetLevel(uint16_t pin_mask) {
        uint32_t pin_val = 0;
        esp_io_expander_get_level(io_exp_handle, DRV_IO_EXP_INPUT_MASK, &pin_val);
        pin_mask &= DRV_IO_EXP_INPUT_MASK;
        return (uint8_t)((pin_val & pin_mask) ? 1 : 0);
    }

    void InitializeIoExpander() {
        esp_err_t ret = ESP_OK;
        esp_io_expander_new_i2c_tca95xx_16bit(i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000,
                                              &io_exp_handle);

        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_OUTPUT_MASK, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_dir(io_exp_handle, DRV_IO_EXP_INPUT_MASK, IO_EXPANDER_INPUT);

        ret |= esp_io_expander_set_level(io_exp_handle, XIO_SYS_POW, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_EN_3V3A, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_EN_4G, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_SPK_EN, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_USB_SEL, 1);
        ret |= esp_io_expander_set_level(io_exp_handle, XIO_VBUS_EN, 0);

        assert(ret == ESP_OK);
    }

    // Initialize I2C peripheral
    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    // Rebuild the folder listing; keep the cursor on `select_path` when it is present.
    void RebuildBrowse(const std::string& select_path = "") {
        auto state = music_player_.Snapshot();
        browse_ = box2_music::ListDirectory(music_player_.TrackNames(0, state.total), music_dir_);
        browse_total_ = state.total;
        browse_cursor_ = browse_top_ = 0;
        for (size_t i = 0; i < browse_.size(); ++i)
            if (!select_path.empty() && browse_[i].path == select_path)
                browse_cursor_ = i;
        // A folder can disappear after a library refresh; fall back towards the root.
        if (browse_.empty() || (browse_.size() == 1 && !music_dir_.empty() && !state.total)) {
            music_dir_.clear();
            browse_ = box2_music::ListDirectory(music_player_.TrackNames(0, state.total), "");
        }
    }

    // Smooth the engine's analysis into what the DJ screen draws.
    MusicDjView BuildDjView() {
        using box2_music::DjEngine;
        MusicDjView v;
        auto snap = music_player_.Dj().GetSnapshot();
        dj_idle_ticks_ = snap.frames != dj_last_frames_ ? 0 : dj_idle_ticks_ + 1;
        dj_last_frames_ = snap.frames;
        const bool silent = dj_idle_ticks_ >= 3;  // No audio for 300 ms: let the bars fall.
        for (int i = 0; i < DjEngine::kBands; ++i) {
            float target = silent ? 0.0f : snap.bands[i];
            dj_bands_[i] = std::max(target, dj_bands_[i] * 0.8f);
            v.bands[i] = static_cast<uint8_t>(dj_bands_[i]);
        }
        if (!silent)
            std::copy(std::begin(snap.wave), std::end(snap.wave), std::begin(v.wave));
        if (snap.beats != dj_last_beats_) {
            dj_last_beats_ = snap.beats;
            dj_beat_ticks_ = 2;
        }
        v.beat = dj_beat_ticks_ > 0;
        if (dj_beat_ticks_ > 0)
            --dj_beat_ticks_;
        v.bpm = snap.bpm;
        v.fx = music_player_.Dj().GetFx();
        v.selected = dj_cursor_;
        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < DjEngine::kPadCount; ++i)
            if (now - dj_pad_time_us_[i].load() < 150000)
                v.pad_flash |= 1u << i;
        return v;
    }

    // Now playing for 3 s without a button press switches to the DJ screen.
    void AutoEnterDj(const MusicSnapshot& state) {
        if (music_view_ && music_screen_ == MusicScreen::NowPlaying && state.phase == "playing" &&
            esp_timer_get_time() - music_last_input_us_ >= kDjIdleUs)
            music_screen_ = MusicScreen::Dj;
    }

    // Any press while the player is open: restart the idle timer.
    void MusicInput() { music_last_input_us_ = esp_timer_get_time(); }

    MusicBrowseView BuildBrowseView(const MusicSnapshot& state) {
        MusicBrowseView view;
        // Music started by voice shows now playing without opening the player.
        view.screen = music_view_ ? music_screen_ : MusicScreen::NowPlaying;
        view.path = music_dir_;
        if (view.screen == MusicScreen::Dj)
            view.dj = BuildDjView();
        if (view.screen != MusicScreen::Files)
            return view;
        if (state.total != browse_total_)
            RebuildBrowse();
        const size_t window = display_->VisibleListRows();
        browse_cursor_ = std::min(browse_cursor_, browse_.empty() ? 0 : browse_.size() - 1);
        if (browse_cursor_ < browse_top_)
            browse_top_ = browse_cursor_;
        else if (browse_cursor_ >= browse_top_ + window)
            browse_top_ = browse_cursor_ - window + 1;
        for (size_t i = browse_top_; i < std::min(browse_.size(), browse_top_ + window); ++i) {
            MusicListRow row;
            const auto& entry = browse_[i];
            row.text = entry.kind == box2_music::BrowseEntry::Kind::File ? entry.name
                                                                         : entry.name + "/";
            row.selected = i == browse_cursor_;
            row.playing = entry.kind == box2_music::BrowseEntry::Kind::File &&
                          entry.path == state.filename;
            view.rows.push_back(std::move(row));
        }
        return view;
    }

    void RefreshMusicView(bool visible) {
        auto state = music_player_.Snapshot();
        display_->UpdateMusic(state, visible, GetAudioCodec()->output_volume(),
                              visible ? BuildBrowseView(state) : MusicBrowseView());
    }

    // Open the player: browse the folder of the current track, or show now playing if busy.
    void OpenMusicView() {
        music_view_ = true;
        music_player_.ScanInBackground();
        auto state = music_player_.Snapshot();
        music_screen_ = (state.busy || state.phase == "paused") ? MusicScreen::NowPlaying
                                                                 : MusicScreen::Files;
        MusicInput();
        music_dir_ = box2_music::ParentDirectory(state.filename);
        RebuildBrowse(state.filename);
        RefreshMusicView(true);
    }

    void CloseMusicView() {
        music_player_.Stop();
        music_view_ = false;
        RefreshMusicView(false);
    }

    // M double-click: one screen forward (files -> now playing -> DJ).
    void ScreenForward() {
        MusicInput();
        if (music_screen_ == MusicScreen::Files)
            music_screen_ = MusicScreen::NowPlaying;
        else if (music_screen_ == MusicScreen::NowPlaying)
            music_screen_ = MusicScreen::Dj;
        RefreshMusicView(true);
    }

    // Q: DJ -> now playing -> files -> up through the folders -> exit.
    void ScreenBack() {
        MusicInput();
        if (music_screen_ == MusicScreen::Dj) {
            music_screen_ = MusicScreen::NowPlaying;
        } else if (music_screen_ == MusicScreen::NowPlaying) {
            music_screen_ = MusicScreen::Files;
            auto state = music_player_.Snapshot();
            music_dir_ = box2_music::ParentDirectory(state.filename);
            RebuildBrowse(state.filename);
        } else if (!music_dir_.empty()) {
            BrowseUp();
            return;
        } else {
            CloseMusicView();
            return;
        }
        RefreshMusicView(true);
    }

    // DJ screen: fire the highlighted pad or toggle the highlighted effect. Safe from any task.
    void DjActivate() {
        using box2_music::DjEngine;
        if (dj_cursor_ < DjEngine::kPadCount) {
            music_player_.Dj().TriggerPad(dj_cursor_);
            dj_pad_time_us_[dj_cursor_].store(esp_timer_get_time());
        } else {
            int fx = dj_cursor_ - DjEngine::kPadCount + 1;
            music_player_.Dj().SetFx(music_player_.Dj().GetFx() == fx ? DjEngine::kFxOff : fx);
        }
    }

    // Move through the DJ items in reading order (pads, then effects).
    void DjMove(int delta) {
        MusicInput();
        dj_cursor_ = (dj_cursor_ + MusicDisplay::kDjItems + delta) % MusicDisplay::kDjItems;
        RefreshMusicView(true);
    }

    // Move between the two pad rows and the effect row, keeping the column.
    void DjMoveRow(int direction) {
        MusicInput();
        using box2_music::DjEngine;
        constexpr int kPadRows = DjEngine::kPadCount / MusicDisplay::kDjPadColumns;
        const bool on_fx = dj_cursor_ >= DjEngine::kPadCount;
        int row = on_fx ? kPadRows : dj_cursor_ / MusicDisplay::kDjPadColumns;
        int column = on_fx ? std::min(dj_cursor_ - DjEngine::kPadCount, MusicDisplay::kDjPadColumns - 1)
                           : dj_cursor_ % MusicDisplay::kDjPadColumns;
        row = (row + direction + kPadRows + 1) % (kPadRows + 1);
        dj_cursor_ = row == kPadRows ? DjEngine::kPadCount + column
                                     : row * MusicDisplay::kDjPadColumns + column;
        RefreshMusicView(true);
    }

    // Hold L/R inside the player: volume down/up, repeating while held.
    void RegisterVolumeHold(button_handle_t handle, bool up) {
        struct Context {
            atk_dnesp32s3_box2_wifi* self;
            bool up;
        };
        iot_button_register_cb(
            handle, BUTTON_LONG_PRESS_HOLD, nullptr,
            [](void*, void* data) {
                auto* ctx = static_cast<Context*>(data);
                int64_t now = esp_timer_get_time();
                if (!ctx->self->music_view_ || now - ctx->self->last_volume_hold_us_ < 250000)
                    return;
                ctx->self->last_volume_hold_us_ = now;
                ctx->self->audio_volume_change(ctx->up);
            },
            new Context{this, up});  // Lives as long as the board.
    }

    void ShowNowPlayingScreen() {
        MusicInput();
        music_screen_ = MusicScreen::NowPlaying;
        RefreshMusicView(true);
    }

    void MoveBrowseCursor(int direction) {
        if (browse_.empty())
            return;
        browse_cursor_ = (browse_cursor_ + browse_.size() + direction) % browse_.size();
        RefreshMusicView(true);
    }

    void EnterMusicFolder(const std::string& dir, const std::string& select_path = "") {
        music_dir_ = dir;
        RebuildBrowse(select_path);
        RefreshMusicView(true);
    }

    void BrowseUp() {
        if (!music_dir_.empty())
            EnterMusicFolder(box2_music::ParentDirectory(music_dir_), music_dir_);
    }

    // M in the browser: open a folder, go up, or play the highlighted file.
    void ActivateBrowseEntry() {
        if (browse_.empty())
            return;
        auto entry = browse_[std::min(browse_cursor_, browse_.size() - 1)];
        using Kind = box2_music::BrowseEntry::Kind;
        if (entry.kind == Kind::Up) {
            BrowseUp();
        } else if (entry.kind == Kind::Folder) {
            EnterMusicFolder(entry.path);
        } else {
            auto result = music_player_.Start(entry.path);
            if (!result)
                GetDisplay()->ShowNotification(result.error());
            else
                music_screen_ = MusicScreen::NowPlaying;
            RefreshMusicView(true);
        }
    }

    // M on the now-playing screen: pause when playing, otherwise play/resume.
    void TogglePlayPause() {
        auto state = music_player_.Snapshot();
        if (state.phase == "playing" || state.phase == "waiting" || state.phase == "indexing") {
            music_player_.Pause();
        } else {
            auto result = state.filename.empty() ? music_player_.Start() : music_player_.Resume();
            if (!result)
                GetDisplay()->ShowNotification(result.error());
        }
        RefreshMusicView(true);
    }

    void SkipMusic(int direction) {
        Application::GetInstance().Schedule([this, direction]() {
            power_save_timer_->WakeUp();
            if (!music_view_)
                music_screen_ = MusicScreen::NowPlaying;
            music_view_ = true;
            auto result = music_player_.Skip(direction);
            if (!result)
                GetDisplay()->ShowNotification(result.error());
            RefreshMusicView(true);
        });
    }

    void InitializeButtons() {
        instance_ = this;

        button_config_t l_btn_cfg = {.long_press_time = 800, .short_press_time = 500};

        button_config_t m_btn_cfg = {.long_press_time = 800, .short_press_time = 500};

        button_config_t r_btn_cfg = {.long_press_time = 800, .short_press_time = 500};

        button_config_t q_btn_cfg = {.long_press_time = 800, .short_press_time = 500};
        button_driver_t* xio_q_btn_driver_ = nullptr;
        button_handle_t q_btn_handle = NULL;
        button_driver_t* xio_l_btn_driver_ = nullptr;
        button_driver_t* xio_m_btn_driver_ = nullptr;

        button_handle_t l_btn_handle = NULL;
        button_handle_t m_btn_handle = NULL;
        button_handle_t r_btn_handle = NULL;

        xio_l_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        xio_l_btn_driver_->enable_power_save = false;
        xio_l_btn_driver_->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            return !instance_->IoExpanderGetLevel(XIO_KEY_L);
        };
        ESP_ERROR_CHECK(iot_button_create(&l_btn_cfg, xio_l_btn_driver_, &l_btn_handle));

        // The Q key's polarity is not documented (L is active-low, M active-high), so treat the
        // level seen at start-up, when the key is not pressed, as "released".
        q_idle_level_ = IoExpanderGetLevel(XIO_KEY_Q);
        ESP_LOGI(TAG, "Q key idle level: %d", q_idle_level_);
        xio_q_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        xio_q_btn_driver_->enable_power_save = false;
        xio_q_btn_driver_->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            return instance_->IoExpanderGetLevel(XIO_KEY_Q) != instance_->q_idle_level_;
        };
        ESP_ERROR_CHECK(iot_button_create(&q_btn_cfg, xio_q_btn_driver_, &q_btn_handle));

        xio_m_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        xio_m_btn_driver_->enable_power_save = false;
        xio_m_btn_driver_->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            return instance_->IoExpanderGetLevel(XIO_KEY_M);
        };
        ESP_ERROR_CHECK(iot_button_create(&m_btn_cfg, xio_m_btn_driver_, &m_btn_handle));

        button_gpio_config_t r_cfg = {.gpio_num = R_BUTTON_GPIO,
                                      .active_level = BUTTON_INACTIVE,
                                      .enable_power_save = false,
                                      .disable_pull = false};
        ESP_ERROR_CHECK(iot_button_new_gpio_device(&r_btn_cfg, &r_cfg, &r_btn_handle));

        // Every press restarts the DJ idle timer; the DJ pad fires on press, not on click,
        // so it is not delayed by double-click detection.
        iot_button_register_cb(
            l_btn_handle, BUTTON_PRESS_DOWN, nullptr,
            [](void*, void* data) { static_cast<atk_dnesp32s3_box2_wifi*>(data)->MusicInput(); },
            this);
        iot_button_register_cb(
            r_btn_handle, BUTTON_PRESS_DOWN, nullptr,
            [](void*, void* data) { static_cast<atk_dnesp32s3_box2_wifi*>(data)->MusicInput(); },
            this);
        iot_button_register_cb(
            q_btn_handle, BUTTON_PRESS_DOWN, nullptr,
            [](void*, void* data) { static_cast<atk_dnesp32s3_box2_wifi*>(data)->MusicInput(); },
            this);
        iot_button_register_cb(
            m_btn_handle, BUTTON_PRESS_DOWN, nullptr,
            [](void*, void* data) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(data);
                self->MusicInput();
                if (self->music_view_ && self->music_screen_ == MusicScreen::Dj)
                    self->DjActivate();
            },
            this);

        // L / R single click: navigate in the player, volume elsewhere.
        for (int direction : {-1, 1}) {
            struct Context {
                atk_dnesp32s3_box2_wifi* self;
                int direction;
            };
            auto* ctx = new Context{this, direction};  // Lives as long as the board.
            iot_button_register_cb(
                direction < 0 ? l_btn_handle : r_btn_handle, BUTTON_SINGLE_CLICK, nullptr,
                [](void*, void* data) {
                    auto* c = static_cast<Context*>(data);
                    auto self = c->self;
                    int direction = c->direction;
                    self->power_save_timer_->WakeUp();
                    if (!self->music_view_) {
                        self->audio_volume_change(direction > 0);
                        return;
                    }
                    Application::GetInstance().Schedule([self, direction]() {
                        switch (self->music_screen_) {
                        case MusicScreen::Files:
                            self->MoveBrowseCursor(direction);
                            break;
                        case MusicScreen::NowPlaying:
                            self->SkipMusic(direction);  // Previous / next track.
                            break;
                        case MusicScreen::Dj:
                            self->DjMove(direction);
                            break;
                        }
                    });
                },
                ctx);
            // L / R double click: previous/next track outside the player, DJ row up/down.
            iot_button_register_cb(
                direction < 0 ? l_btn_handle : r_btn_handle, BUTTON_DOUBLE_CLICK, nullptr,
                [](void*, void* data) {
                    auto* c = static_cast<Context*>(data);
                    auto self = c->self;
                    int direction = c->direction;
                    if (!self->music_view_) {
                        self->SkipMusic(direction);
                    } else if (self->music_screen_ == MusicScreen::Dj) {
                        Application::GetInstance().Schedule(
                            [self, direction]() { self->DjMoveRow(direction); });
                    }
                },
                ctx);
            // Hold L / R: volume in the player (repeats while held), mute / maximum elsewhere.
            iot_button_register_cb(
                direction < 0 ? l_btn_handle : r_btn_handle, BUTTON_LONG_PRESS_START, nullptr,
                [](void*, void* data) {
                    auto* c = static_cast<Context*>(data);
                    auto self = c->self;
                    self->power_save_timer_->WakeUp();
                    if (self->music_view_)
                        self->audio_volume_change(c->direction > 0);
                    else if (c->direction < 0)
                        self->audio_volume_minimum();
                    else
                        self->audio_volume_maxmum();
                },
                ctx);
        }
        RegisterVolumeHold(l_btn_handle, false);
        RegisterVolumeHold(r_btn_handle, true);

        // M single click: select / play-pause; the DJ screen acts on press instead.
        iot_button_register_cb(
            m_btn_handle, BUTTON_SINGLE_CLICK, nullptr,
            [](void*, void* usr_data) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
                Application::GetInstance().Schedule([self]() {
                    self->power_save_timer_->WakeUp();
                    auto state = self->music_player_.Snapshot();
                    if (self->music_view_) {
                        self->MusicInput();
                        if (self->music_screen_ == MusicScreen::Files)
                            self->ActivateBrowseEntry();
                        else if (self->music_screen_ == MusicScreen::NowPlaying)
                            self->TogglePlayPause();
                    } else if (state.busy || state.phase == "paused") {
                        self->OpenMusicView();
                    } else
                        Application::GetInstance().ToggleChatState();
                });
            },
            this);

        // M double click: open the player, or go one screen forward inside it.
        iot_button_register_cb(
            m_btn_handle, BUTTON_DOUBLE_CLICK, nullptr,
            [](void*, void* usr_data) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
                Application::GetInstance().Schedule([self]() {
                    self->power_save_timer_->WakeUp();
                    if (self->music_view_)
                        self->ScreenForward();
                    else
                        self->OpenMusicView();
                });
            },
            this);

        iot_button_register_cb(
            m_btn_handle, BUTTON_LONG_PRESS_START, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);

                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    self->EnterWifiConfigMode();
                    return;
                }

                if (self->power_status_ == kDeviceBatterySupply) {
                    auto backlight = Board::GetInstance().GetBacklight();
                    backlight->SetBrightness(0);
                    esp_timer_stop(self->power_manager_->timer_handle_);
                    esp_io_expander_set_dir(self->io_exp_handle, XIO_CHG_CTRL, IO_EXPANDER_OUTPUT);
                    esp_io_expander_set_level(self->io_exp_handle, XIO_CHG_CTRL, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_io_expander_set_level(self->io_exp_handle, XIO_SYS_POW, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
                } else {
                    // USB-C powered: the power latch cannot cut power, so reboot instead
                    esp_restart();
                }
            },
            this);

        // Q: back / up one level; hold Q leaves the player (stopping the music).
        iot_button_register_cb(
            q_btn_handle, BUTTON_SINGLE_CLICK, nullptr,
            [](void*, void* usr_data) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
                Application::GetInstance().Schedule([self]() {
                    self->power_save_timer_->WakeUp();
                    auto state = self->music_player_.Snapshot();
                    if (self->music_view_)
                        self->ScreenBack();
                    else if (state.busy || state.phase == "paused")
                        self->CloseMusicView();
                });
            },
            this);
        iot_button_register_cb(
            q_btn_handle, BUTTON_LONG_PRESS_START, nullptr,
            [](void*, void* usr_data) {
                auto self = static_cast<atk_dnesp32s3_box2_wifi*>(usr_data);
                Application::GetInstance().Schedule([self]() {
                    self->power_save_timer_->WakeUp();
                    auto state = self->music_player_.Snapshot();
                    if (self->music_view_ || state.busy || state.phase == "paused")
                        self->CloseMusicView();
                });
            },
            this);
    }

    void InitializeSt7789Display() {
        ESP_LOGI(TAG, "Install panel IO");

        /* RD PIN */
        gpio_config_t gpio_init_struct;
        gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
        gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;
        gpio_init_struct.pin_bit_mask = 1ull << LCD_PIN_RD;
        gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&gpio_init_struct);
        gpio_set_level(LCD_PIN_RD, 1);

        /* BL PIN */
        gpio_init_struct.pin_bit_mask = 1ull << DISPLAY_BACKLIGHT_PIN;
        gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&gpio_init_struct);

        esp_lcd_i80_bus_handle_t i80_bus = NULL;
        esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = LCD_PIN_DC,
            .wr_gpio_num = LCD_PIN_WR,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums =
                {
                    LCD_PIN_D0,
                    LCD_PIN_D1,
                    LCD_PIN_D2,
                    LCD_PIN_D3,
                    LCD_PIN_D4,
                    LCD_PIN_D5,
                    LCD_PIN_D6,
                    LCD_PIN_D7,
                },
            .bus_width = 8,
            .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
            .dma_burst_size = 64,
        };
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

        esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = LCD_PIN_CS,
            .pclk_hz = (20 * 1000 * 1000),
            .trans_queue_depth = 7,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels =
                {
                    .dc_idle_level = 1,
                    .dc_cmd_level = 0,
                    .dc_dummy_level = 0,
                    .dc_data_level = 1,
                },
            .flags =
                {
                    .cs_active_high = 0,
                    .pclk_active_neg = 0,
                    .pclk_idle_low = 0,
                },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.reset_gpio_num = LCD_PIN_RST;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_set_gap(panel, 0, 0);
        esp_lcd_panel_io_tx_param(panel_io, 0xCF, (uint8_t[]){0x00, 0x83, 0x30}, 3);
        esp_lcd_panel_io_tx_param(panel_io, 0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);
        esp_lcd_panel_io_tx_param(panel_io, 0xE8, (uint8_t[]){0x85, 0x01, 0x79}, 3);
        esp_lcd_panel_io_tx_param(panel_io, 0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
        esp_lcd_panel_io_tx_param(panel_io, 0xF7, (uint8_t[]){0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xEA, (uint8_t[]){0x00, 0x00}, 2);
        esp_lcd_panel_io_tx_param(panel_io, 0xbb, (uint8_t[]){0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xc3, (uint8_t[]){0x00}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC4, (uint8_t[]){0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC5, (uint8_t[]){0x20}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC6, (uint8_t[]){0x10}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xC7, (uint8_t[]){0xB0}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x36, (uint8_t[]){0x60}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x3A, (uint8_t[]){0x55}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xB1, (uint8_t[]){0x00, 0x1B}, 2);
        esp_lcd_panel_io_tx_param(panel_io, 0xF2, (uint8_t[]){0x08}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x26, (uint8_t[]){0x01}, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0xE0,
                                  (uint8_t[]){0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42,
                                              0x06, 0x0E, 0x12, 0x14, 0x17},
                                  14);
        esp_lcd_panel_io_tx_param(panel_io, 0xE1,
                                  (uint8_t[]){0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47,
                                              0x0E, 0x1C, 0x17, 0x1B, 0x1E},
                                  14);
        esp_lcd_panel_io_tx_param(panel_io, 0xB7, (uint8_t[]){0x07}, 1);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ =
            new MusicDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                             DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

public:
    atk_dnesp32s3_box2_wifi() {
        InitializeI2c();
        InitializeIoExpander();
        InitializePowerSaveTimer();
        InitializePowerManager();
        InitializeSt7789Display();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
        InitializeBoardPowerManager();
        music_player_.InitializeTools();
        music_player_.ScanInBackground();  // Mount the card and log every MP3 found.
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8389AudioCodec audio_codec(
            i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, GPIO_NUM_NC, AUDIO_CODEC_ES8389_ADDR, AUDIO_CODEC_USE_MCLK);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(atk_dnesp32s3_box2_wifi);

// 定义静态成员变量
atk_dnesp32s3_box2_wifi* atk_dnesp32s3_box2_wifi::instance_ = nullptr;

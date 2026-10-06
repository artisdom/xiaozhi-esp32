#pragma once

#include <atomic>
#include <expected>
#include <mutex>
#include <string>
#include <vector>

#include <esp_err.h>
#include <sdmmc_cmd.h>

#include "mp3_index.h"
#include "music_playlist.h"

struct MusicSnapshot {
    std::string phase, filename, title, artist, album, error, repeat;
    uint32_t position_ms = 0, duration_ms = 0, sample_rate = 0, bitrate = 0;
    size_t index = 0, total = 0;
    bool shuffle = false, busy = false, truncated = false, library_scanning = false;
};
class SdMp3Player {
public:
    void InitializeTools();
    std::expected<std::string, std::string> Start(std::string filename = "");
    std::expected<std::string, std::string> Resume();
    void Pause();
    void Stop();
    std::expected<std::string, std::string> Skip(int direction);
    std::expected<std::string, std::string> Seek(uint32_t milliseconds);
    void SetMode(box2_music::Repeat repeat, bool shuffle);
    MusicSnapshot Snapshot();
    bool IsBusy() const { return busy_.load(); }
    bool IsPaused() const { return paused_.load(); }

private:
    std::mutex mutex_, storage_mutex_;
    std::atomic<bool> busy_{false}, cancelled_{false}, paused_{false};
    std::atomic<uint32_t> revision_{0};
    sdmmc_card_t* card_ = nullptr;
    box2_music::Playlist playlist_;
    box2_music::TrackInfo info_;
    bool library_loaded_ = false, truncated_ = false, library_scanning_ = false;
    bool library_refresh_ = false;
    std::string filename_, error_, phase_ = "stopped";
    uint32_t position_ms_ = 0, target_ms_ = 0;
    esp_err_t Mount();
    std::expected<void, std::string> LoadLibrary(bool refresh = false);
    std::expected<bool, std::string> QueueLibraryScan(bool refresh);
    std::expected<void, std::string> LaunchLocked();
    void Worker();
    std::expected<void, std::string> PlayFile(const std::string& filename, uint32_t token,
                                              uint32_t revision, uint32_t target,
                                              const box2_music::TrackInfo& info);
    bool Interrupted(uint32_t revision) const;
};

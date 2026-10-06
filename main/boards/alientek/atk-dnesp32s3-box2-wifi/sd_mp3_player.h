#pragma once

#include <atomic>
#include <expected>
#include <mutex>
#include <string>
#include <vector>

#include <esp_err.h>
#include <sdmmc_cmd.h>

class SdMp3Player {
public:
    void InitializeTools();
    std::expected<std::string, std::string> Start(std::string filename = "");
    void Stop();
    bool IsBusy() const { return busy_.load(); }

private:
    std::mutex mutex_;
    std::mutex storage_mutex_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
    sdmmc_card_t* card_ = nullptr;
    std::string filename_;
    std::string error_;
    std::string phase_ = "stopped";

    esp_err_t Mount();
    std::expected<std::vector<std::string>, std::string> ListTracks(int offset, int limit);
    void Worker();
    std::expected<void, std::string> PlayFile(const std::string& filename, uint32_t token);
    bool ShouldStop() const;
    void SetPhase(const char* phase);
};

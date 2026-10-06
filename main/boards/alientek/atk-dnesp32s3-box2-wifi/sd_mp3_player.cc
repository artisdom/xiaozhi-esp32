#include "sd_mp3_player.h"

#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <new>
#include <utility>

#include <esp_random.h>
#include "application.h"
#include "config.h"
#include "display.h"
#include "esp_ae_rate_cvt.h"
#include "esp_mp3_dec.h"
#include "mcp_server.h"
#include "mp3_utils.h"
#include "settings.h"

namespace {
constexpr const char* kTag = "Box2Music";
constexpr const char* kMountPoint = "/sdcard";
constexpr int kMaxDirectoryEntries = 4096;
constexpr int kMaxScanDepth = 6;

struct DecodeBuffers {
    std::array<uint8_t, 1441> input;    // Largest supported Layer III frame.
    std::array<int16_t, 2304> decoded;  // One maximum-size stereo MP3 frame.
    std::array<int16_t, 1152> mono;
    std::array<int16_t, 4608> resampled;  // 8 kHz -> 24 kHz, plus filter headroom.
};

struct DecoderResources {
    void* decoder = nullptr;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    ~DecoderResources() {
        if (resampler) {
            esp_ae_rate_cvt_close(resampler);
        }
        if (decoder) {
            esp_mp3_dec_close(decoder);
        }
    }
};
}  // namespace

esp_err_t SdMp3Player::Mount() {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    if (card_) {
        return ESP_OK;
    }
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_MOSI_GPIO;
    bus.miso_io_num = SD_MISO_GPIO;
    bus.sclk_io_num = SD_SCLK_GPIO;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = 4096;
    auto ret = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        return ret;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = SD_CS_GPIO;
    esp_vfs_fat_sdmmc_mount_config_t config = {};
    config.format_if_mount_failed = false;
    config.max_files = 3;
    config.allocation_unit_size = 16 * 1024;
    ret = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot, &config, &card_);
    if (ret != ESP_OK) {
        card_ = nullptr;
        spi_bus_free(SD_SPI_HOST);
        ESP_LOGW(kTag, "SD mount failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(kTag, "SD card mounted: %s, %llu MB", card_->cid.name,
                 (unsigned long long)card_->csd.capacity * card_->csd.sector_size / (1024 * 1024));
    }
    return ret;
}

std::expected<void, std::string> SdMp3Player::LoadLibrary(bool refresh) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (library_loaded_ && !refresh)
            return {};
        if (refresh && busy_.load())
            return std::unexpected("Stop playback before refreshing the library");
    }
    auto ret = Mount();
    if (ret != ESP_OK)
        return std::unexpected(std::string("SD card unavailable: ") + esp_err_to_name(ret));
    std::lock_guard<std::mutex> storage_lock(storage_mutex_);
    std::vector<std::string> tracks;
    tracks.reserve(box2_music::Playlist::kCapacity);
    int scanned = 0;
    bool truncated = false, root_opened = false;
    // Breadth-first walk of the whole card; paths are stored relative to the mount point.
    std::vector<std::pair<std::string, int>> pending{{"", 0}};
    for (size_t next = 0; next < pending.size() && !truncated; ++next) {
        auto relative = pending[next].first;
        int depth = pending[next].second;
        auto directory = std::string(kMountPoint) + (relative.empty() ? "" : "/" + relative);
        std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(directory.c_str()), closedir);
        if (!dir) {
            if (relative.empty())
                break;
            continue;
        }
        if (relative.empty())
            root_opened = true;
        while (auto* entry = readdir(dir.get())) {
            if (++scanned > kMaxDirectoryEntries) {
                truncated = true;
                break;
            }
            std::string name = entry->d_name;
            if (name.empty() || name[0] == '.' || name == "System Volume Information")
                continue;
            auto child = relative.empty() ? name : relative + "/" + name;
            struct stat st = {};
            if (child.size() > 255 || stat((std::string(kMountPoint) + "/" + child).c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                if (depth < kMaxScanDepth)
                    pending.emplace_back(std::move(child), depth + 1);
                continue;
            }
            if (!S_ISREG(st.st_mode) || !box2_music::IsMp3Filename(name) ||
                !box2_music::IsMp3RelativePath(child))
                continue;
            if (tracks.size() == box2_music::Playlist::kCapacity) {
                truncated = true;
                break;
            }
            tracks.push_back(std::move(child));
        }
    }
    if (!root_opened)
        return std::unexpected("Cannot read SD card");
    std::lock_guard<std::mutex> lock(mutex_);
    if (refresh && busy_.load())
        return std::unexpected("Playback started during refresh; stop and retry");
    if (library_loaded_ && !refresh)
        return {};
    playlist_.SetTracks(std::move(tracks));
    ESP_LOGI(kTag, "Found %u MP3 file(s) on SD card%s", (unsigned)playlist_.Tracks().size(),
             truncated ? " (list truncated)" : "");
    for (size_t i = 0; i < playlist_.Tracks().size(); ++i)
        ESP_LOGI(kTag, "  [%03u] %s", (unsigned)(i + 1), playlist_.Tracks()[i].c_str());
    if (refresh)
        info_ = {};  // Files may have been replaced even when their names are unchanged.
    if (refresh && !filename_.empty() && !playlist_.Select(filename_)) {
        filename_ = playlist_.Current();
        info_ = {};
        target_ms_ = position_ms_ = 0;
        paused_.store(false);
        phase_ = "stopped";
    }
    library_loaded_ = true;
    truncated_ = truncated;
    return {};
}

std::expected<bool, std::string> SdMp3Player::QueueLibraryScan(bool refresh) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (library_scanning_)
        return true;
    if (library_loaded_ && !refresh)
        return false;
    if (refresh && busy_.load())
        return std::unexpected("Stop playback before refreshing the library");
    library_scanning_ = true;
    library_refresh_ = refresh;
    // Mounting and scanning can take seconds on a card; MCP runs on the main task.
    if (xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<SdMp3Player*>(arg);
                bool refresh;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    refresh = self->library_refresh_;
                }
                {
                    auto result = self->LoadLibrary(refresh);
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    self->library_scanning_ = false;
                    if (!self->busy_.load()) {
                        if (!result) {
                            self->error_ = result.error();
                            self->phase_ = "error";
                        } else if (self->phase_ == "error") {
                            self->error_.clear();
                            self->phase_ = "stopped";
                        }
                    }
                }  // Release the result's string before deleting this FreeRTOS task.
                vTaskDelete(nullptr);
            },
            "sd_library", 8 * 1024, this, 2, nullptr) != pdPASS) {
        library_scanning_ = false;
        return std::unexpected("Cannot allocate SD library task");
    }
    return true;
}

MusicSnapshot SdMp3Player::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    MusicSnapshot s;
    s.phase = phase_;
    s.library_scanning = library_scanning_;
    s.filename = filename_;
    s.title = info_.title.empty() ? filename_ : info_.title;
    s.artist = info_.artist;
    s.album = info_.album;
    s.error = error_;
    s.position_ms = position_ms_;
    s.duration_ms = info_.duration_ms;
    s.sample_rate = info_.sample_rate;
    s.bitrate = info_.bitrate;
    s.index = playlist_.Index();
    s.total = playlist_.Tracks().size();
    s.truncated = truncated_;
    s.repeat = box2_music::RepeatName(playlist_.GetRepeat());
    s.shuffle = playlist_.Shuffle();
    s.busy = busy_.load();
    return s;
}

std::vector<std::string> SdMp3Player::TrackNames(size_t offset, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& tracks = playlist_.Tracks();
    if (offset >= tracks.size())
        return {};
    return {tracks.begin() + offset, tracks.begin() + std::min(tracks.size(), offset + count)};
}

void SdMp3Player::ScanInBackground() {
    auto result = QueueLibraryScan(false);
    if (!result)
        ESP_LOGW(kTag, "Library scan not started: %s", result.error().c_str());
}

std::expected<void, std::string> SdMp3Player::LaunchLocked() {
    if (busy_.load())
        return {};
    busy_.store(true);
    if (xTaskCreate(
            [](void* arg) {
                static_cast<SdMp3Player*>(arg)->Worker();
                vTaskDelete(nullptr);
            },
            "sd_mp3", 20 * 1024, this, 2, nullptr) != pdPASS) {
        busy_.store(false);
        phase_ = "error";
        error_ = "Cannot allocate MP3 task";
        return std::unexpected(error_);
    }
    return {};
}
std::expected<std::string, std::string> SdMp3Player::Start(std::string filename) {
    if (!filename.empty() && !box2_music::IsMp3RelativePath(filename))
        return std::unexpected("Expected an MP3 path from music.list, relative to the SD root");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!filename.empty() && library_loaded_ && !playlist_.Select(filename))
        return std::unexpected("Track not in library; refresh the library first");
    if (!filename.empty()) {
        if (filename_ != filename)
            info_ = {};
        filename_ = std::move(filename);
    } else if (filename_.empty()) {
        info_ = {};
        filename_ = playlist_.Current();
    }
    target_ms_ = position_ms_ = 0;
    paused_.store(false);
    cancelled_.store(false);
    error_.clear();
    phase_ = "waiting";
    ++revision_;
    auto result = LaunchLocked();
    if (!result)
        return std::unexpected(result.error());
    return "Playback queued";
}
std::expected<std::string, std::string> SdMp3Player::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (filename_.empty())
        return std::unexpected("No selected track; use play first");
    if (info_.duration_ms && position_ms_ >= info_.duration_ms)
        position_ms_ = 0;
    target_ms_ = position_ms_;
    paused_.store(false);
    cancelled_.store(false);
    phase_ = "waiting";
    error_.clear();
    ++revision_;
    auto result = LaunchLocked();
    if (!result)
        return std::unexpected(result.error());
    return "Resume queued";
}
void SdMp3Player::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (filename_.empty())
        return;
    paused_.store(true);
    target_ms_ = position_ms_;
    phase_ = "paused";
    ++revision_;
}
void SdMp3Player::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true);
    paused_.store(false);
    target_ms_ = position_ms_ = 0;
    phase_ = "stopped";
    ++revision_;
}
std::expected<std::string, std::string> SdMp3Player::Skip(int direction) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playlist_.Tracks().empty())
        return std::unexpected("Library empty; list or play music first");
    if (direction > 0 || position_ms_ <= 3000)
        playlist_.Advance(direction, true);
    if (filename_ != playlist_.Current())
        info_ = {};
    filename_ = playlist_.Current();
    target_ms_ = position_ms_ = 0;
    paused_.store(false);
    cancelled_.store(false);
    error_.clear();
    phase_ = "waiting";
    ++revision_;
    auto result = LaunchLocked();
    if (!result)
        return std::unexpected(result.error());
    return "Track change queued";
}
std::expected<std::string, std::string> SdMp3Player::Seek(uint32_t milliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (filename_.empty() || !info_.duration_ms)
        return std::unexpected("Select and index a track before seeking");
    target_ms_ = position_ms_ = std::min(milliseconds, info_.duration_ms);
    ++revision_;
    cancelled_.store(false);
    phase_ = paused_.load() ? "paused" : "waiting";
    if (!paused_.load()) {
        auto result = LaunchLocked();
        if (!result)
            return std::unexpected(result.error());
    }
    return "Seek queued";
}
void SdMp3Player::SetMode(box2_music::Repeat repeat, bool shuffle) {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_.SetRepeat(repeat);
    if (playlist_.Shuffle() != shuffle)
        playlist_.SetShuffle(shuffle, esp_random());
    Settings settings("box2_music", true);
    settings.SetString("repeat", box2_music::RepeatName(repeat));
    settings.SetBool("shuffle", shuffle);
}

void SdMp3Player::InitializeTools() {
    {
        Settings settings("box2_music");
        auto repeat = settings.GetString("repeat", "off");
        playlist_.SetRepeat(repeat == "all" ? box2_music::Repeat::All
                                            : (repeat == "one" ? box2_music::Repeat::One
                                                               : box2_music::Repeat::Off));
        playlist_.SetShuffle(settings.GetBool("shuffle", false), esp_random());
    }
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.music.list",
        "List sorted MP3 paths (relative to the SD card root, searched recursively). Supports filename search "
        "and pagination. First scan or refresh is asynchronous: if scanning=true, call again with "
        "refresh=false until ready. Refresh only while stopped; library capped at 512 tracks.",
        PropertyList({Property("offset", kPropertyTypeInteger, 0, 0, 512),
                      Property("limit", kPropertyTypeInteger, 20, 1, 64),
                      Property("query", kPropertyTypeString, std::string()).SetMaxLength(255),
                      Property("refresh", kPropertyTypeBoolean, false)}),
        [this](const PropertyList& p) -> ToolResult {
            auto loaded = QueueLibraryScan(p["refresh"].value<bool>());
            if (!loaded)
                return std::unexpected(loaded.error());
            CJsonUniquePtr result(cJSON_CreateObject()), files(cJSON_CreateArray());
            if (!result || !files)
                return std::unexpected("Out of memory");
            if (*loaded) {
                cJSON_AddBoolToObject(result.get(), "scanning", true);
                cJSON_AddNumberToObject(result.get(), "next_offset", 0);
                if (!cJSON_AddItemToObject(result.get(), "files", files.get()))
                    return std::unexpected("Out of memory");
                files.release();
                return result.release();
            }
            cJSON_AddBoolToObject(result.get(), "scanning", false);
            auto query = p["query"].value<std::string>();
            int offset = p["offset"].value<int>(), limit = p["limit"].value<int>();
            std::lock_guard<std::mutex> lock(mutex_);
            int matching = 0, returned = 0;
            for (const auto& name : playlist_.Tracks()) {
                std::string haystack = name, needle = query;
                for (char& c : haystack)
                    if (c >= 'A' && c <= 'Z')
                        c += 32;
                for (char& c : needle)
                    if (c >= 'A' && c <= 'Z')
                        c += 32;
                if (haystack.find(needle) == std::string::npos)
                    continue;
                if (matching++ < offset || returned >= limit)
                    continue;
                CJsonUniquePtr item(cJSON_CreateString(name.c_str()));
                if (!item || !cJSON_AddItemToArray(files.get(), item.get()))
                    return std::unexpected("Out of memory");
                item.release();
                ++returned;
            }
            cJSON_AddNumberToObject(result.get(), "total", matching);
            cJSON_AddNumberToObject(result.get(), "next_offset", offset + returned);
            cJSON_AddBoolToObject(result.get(), "truncated", truncated_);
            if (!cJSON_AddItemToObject(result.get(), "files", files.get()))
                return std::unexpected("Out of memory");
            files.release();
            return result.release();
        });
    mcp.AddTool(
        "self.music.play",
        "Play or replace the selected MP3 using its exact path from self.music.list. Empty "
        "filename starts selected/first track. Continues through the playlist. Waits for quiet "
        "conversation audio.",
        PropertyList({Property("filename", kPropertyTypeString, std::string()).SetMaxLength(255)}),
        [this](const PropertyList& p) -> ToolResult {
            auto r = Start(p["filename"].value<std::string>());
            if (!r)
                return std::unexpected(r.error());
            return *r;
        });
    mcp.AddTool("self.music.pause", "Pause music and retain the current track and position.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue {
                    Pause();
                    return true;
                });
    mcp.AddTool("self.music.resume",
                "Resume the retained track at its saved position after conversation audio.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    auto r = Resume();
                    if (!r)
                        return std::unexpected(r.error());
                    return *r;
                });
    mcp.AddTool("self.music.stop", "Stop music and reset the selected track position.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue {
                    Stop();
                    return true;
                });
    for (int direction : {-1, 1})
        mcp.AddTool(direction > 0 ? "self.music.next" : "self.music.previous",
                    direction > 0 ? "Play the next playlist track."
                                  : "Restart after 3 seconds, otherwise play the previous track.",
                    PropertyList(), [this, direction](const PropertyList&) -> ToolResult {
                        auto r = Skip(direction);
                        if (!r)
                            return std::unexpected(r.error());
                        return *r;
                    });
    mcp.AddTool("self.music.seek",
                "Seek to a position in seconds in the selected MP3; supports VBR. Clamped "
                "to track duration. Paused tracks stay paused.",
                PropertyList({Property("seconds", kPropertyTypeInteger, 0, 86400)}),
                [this](const PropertyList& p) -> ToolResult {
                    auto r = Seek(uint32_t(p["seconds"].value<int>()) * 1000);
                    if (!r)
                        return std::unexpected(r.error());
                    return *r;
                });
    mcp.AddTool("self.music.set_mode",
                "Set repeat (off/all/one) and shuffle. Both settings persist across restarts.",
                PropertyList({Property("repeat", kPropertyTypeString).SetMaxLength(3),
                              Property("shuffle", kPropertyTypeBoolean)}),
                [this](const PropertyList& p) -> ToolResult {
                    auto repeat = p["repeat"].value<std::string>();
                    if (repeat != "off" && repeat != "all" && repeat != "one")
                        return std::unexpected("Repeat must be off, all or one");
                    SetMode(repeat == "all" ? box2_music::Repeat::All
                                            : (repeat == "one" ? box2_music::Repeat::One
                                                               : box2_music::Repeat::Off),
                            p["shuffle"].value<bool>());
                    return true;
                });
    mcp.AddTool("self.music.status",
                "Get phase, metadata, position/duration in milliseconds, playlist index "
                "(zero based), modes, format and last error.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    auto s = Snapshot();
                    CJsonUniquePtr r(cJSON_CreateObject());
                    if (!r)
                        return std::unexpected("Out of memory");
                    cJSON_AddStringToObject(r.get(), "phase", s.phase.c_str());
                    cJSON_AddStringToObject(r.get(), "filename", s.filename.c_str());
                    cJSON_AddStringToObject(r.get(), "title", s.title.c_str());
                    cJSON_AddStringToObject(r.get(), "artist", s.artist.c_str());
                    cJSON_AddStringToObject(r.get(), "album", s.album.c_str());
                    cJSON_AddStringToObject(r.get(), "error", s.error.c_str());
                    cJSON_AddStringToObject(r.get(), "repeat", s.repeat.c_str());
                    cJSON_AddBoolToObject(r.get(), "shuffle", s.shuffle);
                    cJSON_AddBoolToObject(r.get(), "busy", s.busy);
                    cJSON_AddBoolToObject(r.get(), "library_scanning", s.library_scanning);
                    cJSON_AddBoolToObject(r.get(), "truncated", s.truncated);
                    cJSON_AddNumberToObject(r.get(), "position_ms", s.position_ms);
                    cJSON_AddNumberToObject(r.get(), "duration_ms", s.duration_ms);
                    cJSON_AddNumberToObject(r.get(), "index", s.index);
                    cJSON_AddNumberToObject(r.get(), "total", s.total);
                    cJSON_AddNumberToObject(r.get(), "sample_rate", s.sample_rate);
                    cJSON_AddNumberToObject(r.get(), "bitrate", s.bitrate);
                    return r.release();
                });
}

bool SdMp3Player::Interrupted(uint32_t revision) const {
    return cancelled_.load() || revision_.load() != revision || paused_.load() ||
           Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
}
void SdMp3Player::Worker() {
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    uint32_t revision = revision_.load();
    std::string indexed_file;
    box2_music::TrackInfo metadata;
    std::expected<void, std::string> result = LoadLibrary();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (info_.duration_ms && !info_.points.empty()) {
            metadata = info_;
            indexed_file = filename_;
        }
    }
    bool first = true;
    while (result && !cancelled_.load()) {
        std::string filename;
        uint32_t target;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            revision = revision_.load();
            if (filename_.empty())
                filename_ = playlist_.Current();
            if (!playlist_.Select(filename_)) {
                result = std::unexpected("Track not in SD library");
                break;
            }
            filename = filename_;
            target = target_ms_;
        }
        if (paused_.load())
            break;  // Retain position, but release decoder/task memory while paused.
        // Initial requests and requests issued through conversation wait for two seconds of quiet.
        int quiet = 0;
        uint32_t token = 0;
        for (int ticks = 0;
             ticks < 600 && !cancelled_.load() && revision_.load() == revision && !paused_.load();
             ++ticks) {
            auto state = app.GetDeviceState();
            if ((state == kDeviceStateIdle || state == kDeviceStateListening) &&
                audio.IsPlaybackIdle()) {
                if (++quiet >= (first ? 40 : 1)) {
                    if (state == kDeviceStateListening)
                        app.StopListening();
                    else if ((token = audio.BeginPcmPlayback()) != 0)
                        break;
                }
            } else {
                quiet = 0;
                if (state != kDeviceStateSpeaking && state != kDeviceStateConnecting)
                    break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!token) {
            if (revision_.load() != revision || paused_.load())
                continue;
            if (!cancelled_.load())
                result = std::unexpected("Device did not become ready for music within 30 seconds");
            break;
        }
        app.Schedule([] {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle)
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        });
        first = false;
        if (indexed_file != filename) {
            auto path = std::string(kMountPoint) + "/" + filename;
            std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
            if (!file)
                result = std::unexpected("Cannot open MP3 file");
            else {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (revision_.load() == revision)
                        phase_ = "indexing";
                }
                auto inspected = box2_music::InspectMp3(
                    file.get(), [this, revision] { return Interrupted(revision); },
                    [] { vTaskDelay(1); });
                if (inspected) {
                    metadata = std::move(*inspected);
                    indexed_file = filename;
                } else if (!Interrupted(revision))
                    result = std::unexpected(inspected.error());
            }
        }
        if (result && !Interrupted(revision)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (revision_.load() != revision) {
                    audio.EndPcmPlayback(token, true);
                    continue;
                }
                info_ = metadata;
                target = std::min(target, metadata.duration_ms);
                phase_ = "playing";
            }
            result = PlayFile(filename, token, revision, target, metadata);
            while (result && !Interrupted(revision) && audio.IsPcmPlaybackPending(token))
                vTaskDelay(pdMS_TO_TICKS(20));
        }
        audio.EndPcmPlayback(token, !result || Interrupted(revision));
        std::lock_guard<std::mutex> lock(mutex_);
        if (revision_.load() != revision) {
            first = true;
            continue;
        }
        if (app.GetDeviceState() != kDeviceStateIdle && !cancelled_.load()) {
            paused_.store(true);
            target_ms_ = position_ms_;
            phase_ = "paused";
            break;
        }
        if (!result || cancelled_.load())
            break;
        if (paused_.load())
            continue;
        position_ms_ = metadata.duration_ms;
        if (!playlist_.Advance(1, false)) {
            phase_ = "stopped";
            target_ms_ = 0;
            break;
        }
        filename_ = playlist_.Current();
        info_ = {};
        target_ms_ = position_ms_ = 0;
        ++revision_;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!result && revision_.load() == revision && !cancelled_.load()) {
            error_ = result.error();
            phase_ = "error";
        }
        busy_.store(false);
        // A command may arrive as an interrupted worker is exiting. Keep it queued.
        if (revision_.load() != revision && !cancelled_.load() && !paused_.load() &&
            phase_ == "waiting")
            LaunchLocked();
    }
    app.Schedule([this] {
        if (!IsBusy() && Application::GetInstance().GetDeviceState() == kDeviceStateIdle)
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    });
}
std::expected<void, std::string> SdMp3Player::PlayFile(const std::string& filename, uint32_t token,
                                                       uint32_t revision, uint32_t target,
                                                       const box2_music::TrackInfo& info) {
    auto path = std::string(kMountPoint) + "/" + filename;
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file) {
        return std::unexpected("Cannot open MP3 file");
    }
    auto point = info.SeekStart(target);
    if (fseek(file.get(), point.offset, SEEK_SET) != 0)
        return std::unexpected("Cannot seek MP3 file");
    uint64_t elapsed_samples = 0;
    auto buffers = std::unique_ptr<DecodeBuffers>(new (std::nothrow) DecodeBuffers);
    if (!buffers) {
        return std::unexpected("Cannot allocate MP3 buffers");
    }
    DecoderResources resources;
    if (esp_mp3_dec_open(nullptr, 0, &resources.decoder) != ESP_AUDIO_ERR_OK)
        return std::unexpected("Cannot open MP3 decoder");
    const auto output_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    if (info.sample_rate != static_cast<uint32_t>(output_rate)) {
        esp_ae_rate_cvt_cfg_t config = {};
        config.src_rate = info.sample_rate;
        config.dest_rate = output_rate;
        config.channel = 1;
        config.bits_per_sample = 16;
        config.complexity = 2;
        config.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        if (esp_ae_rate_cvt_open(&config, &resources.resampler) != ESP_AE_ERR_OK)
            return std::unexpected("Cannot create music resampler");
    }
    bool decoded_any = false;
    unsigned frames = 0;
    while (!Interrupted(revision)) {
        uint32_t frame_start = (point.sample_offset + elapsed_samples) * 1000 / info.sample_rate;
        if (frame_start >= info.duration_ms)
            break;
        if (fread(buffers->input.data(), 1, 4, file.get()) != 4)
            return std::unexpected("SD card read failed");
        auto header = box2_music::ParseMp3Frame(buffers->input.data());
        if (!header || header->bytes > buffers->input.size() ||
            header->sample_rate != info.sample_rate || header->channels != info.channels)
            return std::unexpected("MP3 changed since indexing; refresh library");
        if (fread(buffers->input.data() + 4, 1, header->bytes - 4, file.get()) !=
            size_t(header->bytes - 4))
            return std::unexpected("Truncated MP3 frame");
        elapsed_samples += header->samples;
        uint32_t frame_end = (point.sample_offset + elapsed_samples) * 1000 / info.sample_rate;
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = buffers->input.data();
        raw.len = header->bytes;
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = reinterpret_cast<uint8_t*>(buffers->decoded.data());
        frame.len = buffers->decoded.size() * sizeof(int16_t);
        esp_audio_dec_info_t decoded = {};
        auto ret = esp_mp3_dec_decode(resources.decoder, &raw, &frame, &decoded);
        // A seek starts without earlier bit-reservoir data. Only tolerate initial
        // reservoir underflow in the discarded preroll, never in audible frames.
        if (ret == ESP_AUDIO_ERR_FAIL && point.offset != info.points.front().offset &&
            frames < 10 && frame_end <= target) {
            ++frames;
            continue;
        }
        ++frames;
        if (ret != ESP_AUDIO_ERR_OK || raw.consumed != raw.len || frame.decoded_size > frame.len)
            return std::unexpected("Invalid or unsupported MP3 stream");
        if (!frame.decoded_size) {
            if (frames > 10)
                return std::unexpected("MP3 decoder made no progress");
            continue;
        }
        if (decoded.bits_per_sample != 16 || decoded.channel != info.channels ||
            decoded.sample_rate != info.sample_rate ||
            frame.decoded_size % (decoded.channel * sizeof(int16_t)) != 0)
            return std::unexpected("Unsupported MP3 audio format");
        uint32_t count = frame.decoded_size / (sizeof(int16_t) * decoded.channel);
        if (count > buffers->mono.size())
            return std::unexpected("MP3 frame exceeds buffer capacity");
        for (size_t i = 0; i < count; ++i)
            buffers->mono[i] = decoded.channel == 1
                                   ? buffers->decoded[i]
                                   : box2_music::StereoToMono(buffers->decoded[2 * i],
                                                              buffers->decoded[2 * i + 1]);
        int16_t* pcm = buffers->mono.data();
        if (resources.resampler) {
            uint32_t capacity = 0;
            if (esp_ae_rate_cvt_get_max_out_sample_num(resources.resampler, count, &capacity) !=
                    ESP_AE_ERR_OK ||
                capacity > buffers->resampled.size())
                return std::unexpected("Resampled frame exceeds buffer capacity");
            if (esp_ae_rate_cvt_process(resources.resampler, pcm, count, buffers->resampled.data(),
                                        &capacity) != ESP_AE_ERR_OK)
                return std::unexpected("MP3 resampling failed");
            pcm = buffers->resampled.data();
            count = capacity;
        }
        decoded_any = true;
        if (frame_end > target) {
            uint32_t offset =
                target > frame_start
                    ? std::min<uint32_t>(count, uint64_t(target - frame_start) * output_rate / 1000)
                    : 0;
            while (offset < count && !Interrupted(revision)) {
                auto chunk = std::min<uint32_t>(480, count - offset);
                auto queued = Application::GetInstance().GetAudioService().QueuePcm(
                    token, pcm + offset, chunk);
                if (queued == ESP_ERR_TIMEOUT)
                    continue;
                if (queued == ESP_ERR_INVALID_STATE) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (revision_.load() == revision) {
                        paused_.store(true);
                        target_ms_ = position_ms_;
                        phase_ = "paused";
                    }
                    return {};
                }
                if (queued != ESP_OK)
                    return std::unexpected("Cannot queue music audio");
                offset += chunk;
                std::lock_guard<std::mutex> lock(mutex_);
                if (revision_.load() == revision)
                    position_ms_ = std::min<uint32_t>(
                        info.duration_ms, frame_start + uint64_t(offset) * 1000 / output_rate);
            }
        }
        if (frames % 8 == 0)
            vTaskDelay(1);  // Yield during preroll, including zero-volume playback.
    }
    if (!decoded_any && target < info.duration_ms && !Interrupted(revision))
        return std::unexpected("File contained no MP3 audio");
    return {};
}

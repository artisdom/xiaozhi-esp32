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

#include "application.h"
#include "config.h"
#include "display.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "mcp_server.h"
#include "mp3_utils.h"

namespace {
constexpr const char* kTag = "Box2Music";
constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kMusicDir = "/sdcard/MUSIC";
constexpr int kMaxDirectoryEntries = 4096;

struct DecodeBuffers {
    std::array<uint8_t, 1024> input;
    std::array<int16_t, 2304> decoded;  // One maximum-size stereo MP3 frame.
    std::array<int16_t, 1152> mono;
    std::array<int16_t, 4608> resampled;  // 8 kHz -> 24 kHz, plus filter headroom.
};

struct DecoderResources {
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    ~DecoderResources() {
        if (resampler) {
            esp_ae_rate_cvt_close(resampler);
        }
        if (decoder) {
            esp_audio_simple_dec_close(decoder);
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
    }
    return ret;
}

std::expected<std::vector<std::string>, std::string> SdMp3Player::ListTracks(int offset,
                                                                             int limit) {
    auto ret = Mount();
    if (ret != ESP_OK) {
        return std::unexpected(std::string("SD card unavailable: ") + esp_err_to_name(ret));
    }
    std::lock_guard<std::mutex> lock(storage_mutex_);
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(kMusicDir), closedir);
    if (!dir) {
        return std::unexpected("Cannot open MUSIC directory on SD card");
    }
    std::vector<std::string> tracks;
    tracks.reserve(limit);
    int index = 0;
    int scanned = 0;
    while (auto* entry = readdir(dir.get())) {
        if (++scanned > kMaxDirectoryEntries) {
            break;
        }
        if (!box2_music::IsMp3Filename(entry->d_name)) {
            continue;
        }
        struct stat info = {};
        auto path = std::string(kMusicDir) + "/" + entry->d_name;
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        if (index++ < offset) {
            continue;
        }
        tracks.emplace_back(entry->d_name);
        if (tracks.size() == static_cast<size_t>(limit)) {
            break;
        }
    }
    return tracks;
}

void SdMp3Player::InitializeTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.music.list",
                "List MP3 filenames in the SD card MUSIC folder. Use exact "
                "filenames with self.music.play. Directory order; at most 4096 entries scanned.",
                PropertyList({Property("offset", kPropertyTypeInteger, 0, 0, 4096),
                              Property("limit", kPropertyTypeInteger, 20, 1, 64)}),
                [this](const PropertyList& p) -> ToolResult {
                    auto tracks = ListTracks(p["offset"].value<int>(), p["limit"].value<int>());
                    if (!tracks) {
                        return std::unexpected(tracks.error());
                    }
                    CJsonUniquePtr result(cJSON_CreateObject());
                    CJsonUniquePtr files(cJSON_CreateArray());
                    if (!result || !files) {
                        return std::unexpected("Out of memory");
                    }
                    for (const auto& track : *tracks) {
                        CJsonUniquePtr item(cJSON_CreateString(track.c_str()));
                        if (!item || !cJSON_AddItemToArray(files.get(), item.get())) {
                            return std::unexpected("Out of memory");
                        }
                        item.release();
                    }
                    cJSON_AddNumberToObject(result.get(), "next_offset",
                                            p["offset"].value<int>() + tracks->size());
                    if (!cJSON_AddItemToObject(result.get(), "files", files.get())) {
                        return std::unexpected("Out of memory");
                    }
                    files.release();
                    return result.release();
                });
    mcp.AddTool(
        "self.music.play",
        "Play one MP3 from the SD card MUSIC folder. filename is an "
        "exact basename from self.music.list, or empty to play the first track. Playback "
        "starts after the spoken reply, ends conversation listening, and is interrupted "
        "by wake word, chat or other audio. Stop current music before choosing another.",
        PropertyList({Property("filename", kPropertyTypeString, std::string()).SetMaxLength(255)}),
        [this](const PropertyList& p) -> ToolResult {
            auto result = Start(p["filename"].value<std::string>());
            if (!result) {
                return std::unexpected(result.error());
            }
            return *result;
        });
    mcp.AddTool("self.music.stop", "Stop SD-card music or cancel a pending music request.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue {
                    Stop();
                    return true;
                });
    mcp.AddTool("self.music.status", "Get SD-card music phase, filename and last error.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    std::lock_guard<std::mutex> lock(mutex_);
                    CJsonUniquePtr result(cJSON_CreateObject());
                    if (!result) {
                        return std::unexpected("Out of memory");
                    }
                    cJSON_AddBoolToObject(result.get(), "busy", busy_.load());
                    cJSON_AddStringToObject(result.get(), "phase", phase_.c_str());
                    cJSON_AddStringToObject(result.get(), "filename", filename_.c_str());
                    cJSON_AddStringToObject(result.get(), "error", error_.c_str());
                    return result.release();
                });
}

std::expected<std::string, std::string> SdMp3Player::Start(std::string filename) {
    if (!filename.empty() && !box2_music::IsMp3Filename(filename)) {
        return std::unexpected("Expected an MP3 basename in MUSIC (no paths)");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_.load()) {
        return std::unexpected("Music is busy; stop it before starting another track");
    }
    filename_ = std::move(filename);
    error_.clear();
    phase_ = "waiting";
    cancelled_.store(false);
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
    return "Music request queued";
}

void SdMp3Player::Stop() { cancelled_.store(true); }

void SdMp3Player::SetPhase(const char* phase) {
    std::lock_guard<std::mutex> lock(mutex_);
    phase_ = phase;
}

bool SdMp3Player::ShouldStop() const {
    return cancelled_.load() || Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
}

void SdMp3Player::Worker() {
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        filename = filename_;
    }
    std::expected<void, std::string> result;
    uint32_t token = 0;
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    do {
        if (auto ret = Mount(); ret != ESP_OK) {
            result = std::unexpected(std::string("SD card unavailable: ") + esp_err_to_name(ret));
            break;
        }
        if (filename.empty()) {
            auto tracks = ListTracks(0, 1);
            if (!tracks || tracks->empty()) {
                result = std::unexpected(tracks ? "No MP3 files in MUSIC" : tracks.error());
                break;
            }
            filename = tracks->front();
            std::lock_guard<std::mutex> lock(mutex_);
            filename_ = filename;
        }

        // Let the MCP reply and any TTS finish. Auto-listening must end before local playback.
        int quiet_ticks = 0;
        for (int ticks = 0; ticks < 600 && !cancelled_.load(); ++ticks) {
            auto state = app.GetDeviceState();
            if ((state == kDeviceStateIdle || state == kDeviceStateListening) &&
                audio.IsPlaybackIdle()) {
                ++quiet_ticks;
                if (quiet_ticks >= 40) {
                    if (state == kDeviceStateListening) {
                        app.StopListening();  // Event-based; handled by the main task.
                    } else if ((token = audio.BeginPcmPlayback()) != 0) {
                        break;
                    }
                }
            } else {
                quiet_ticks = 0;
                if (state != kDeviceStateSpeaking && state != kDeviceStateConnecting) {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (token == 0) {
            if (!cancelled_.load()) {
                result = std::unexpected("Device did not become ready for music within 30 seconds");
            }
            break;
        }
        SetPhase("playing");
        app.Schedule([filename]() {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                auto& board = Board::GetInstance();
                board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                if (auto* display = board.GetDisplay()) {
                    display->ShowNotification("Playing: " + filename);
                }
            }
        });
        ESP_LOGI(kTag, "Playing %s", filename.c_str());
        result = PlayFile(filename, token);
        while (result && !ShouldStop() && audio.IsPcmPlaybackPending(token)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    } while (false);
    audio.EndPcmPlayback(token, !result || ShouldStop());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!result) {
            error_ = result.error();
            phase_ = "error";
            ESP_LOGW(kTag, "%s", error_.c_str());
        } else {
            phase_ = "stopped";
        }
        busy_.store(false);
    }
    auto error = result ? std::string() : result.error();
    app.Schedule([this, error]() {
        if (!IsBusy() && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            auto& board = Board::GetInstance();
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            if (auto* display = board.GetDisplay()) {
                display->ShowNotification(error.empty() ? "Music stopped" : error.c_str());
            }
        }
    });
}

std::expected<void, std::string> SdMp3Player::PlayFile(const std::string& filename,
                                                       uint32_t token) {
    auto path = std::string(kMusicDir) + "/" + filename;
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file) {
        return std::unexpected("Cannot open MP3 file");
    }
    auto buffers = std::unique_ptr<DecodeBuffers>(new (std::nothrow) DecodeBuffers);
    if (!buffers) {
        return std::unexpected("Cannot allocate MP3 buffers");
    }
    // Register only MP3; the simple decoder supplies the MP3 elementary-stream parser.
    if (esp_mp3_dec_register() != ESP_AUDIO_ERR_OK) {
        return std::unexpected("Cannot register MP3 decoder");
    }
    DecoderResources resources;
    esp_audio_simple_dec_cfg_t config = {};
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (esp_audio_simple_dec_open(&config, &resources.decoder) != ESP_AUDIO_ERR_OK) {
        return std::unexpected("Cannot open MP3 decoder");
    }
    const auto output_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    uint32_t source_rate = 0;
    uint8_t source_channels = 0;
    bool decoded_any = false;
    bool eos = false;
    int no_progress = 0;
    int unconsumed_frames = 0;
    while (!ShouldStop() && !eos) {
        size_t bytes = fread(buffers->input.data(), 1, buffers->input.size(), file.get());
        if (ferror(file.get())) {
            return std::unexpected("SD card read failed");
        }
        eos = bytes < buffers->input.size();
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = buffers->input.data();
        raw.len = bytes;
        raw.eos = eos;
        // Also feed a zero-byte EOS when file size is an exact multiple of the read size.
        do {
            if (ShouldStop()) {
                return {};
            }
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = reinterpret_cast<uint8_t*>(buffers->decoded.data());
            frame.len = buffers->decoded.size() * sizeof(int16_t);
            auto ret = esp_audio_simple_dec_process(resources.decoder, &raw, &frame);
            if (ret != ESP_AUDIO_ERR_OK || raw.consumed > raw.len ||
                frame.decoded_size > frame.len) {
                return std::unexpected("Invalid or unsupported MP3 stream");
            }
            if (raw.consumed == 0 && frame.decoded_size == 0) {
                if (raw.len != 0 || ++no_progress > 4) {
                    return std::unexpected("MP3 decoder made no progress");
                }
            } else {
                no_progress = 0;
            }
            if (raw.consumed == 0 && raw.len != 0) {
                if (++unconsumed_frames > 64) {
                    return std::unexpected("MP3 decoder stalled on input");
                }
            } else {
                unconsumed_frames = 0;
            }
            if (frame.decoded_size != 0) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(resources.decoder, &info) != ESP_AUDIO_ERR_OK ||
                    info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
                    info.sample_rate < 8000 || info.sample_rate > 48000 ||
                    frame.decoded_size % (info.channel * sizeof(int16_t)) != 0) {
                    return std::unexpected("Unsupported MP3 audio format");
                }
                if (source_rate == 0) {
                    source_rate = info.sample_rate;
                    source_channels = info.channel;
                    if (source_rate != static_cast<uint32_t>(output_rate)) {
                        esp_ae_rate_cvt_cfg_t rate_config = {};
                        rate_config.src_rate = source_rate;
                        rate_config.dest_rate = output_rate;
                        rate_config.channel = 1;
                        rate_config.bits_per_sample = 16;
                        rate_config.complexity = 2;
                        rate_config.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                        if (esp_ae_rate_cvt_open(&rate_config, &resources.resampler) !=
                            ESP_AE_ERR_OK) {
                            return std::unexpected("Cannot create music resampler");
                        }
                    }
                } else if (source_rate != info.sample_rate || source_channels != info.channel) {
                    return std::unexpected("MP3 format changed mid-stream");
                }
                uint32_t count = frame.decoded_size / (sizeof(int16_t) * info.channel);
                if (count > buffers->mono.size()) {
                    return std::unexpected("MP3 frame exceeds buffer capacity");
                }
                for (size_t i = 0; i < count; ++i) {
                    buffers->mono[i] = info.channel == 1
                                           ? buffers->decoded[i]
                                           : box2_music::StereoToMono(buffers->decoded[2 * i],
                                                                      buffers->decoded[2 * i + 1]);
                }
                int16_t* pcm = buffers->mono.data();
                if (resources.resampler) {
                    uint32_t capacity = 0;
                    if (esp_ae_rate_cvt_get_max_out_sample_num(resources.resampler, count,
                                                               &capacity) != ESP_AE_ERR_OK ||
                        capacity > buffers->resampled.size()) {
                        return std::unexpected("Resampled frame exceeds buffer capacity");
                    }
                    if (esp_ae_rate_cvt_process(resources.resampler, pcm, count,
                                                buffers->resampled.data(),
                                                &capacity) != ESP_AE_ERR_OK) {
                        return std::unexpected("MP3 resampling failed");
                    }
                    pcm = buffers->resampled.data();
                    count = capacity;
                }
                for (uint32_t offset = 0; offset < count && !ShouldStop();) {
                    auto chunk = std::min<uint32_t>(480, count - offset);
                    auto queued = Application::GetInstance().GetAudioService().QueuePcm(
                        token, pcm + offset, chunk);
                    if (queued == ESP_ERR_TIMEOUT) {
                        continue;
                    }
                    if (queued == ESP_ERR_INVALID_STATE) {
                        return {};  // A higher-priority audio producer interrupted music.
                    }
                    if (queued != ESP_OK) {
                        return std::unexpected("Cannot queue music audio");
                    }
                    offset += chunk;
                }
                decoded_any = true;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        } while (raw.len != 0);
        vTaskDelay(1);  // Yield even for corrupt streams or large metadata tags.
    }
    if (!decoded_any && !ShouldStop()) {
        return std::unexpected("File contained no MP3 audio");
    }
    return {};
}

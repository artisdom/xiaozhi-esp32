#include "audio_recorder.h"
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <ctime>
#include <algorithm>
#include <errno.h>

#include "board.h"
#include "application.h"

#include <esp_lvgl_port.h>

static const char* TAG = "AudioRecorder";

// SD card recording folder
#define RECORDING_FOLDER "/sdcard/Recording"

// Recording parameters - will use codec's actual sample rate
#define RECORDING_BITS_PER_SAMPLE 16

// Task configuration
#define RECORDING_TASK_STACK_SIZE (8 * 1024)
#define RECORDING_TASK_PRIORITY   3
#define COMMAND_QUEUE_SIZE        10
#define RECORDING_READ_INTERVAL_MS 10  // Read audio every 10ms (matches audio service)

// Grid layout
#define GRID_COLUMNS 3
#define GRID_ITEM_WIDTH 150
#define GRID_ITEM_HEIGHT 80

//=============================================================================
// AudioRecorder implementation
//=============================================================================

AudioRecorder::AudioRecorder() {
    command_queue_ = xQueueCreate(COMMAND_QUEUE_SIZE, sizeof(RecordCommand));
}

AudioRecorder::~AudioRecorder() {
    if (task_running_.load()) {
        RecordCommand cmd = RecordCommand::EXIT;
        xQueueSend(command_queue_, &cmd, portMAX_DELAY);
        
        // Wait for task to exit
        int timeout = 100;
        while (task_running_.load() && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout--;
        }
        
        if (task_running_.load()) {
            ESP_LOGW(TAG, "Recording task did not exit cleanly, deleting forcefully");
            if (record_task_handle_) {
                vTaskDelete(record_task_handle_);
                record_task_handle_ = nullptr;
            }
        }
    }
    
    if (command_queue_) {
        vQueueDelete(command_queue_);
        command_queue_ = nullptr;
    }
    
    if (timer_update_timer_) {
        lv_timer_delete(timer_update_timer_);
        timer_update_timer_ = nullptr;
    }
    
    if (recording_file_) {
        fclose(recording_file_);
        recording_file_ = nullptr;
    }
    
    if (container_) {
        lv_obj_delete(container_);
        container_ = nullptr;
    }
}

void AudioRecorder::Init(lv_obj_t* parent) {
    if (container_) {
        return; // Already initialized
    }
    if (parent == nullptr) {
        parent = lv_screen_active();
    }
    CreateUI(parent);
    
    ESP_LOGI(TAG, "Audio recorder initialized");
}

void AudioRecorder::CreateUI(lv_obj_t* parent) {
    // Main fullscreen container
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    // Title bar
    title_bar_ = lv_obj_create(container_);
    lv_obj_set_size(title_bar_, LV_HOR_RES, 60);
    lv_obj_align(title_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(title_bar_, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(title_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(title_bar_, 0, 0);
    lv_obj_set_style_border_width(title_bar_, 0, 0);
    lv_obj_set_style_pad_all(title_bar_, 8, 0);
    lv_obj_clear_flag(title_bar_, LV_OBJ_FLAG_SCROLLABLE);

    // Title label
    title_label_ = lv_label_create(title_bar_);
    lv_obj_set_width(title_label_, LV_HOR_RES - 140);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(title_label_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_label_set_text(title_label_, "Audio Recorder");

    // Close button (top right)
    close_btn_ = lv_btn_create(title_bar_);
    lv_obj_set_size(close_btn_, 50, 44);
    lv_obj_align(close_btn_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn_, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_radius(close_btn_, 8, 0);
    lv_obj_set_style_border_width(close_btn_, 0, 0);
    lv_obj_add_event_cb(close_btn_, OnCloseButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* close_label = lv_label_create(close_btn_);
    lv_label_set_text(close_label, FONT_AWESOME_XMARK);
    lv_obj_center(close_label);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);

    // Recording control area (below title bar)
    lv_obj_t* control_area = lv_obj_create(container_);
    lv_obj_set_size(control_area, LV_HOR_RES, 140);
    lv_obj_align(control_area, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(control_area, lv_color_hex(0x1a1a3e), 0);
    lv_obj_set_style_bg_opa(control_area, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(control_area, 0, 0);
    lv_obj_set_style_border_width(control_area, 0, 0);
    lv_obj_set_style_pad_all(control_area, 16, 0);
    lv_obj_clear_flag(control_area, LV_OBJ_FLAG_SCROLLABLE);

    // Record button (large circular button)
    record_btn_ = lv_btn_create(control_area);
    lv_obj_set_size(record_btn_, 80, 80);
    lv_obj_align(record_btn_, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_bg_color(record_btn_, lv_color_hex(0xf44336), 0);  // Red for record
    lv_obj_set_style_bg_opa(record_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(record_btn_, 40, 0);
    lv_obj_set_style_border_color(record_btn_, lv_color_white(), 0);
    lv_obj_set_style_border_width(record_btn_, 3, 0);
    lv_obj_add_event_cb(record_btn_, OnRecordButtonClicked, LV_EVENT_CLICKED, this);
    
    record_btn_label_ = lv_label_create(record_btn_);
    lv_label_set_text(record_btn_label_, FONT_AWESOME_MICROPHONE);
    lv_obj_center(record_btn_label_);
    lv_obj_set_style_text_color(record_btn_label_, lv_color_white(), 0);

    // Timer label (shows recording duration)
    timer_label_ = lv_label_create(control_area);
    lv_obj_align(timer_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(timer_label_, lv_color_white(), 0);
    // Use default font - larger fonts may not be available
    lv_label_set_text(timer_label_, "00:00");

    // Status label
    status_label_ = lv_label_create(control_area);
    lv_obj_align(status_label_, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x888888), 0);
    lv_label_set_text(status_label_, "Tap to record");

    // Recordings grid area
    recordings_grid_ = lv_obj_create(container_);
    lv_obj_set_size(recordings_grid_, LV_HOR_RES, LV_VER_RES - 200);
    lv_obj_align(recordings_grid_, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_style_bg_opa(recordings_grid_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(recordings_grid_, 0, 0);
    lv_obj_set_style_pad_all(recordings_grid_, 16, 0);
    lv_obj_set_flex_flow(recordings_grid_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(recordings_grid_, 12, 0);
    lv_obj_set_style_pad_column(recordings_grid_, 12, 0);
    lv_obj_set_scrollbar_mode(recordings_grid_, LV_SCROLLBAR_MODE_AUTO);

    // "No recordings" placeholder label
    no_recordings_label_ = lv_label_create(recordings_grid_);
    lv_obj_set_width(no_recordings_label_, LV_HOR_RES - 60);
    lv_obj_set_style_text_align(no_recordings_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(no_recordings_label_, lv_color_hex(0x666666), 0);
    lv_label_set_text(no_recordings_label_, "No recordings yet.\nTap the record button to start.");

    ESP_LOGI(TAG, "Audio recorder UI created");
}

void AudioRecorder::Show() {
    if (!container_) {
        ESP_LOGE(TAG, "Container not initialized");
        return;
    }
    
    is_visible_.store(true);
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    
    // Reset recording state
    lv_label_set_text(timer_label_, "00:00");
    lv_label_set_text(status_label_, "Tap to record");
    lv_label_set_text(record_btn_label_, FONT_AWESOME_MICROPHONE);
    lv_obj_set_style_bg_color(record_btn_, lv_color_hex(0xf44336), 0);  // Red
    
    // Create recording folder if it doesn't exist
    struct stat st;
    if (stat(RECORDING_FOLDER, &st) != 0) {
        if (mkdir(RECORDING_FOLDER, 0755) != 0) {
            ESP_LOGE(TAG, "Failed to create Recording folder: %s", strerror(errno));
        } else {
            ESP_LOGI(TAG, "Created Recording folder");
        }
    }
    
    // Refresh recordings list
    RefreshRecordingsList();
    
    ESP_LOGI(TAG, "Audio recorder shown");
}

void AudioRecorder::Hide() {
    if (!container_) return;
    
    // Stop recording if in progress
    if (is_recording_.load()) {
        StopRecording();
    }
    
    // Stop timer if running
    if (timer_update_timer_) {
        lv_timer_delete(timer_update_timer_);
        timer_update_timer_ = nullptr;
    }
    
    is_visible_.store(false);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    
    if (close_callback_) {
        close_callback_();
    }
    
    ESP_LOGI(TAG, "Audio recorder hidden");
}

void AudioRecorder::RefreshRecordingsList() {
    recordings_ = ScanRecordings();
    PopulateRecordingsGrid();
    ESP_LOGI(TAG, "Found %zu recordings", recordings_.size());
}

std::vector<RecordingEntry> AudioRecorder::ScanRecordings() {
    std::vector<RecordingEntry> entries;
    
    DIR* dir = opendir(RECORDING_FOLDER);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open Recording folder");
        return entries;
    }
    
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type == DT_REG) {  // Regular file
            std::string name = ent->d_name;
            // Check if it's a WAV file
            if (name.size() > 4) {
                std::string ext = name.substr(name.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".wav") {
                    RecordingEntry entry;
                    entry.name = name;
                    entry.path = std::string(RECORDING_FOLDER) + "/" + name;
                    
                    struct stat st;
                    if (stat(entry.path.c_str(), &st) == 0) {
                        entry.size = st.st_size;
                        entry.timestamp = st.st_mtime;
                    } else {
                        entry.size = 0;
                        entry.timestamp = 0;
                    }
                    
                    entries.push_back(entry);
                }
            }
        }
    }
    closedir(dir);
    
    // Sort by timestamp (newest first)
    std::sort(entries.begin(), entries.end(), [](const RecordingEntry& a, const RecordingEntry& b) {
        return a.timestamp > b.timestamp;
    });
    
    return entries;
}

void AudioRecorder::PopulateRecordingsGrid() {
    if (!recordings_grid_) return;
    
    // Clear existing items (except the no_recordings_label_)
    uint32_t child_count = lv_obj_get_child_count(recordings_grid_);
    for (int i = child_count - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(recordings_grid_, i);
        if (child != no_recordings_label_) {
            lv_obj_delete(child);
        }
    }
    
    if (recordings_.empty()) {
        lv_obj_clear_flag(no_recordings_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    
    lv_obj_add_flag(no_recordings_label_, LV_OBJ_FLAG_HIDDEN);
    
    // Create item for each recording
    for (size_t i = 0; i < recordings_.size(); i++) {
        const auto& entry = recordings_[i];
        
        // Item container
        lv_obj_t* item = lv_obj_create(recordings_grid_);
        lv_obj_set_size(item, GRID_ITEM_WIDTH, GRID_ITEM_HEIGHT);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x2a2a4e), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 8, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        
        // Store path as user data (need to copy since entry might be invalidated)
        char* path_copy = (char*)lv_malloc(entry.path.size() + 1);
        strcpy(path_copy, entry.path.c_str());
        lv_obj_set_user_data(item, path_copy);
        
        // Make clickable
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item, OnRecordingItemClicked, LV_EVENT_CLICKED, this);
        
        // Clean up user data when item is deleted
        lv_obj_add_event_cb(item, [](lv_event_t* e) {
            lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
            char* path = (char*)lv_obj_get_user_data(obj);
            if (path) {
                lv_free(path);
            }
        }, LV_EVENT_DELETE, nullptr);
        
        // Music icon
        lv_obj_t* icon = lv_label_create(item);
        lv_label_set_text(icon, FONT_AWESOME_MUSIC);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xffa500), 0);
        
        // Filename (truncated)
        lv_obj_t* name_label = lv_label_create(item);
        lv_obj_set_width(name_label, GRID_ITEM_WIDTH - 50);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        lv_obj_align(name_label, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
        
        // Extract just the filename without path and extension
        std::string display_name = entry.name;
        if (display_name.size() > 4) {
            display_name = display_name.substr(0, display_name.size() - 4);  // Remove .wav
        }
        lv_label_set_text(name_label, display_name.c_str());
        
        // File size
        lv_obj_t* size_label = lv_label_create(item);
        lv_obj_align(size_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_text_color(size_label, lv_color_hex(0x888888), 0);
        
        char size_str[32];
        if (entry.size < 1024) {
            snprintf(size_str, sizeof(size_str), "%zu B", entry.size);
        } else if (entry.size < 1024 * 1024) {
            snprintf(size_str, sizeof(size_str), "%.1f KB", entry.size / 1024.0);
        } else {
            snprintf(size_str, sizeof(size_str), "%.1f MB", entry.size / (1024.0 * 1024.0));
        }
        lv_label_set_text(size_label, size_str);
    }
}

std::string AudioRecorder::GenerateRecordingFilename() {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    
    char filename[64];
    snprintf(filename, sizeof(filename), "REC_%04d%02d%02d_%02d%02d%02d.wav",
             timeinfo->tm_year + 1900,
             timeinfo->tm_mon + 1,
             timeinfo->tm_mday,
             timeinfo->tm_hour,
             timeinfo->tm_min,
             timeinfo->tm_sec);
    
    return std::string(filename);
}

bool AudioRecorder::WriteWavHeader(FILE* file, int sample_rate, int channels, int bits_per_sample) {
    if (!file) return false;
    
    // WAV header structure (44 bytes)
    uint8_t header[44] = {0};
    
    // RIFF chunk
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    // File size - 8 (placeholder, will be updated later)
    // header[4-7] = file size - 8 (placeholder)
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    
    // fmt chunk
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;  // Chunk size (16 for PCM)
    header[20] = 1; header[21] = 0;  // Audio format (1 = PCM)
    header[22] = channels; header[23] = 0;  // Number of channels
    
    // Sample rate (4 bytes, little endian)
    header[24] = sample_rate & 0xFF;
    header[25] = (sample_rate >> 8) & 0xFF;
    header[26] = (sample_rate >> 16) & 0xFF;
    header[27] = (sample_rate >> 24) & 0xFF;
    
    // Byte rate (sample_rate * channels * bits_per_sample / 8)
    int byte_rate = sample_rate * channels * bits_per_sample / 8;
    header[28] = byte_rate & 0xFF;
    header[29] = (byte_rate >> 8) & 0xFF;
    header[30] = (byte_rate >> 16) & 0xFF;
    header[31] = (byte_rate >> 24) & 0xFF;
    
    // Block align (channels * bits_per_sample / 8)
    int block_align = channels * bits_per_sample / 8;
    header[32] = block_align & 0xFF;
    header[33] = (block_align >> 8) & 0xFF;
    
    // Bits per sample
    header[34] = bits_per_sample & 0xFF;
    header[35] = (bits_per_sample >> 8) & 0xFF;
    
    // data chunk
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    // Data size (placeholder, will be updated later)
    // header[40-43] = data size (placeholder)
    
    return fwrite(header, 1, 44, file) == 44;
}

bool AudioRecorder::UpdateWavHeader(FILE* file, size_t data_size) {
    if (!file) return false;
    
    // Update file size (total size - 8)
    uint32_t file_size = data_size + 36;  // 44 - 8
    fseek(file, 4, SEEK_SET);
    fwrite(&file_size, 4, 1, file);
    
    // Update data chunk size
    uint32_t chunk_size = data_size;
    fseek(file, 40, SEEK_SET);
    fwrite(&chunk_size, 4, 1, file);
    
    return true;
}

void AudioRecorder::ShowStatus(const char* message, uint32_t duration_ms) {
    if (!status_label_) return;
    
    lv_label_set_text(status_label_, message);
    
    // If duration specified, reset after delay
    if (duration_ms > 0) {
        lv_timer_t* timer = lv_timer_create([](lv_timer_t* t) {
            lv_obj_t* label = (lv_obj_t*)lv_timer_get_user_data(t);
            if (label) {
                lv_label_set_text(label, "Tap to record");
            }
            lv_timer_delete(t);
        }, duration_ms, status_label_);
        lv_timer_set_repeat_count(timer, 1);
    }
}

void AudioRecorder::UpdateRecordingTimer() {
    if (!timer_label_) return;
    
    uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS - recording_start_time_) / 1000;
    uint32_t minutes = elapsed / 60;
    uint32_t seconds = elapsed % 60;
    
    char timer_str[16];
    snprintf(timer_str, sizeof(timer_str), "%02lu:%02lu", minutes, seconds);
    lv_label_set_text(timer_label_, timer_str);
}

bool AudioRecorder::StartRecording() {
#if CONFIG_IDF_TARGET_ESP32P4
    if (is_recording_.load()) {
        ESP_LOGW(TAG, "Already recording");
        return false;
    }
    
    // Get codec to determine sample rate
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "Audio codec not available");
        ShowStatus("No audio codec", 3000);
        return false;
    }
    
    // Use codec's actual sample rate - always record mono for simplicity
    recording_sample_rate_ = codec->input_sample_rate();
    int recording_channels = 1;
    
    ESP_LOGI(TAG, "Recording at %d Hz, mono (codec input: %d ch)", 
             recording_sample_rate_, codec->input_channels());
    
    // Generate filename and open file
    std::string filename = GenerateRecordingFilename();
    current_recording_path_ = std::string(RECORDING_FOLDER) + "/" + filename;
    
    recording_file_ = fopen(current_recording_path_.c_str(), "wb");
    if (!recording_file_) {
        ESP_LOGE(TAG, "Failed to create recording file: %s", strerror(errno));
        ShowStatus("Failed to create file", 3000);
        return false;
    }
    
    // Write WAV header with actual sample rate
    if (!WriteWavHeader(recording_file_, recording_sample_rate_, recording_channels, RECORDING_BITS_PER_SAMPLE)) {
        ESP_LOGE(TAG, "Failed to write WAV header");
        fclose(recording_file_);
        recording_file_ = nullptr;
        remove(current_recording_path_.c_str());
        ShowStatus("Failed to write header", 3000);
        return false;
    }
    
    samples_recorded_ = 0;
    recording_start_time_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Create recording task if not already running
    if (!task_running_.load()) {
        xTaskCreatePinnedToCore(RecordingTaskFunc, "audio_record",
                                RECORDING_TASK_STACK_SIZE, this,
                                RECORDING_TASK_PRIORITY, &record_task_handle_, 1);
    }
    
    // Send start command
    RecordCommand cmd = RecordCommand::START;
    xQueueSend(command_queue_, &cmd, portMAX_DELAY);
    
    // Update UI
    is_recording_.store(true);
    lv_label_set_text(record_btn_label_, FONT_AWESOME_STOP);
    lv_obj_set_style_bg_color(record_btn_, lv_color_hex(0x4caf50), 0);  // Green when recording
    lv_label_set_text(status_label_, "Recording...");
    
    // Start timer update
    timer_update_timer_ = lv_timer_create(OnTimerUpdate, 500, this);
    
    ESP_LOGI(TAG, "Recording started: %s", current_recording_path_.c_str());
    return true;
#else
    ESP_LOGW(TAG, "Recording not supported on this platform");
    ShowStatus("Not supported", 3000);
    return false;
#endif
}

bool AudioRecorder::StopRecording() {
#if CONFIG_IDF_TARGET_ESP32P4
    if (!is_recording_.load()) {
        ESP_LOGW(TAG, "Not recording");
        return false;
    }
    
    // Send stop command
    RecordCommand cmd = RecordCommand::STOP;
    xQueueSend(command_queue_, &cmd, portMAX_DELAY);
    
    // Wait for recording to stop
    int timeout = 50;
    while (is_recording_.load() && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        timeout--;
    }
    
    // Stop timer update
    if (timer_update_timer_) {
        lv_timer_delete(timer_update_timer_);
        timer_update_timer_ = nullptr;
    }
    
    // Update WAV header with final size
    if (recording_file_) {
        size_t data_size = samples_recorded_ * sizeof(int16_t);
        UpdateWavHeader(recording_file_, data_size);
        fclose(recording_file_);
        recording_file_ = nullptr;
        
        ESP_LOGI(TAG, "Recording saved: %s (%zu samples)", current_recording_path_.c_str(), samples_recorded_);
    }
    
    // Update UI
    lv_label_set_text(record_btn_label_, FONT_AWESOME_MICROPHONE);
    lv_obj_set_style_bg_color(record_btn_, lv_color_hex(0xf44336), 0);  // Red
    ShowStatus("Recording saved", 2000);
    
    // Refresh recordings list
    RefreshRecordingsList();
    
    return true;
#else
    return false;
#endif
}

void AudioRecorder::PlayRecording(const std::string& path) {
    ESP_LOGI(TAG, "Playing recording: %s", path.c_str());
    
    if (play_callback_) {
        // Hide recorder first and let the callback handle playback
        Hide();
        play_callback_(path);
    } else {
        ShowStatus("Playback not available", 2000);
    }
}

void AudioRecorder::RecordingTaskFunc(void* arg) {
    AudioRecorder* recorder = static_cast<AudioRecorder*>(arg);
    recorder->task_running_.store(true);
    
    ESP_LOGI(TAG, "Recording task started");
    
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        ESP_LOGE(TAG, "Audio codec not available");
        recorder->task_running_.store(false);
        vTaskDelete(nullptr);
        return;
    }
    
    bool recording = false;
    std::vector<int16_t> audio_buffer;
    
    // Calculate samples per read based on codec's actual sample rate
    int codec_sample_rate = codec->input_sample_rate();
    int codec_channels = codec->input_channels();
    bool has_reference = codec->input_reference();  // True if second channel is reference for AEC
    int samples_per_read = codec_sample_rate * RECORDING_READ_INTERVAL_MS / 1000;
    
    ESP_LOGI(TAG, "Codec: sample_rate=%d, channels=%d, has_reference=%d, samples_per_read=%d", 
             codec_sample_rate, codec_channels, has_reference, samples_per_read);
    
    while (true) {
        RecordCommand cmd = RecordCommand::NONE;
        
        if (recording) {
            // Non-blocking check for commands while recording
            xQueueReceive(recorder->command_queue_, &cmd, 0);
        } else {
            // Blocking wait when not recording
            xQueueReceive(recorder->command_queue_, &cmd, portMAX_DELAY);
        }
        
        if (cmd == RecordCommand::EXIT) {
            ESP_LOGI(TAG, "Recording task exit requested");
            break;
        }
        
        if (cmd == RecordCommand::START) {
            recording = true;
            codec->EnableInput(true);
            ESP_LOGI(TAG, "Recording started in task");
            continue;
        }
        
        if (cmd == RecordCommand::STOP) {
            recording = false;
            recorder->is_recording_.store(false);
            codec->EnableInput(false);
            ESP_LOGI(TAG, "Recording stopped in task");
            continue;
        }
        
        if (recording && recorder->recording_file_) {
            // Read audio data - buffer size = samples * channels
            // Use smaller chunks and no delay - let InputData blocking call pace the loop
            audio_buffer.resize(samples_per_read * codec_channels);
            if (codec->InputData(audio_buffer)) {
                // Convert to mono if multi-channel
                if (codec_channels > 1) {
                    size_t mono_size = audio_buffer.size() / codec_channels;
                    if (has_reference) {
                        // Second channel is reference (for AEC), only take first channel (microphone)
                        for (size_t i = 0, j = 0; i < mono_size; i++, j += codec_channels) {
                            audio_buffer[i] = audio_buffer[j];
                        }
                    } else {
                        // True stereo - average both channels
                        for (size_t i = 0, j = 0; i < mono_size; i++, j += codec_channels) {
                            int32_t sum = (int32_t)audio_buffer[j] + (int32_t)audio_buffer[j + 1];
                            audio_buffer[i] = (int16_t)(sum / 2);
                        }
                    }
                    audio_buffer.resize(mono_size);
                }
                
                // Write to file
                size_t written = fwrite(audio_buffer.data(), sizeof(int16_t), 
                                       audio_buffer.size(), recorder->recording_file_);
                if (written != audio_buffer.size()) {
                    ESP_LOGE(TAG, "Failed to write audio data: wrote %zu of %zu",
                             written, audio_buffer.size());
                }
                recorder->samples_recorded_ += written;
            }
            // No delay - InputData blocks until DMA buffer has data, naturally pacing the loop
        }
    }
    
    recorder->task_running_.store(false);
    recorder->record_task_handle_ = nullptr;
    ESP_LOGI(TAG, "Recording task exited");
    vTaskDelete(nullptr);
}

void AudioRecorder::OnRecordButtonClicked(lv_event_t* e) {
    AudioRecorder* recorder = static_cast<AudioRecorder*>(lv_event_get_user_data(e));
    if (!recorder) return;
    
    if (recorder->is_recording_.load()) {
        ESP_LOGI(TAG, "Stop button clicked");
        recorder->StopRecording();
    } else {
        ESP_LOGI(TAG, "Record button clicked");
        recorder->StartRecording();
    }
}

void AudioRecorder::OnCloseButtonClicked(lv_event_t* e) {
    AudioRecorder* recorder = static_cast<AudioRecorder*>(lv_event_get_user_data(e));
    if (recorder) {
        ESP_LOGI(TAG, "Close button clicked");
        recorder->Hide();
    }
}

void AudioRecorder::OnRecordingItemClicked(lv_event_t* e) {
    AudioRecorder* recorder = static_cast<AudioRecorder*>(lv_event_get_user_data(e));
    if (!recorder) return;
    
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    char* path = (char*)lv_obj_get_user_data(target);
    if (path) {
        ESP_LOGI(TAG, "Recording item clicked: %s", path);
        recorder->PlayRecording(std::string(path));
    }
}

void AudioRecorder::OnTimerUpdate(lv_timer_t* timer) {
    AudioRecorder* recorder = static_cast<AudioRecorder*>(lv_timer_get_user_data(timer));
    if (recorder && recorder->is_recording_.load()) {
        recorder->UpdateRecordingTimer();
    }
}

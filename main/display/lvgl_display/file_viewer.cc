#include "file_viewer.h"
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <algorithm>

#include "application.h"
#include "board.h"

#ifndef CONFIG_IDF_TARGET_ESP32
#include "jpg/jpeg_to_image.h"
#endif

// MP3/WAV audio player support for ESP32-S3 and ESP32-P4
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32P4)
#define AUDIO_PLAYER_SUPPORTED 1
#include <audio_player.h>
#endif

static const char* TAG = "FileViewer";

// Maximum file size to load (64KB for text files)
#define MAX_TEXT_FILE_SIZE (64 * 1024)
// Maximum image file size (2MB)
#define MAX_IMAGE_FILE_SIZE (2 * 1024 * 1024)
// Maximum audio file size for OGG (4MB)
#define MAX_AUDIO_FILE_SIZE (4 * 1024 * 1024)

//=============================================================================
// FileViewer base class
//=============================================================================

FileViewer::FileViewer() {}

FileViewer::~FileViewer() {
    if (container_) {
        lv_obj_delete(container_);
        container_ = nullptr;
    }
}

void FileViewer::Init(lv_obj_t* parent) {
    if (container_) return;
    if (parent == nullptr) {
        parent = lv_screen_active();
    }
    CreateBaseUI(parent);
}

void FileViewer::CreateBaseUI(lv_obj_t* parent) {
    // Main container overlay
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
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
    lv_obj_set_width(title_label_, LV_HOR_RES - 80);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_label_set_text(title_label_, "File Viewer");

    // Close button
    close_btn_ = lv_btn_create(title_bar_);
    lv_obj_set_size(close_btn_, 50, 44);
    lv_obj_align(close_btn_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn_, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_radius(close_btn_, 8, 0);
    lv_obj_add_event_cb(close_btn_, OnCloseButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* close_label = lv_label_create(close_btn_);
    lv_label_set_text(close_label, FONT_AWESOME_XMARK);
    lv_obj_center(close_label);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);

    // Content area
    content_area_ = lv_obj_create(container_);
    lv_obj_set_size(content_area_, LV_HOR_RES, LV_VER_RES - 60);
    lv_obj_align(content_area_, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(content_area_, lv_color_hex(0x0f0f1a), 0);
    lv_obj_set_style_bg_opa(content_area_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content_area_, 0, 0);
    lv_obj_set_style_pad_all(content_area_, 16, 0);
}

void FileViewer::Close() {
    if (container_) {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = false;
    }
    
    if (close_callback_) {
        close_callback_();
    }
}

void FileViewer::OnCloseButtonClicked(lv_event_t* e) {
    FileViewer* viewer = static_cast<FileViewer*>(lv_event_get_user_data(e));
    viewer->Close();
}

//=============================================================================
// TextFileViewer
//=============================================================================

TextFileViewer::TextFileViewer() {}

TextFileViewer::~TextFileViewer() {
    FreeContent();
}

void TextFileViewer::Init(lv_obj_t* parent) {
    FileViewer::Init(parent);
    
    // Setup text label in content area
    text_label_ = lv_label_create(content_area_);
    lv_obj_set_width(text_label_, LV_HOR_RES - 48);
    lv_label_set_long_mode(text_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(text_label_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(text_label_, lv_color_hex(0xe0e0e0), 0);
    lv_label_set_text(text_label_, "");
    
    // Enable scrolling in content area
    lv_obj_set_scrollbar_mode(content_area_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(content_area_, LV_DIR_VER);
    
    ESP_LOGI(TAG, "TextFileViewer initialized");
}

void TextFileViewer::FreeContent() {
    if (file_content_) {
        free(file_content_);
        file_content_ = nullptr;
    }
}

bool TextFileViewer::Open(const std::string& path) {
    FreeContent();
    current_file_ = path;
    
    // Get file size
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        ESP_LOGE(TAG, "Failed to stat file: %s", path.c_str());
        return false;
    }
    
    size_t file_size = st.st_size;
    if (file_size > MAX_TEXT_FILE_SIZE) {
        ESP_LOGW(TAG, "File too large (%zu bytes), truncating to %d", file_size, MAX_TEXT_FILE_SIZE);
        file_size = MAX_TEXT_FILE_SIZE;
    }
    
    // Allocate buffer
    file_content_ = (char*)malloc(file_size + 1);
    if (!file_content_) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for file content", file_size + 1);
        return false;
    }
    
    // Read file
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
        FreeContent();
        return false;
    }
    
    size_t bytes_read = fread(file_content_, 1, file_size, f);
    fclose(f);
    
    file_content_[bytes_read] = '\0';
    
    // Update UI
    // Extract filename from path
    const char* filename = strrchr(path.c_str(), '/');
    filename = filename ? filename + 1 : path.c_str();
    lv_label_set_text(title_label_, filename);
    
    lv_label_set_text(text_label_, file_content_);
    
    // Show viewer
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    is_visible_ = true;
    
    // Scroll to top
    lv_obj_scroll_to_y(content_area_, 0, LV_ANIM_OFF);
    
    ESP_LOGI(TAG, "Opened text file: %s (%zu bytes)", path.c_str(), bytes_read);
    return true;
}

void TextFileViewer::Close() {
    FreeContent();
    FileViewer::Close();
}

//=============================================================================
// ImageViewer
//=============================================================================

ImageViewer::ImageViewer() {}

ImageViewer::~ImageViewer() {
    FreeImageData();
}

void ImageViewer::Init(lv_obj_t* parent) {
    FileViewer::Init(parent);
    
    // Center content area setup
    lv_obj_set_style_bg_color(content_area_, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(content_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content_area_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_area_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Image object
    image_obj_ = lv_image_create(content_area_);
    lv_obj_center(image_obj_);
    
    // Enable gesture detection on content area
    lv_obj_add_flag(content_area_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(content_area_, OnSwipeEvent, LV_EVENT_GESTURE, this);
    
    // Previous button (left side)
    prev_btn_ = lv_btn_create(container_);
    lv_obj_set_size(prev_btn_, 50, 80);
    lv_obj_align(prev_btn_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(prev_btn_, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(prev_btn_, LV_OPA_70, 0);
    lv_obj_set_style_radius(prev_btn_, 8, 0);
    lv_obj_add_event_cb(prev_btn_, OnPrevButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* prev_label = lv_label_create(prev_btn_);
    lv_label_set_text(prev_label, LV_SYMBOL_LEFT);
    lv_obj_center(prev_label);
    lv_obj_set_style_text_color(prev_label, lv_color_white(), 0);
    
    // Next button (right side)
    next_btn_ = lv_btn_create(container_);
    lv_obj_set_size(next_btn_, 50, 80);
    lv_obj_align(next_btn_, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(next_btn_, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(next_btn_, LV_OPA_70, 0);
    lv_obj_set_style_radius(next_btn_, 8, 0);
    lv_obj_add_event_cb(next_btn_, OnNextButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* next_label = lv_label_create(next_btn_);
    lv_label_set_text(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);
    lv_obj_set_style_text_color(next_label, lv_color_white(), 0);
    
    // Navigation counter label (e.g., "3/10")
    nav_label_ = lv_label_create(title_bar_);
    lv_obj_align(nav_label_, LV_ALIGN_RIGHT_MID, -60, 0);
    lv_obj_set_style_text_color(nav_label_, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(nav_label_, "");
    
    ESP_LOGI(TAG, "ImageViewer initialized with swipe support");
}

void ImageViewer::FreeImageData() {
    if (image_data_) {
        heap_caps_free(image_data_);
        image_data_ = nullptr;
    }
    memset(&image_dsc_, 0, sizeof(image_dsc_));
}

void ImageViewer::ShowError(const char* message) {
    if (!error_label_) {
        error_label_ = lv_label_create(content_area_);
        lv_obj_set_style_text_color(error_label_, lv_color_hex(0xff6666), 0);
        lv_obj_center(error_label_);
    }
    lv_label_set_text(error_label_, message);
    lv_obj_clear_flag(error_label_, LV_OBJ_FLAG_HIDDEN);
}

bool ImageViewer::Open(const std::string& path) {
    FreeImageData();
    current_file_ = path;
    
    // Hide any previous error
    if (error_label_) {
        lv_obj_add_flag(error_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Extract filename from path
    const char* filename = strrchr(path.c_str(), '/');
    filename = filename ? filename + 1 : path.c_str();
    lv_label_set_text(title_label_, filename);
    
    // Check file size
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        ESP_LOGE(TAG, "Failed to stat image file: %s", path.c_str());
        ShowError("Failed to access file");
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = true;
        return false;
    }
    
    if (st.st_size > MAX_IMAGE_FILE_SIZE) {
        ESP_LOGE(TAG, "Image file too large: %ld bytes", st.st_size);
        ShowError("Image file too large\n(max 2MB)");
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = true;
        return false;
    }

#ifndef CONFIG_IDF_TARGET_ESP32
    // Read the file
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open image file: %s", path.c_str());
        ShowError("Failed to open file");
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = true;
        return false;
    }
    
    // Allocate buffer for file data
    uint8_t* file_data = (uint8_t*)heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!file_data) {
        file_data = (uint8_t*)malloc(st.st_size);
    }
    if (!file_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for file: %zu bytes", (size_t)st.st_size);
        fclose(f);
        ShowError("Out of memory");
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = true;
        return false;
    }
    
    size_t read_len = fread(file_data, 1, st.st_size, f);
    fclose(f);
    
    if (read_len != (size_t)st.st_size) {
        ESP_LOGE(TAG, "Failed to read file: got %zu, expected %zu", read_len, (size_t)st.st_size);
        heap_caps_free(file_data);
        ShowError("Failed to read file");
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = true;
        return false;
    }
    
    // Decode JPEG
    size_t out_len = 0, width = 0, height = 0, stride = 0;
    esp_err_t ret = jpeg_to_image(file_data, read_len, &image_data_, &out_len, &width, &height, &stride);
    heap_caps_free(file_data);  // Free the source file data
    
    if (ret != ESP_OK || !image_data_) {
        ESP_LOGE(TAG, "Failed to decode image: %s (err=%d)", path.c_str(), ret);
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            ShowError("Progressive JPEG\nnot supported");
        } else {
            ShowError("Failed to decode image\nFormat not supported");
        }
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = true;
        return false;
    }
    
    ESP_LOGI(TAG, "Decoded image: %zux%zu, stride=%zu, size=%zu", width, height, stride, out_len);
    
    // Setup LVGL image descriptor
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    image_dsc_.header.w = width;
    image_dsc_.header.h = height;
    image_dsc_.header.stride = stride;
    image_dsc_.data_size = out_len;
    image_dsc_.data = image_data_;
    
    // Set image source
    lv_image_set_src(image_obj_, &image_dsc_);
    
    // Scale to fit screen while maintaining aspect ratio
    int32_t max_w = LV_HOR_RES - 32;
    int32_t max_h = LV_VER_RES - 60 - 32;  // Account for title bar and padding
    
    if ((int32_t)width > max_w || (int32_t)height > max_h) {
        float scale_w = (float)max_w / width;
        float scale_h = (float)max_h / height;
        float scale = (scale_w < scale_h) ? scale_w : scale_h;
        lv_image_set_scale(image_obj_, (uint32_t)(scale * 256));
    } else {
        lv_image_set_scale(image_obj_, 256);  // 1:1 scale
    }
#else
    ShowError("Image viewing not supported\non this platform");
#endif
    
    // Find current index in image list
    current_index_ = -1;
    for (size_t i = 0; i < image_list_.size(); i++) {
        if (image_list_[i] == path) {
            current_index_ = (int)i;
            break;
        }
    }
    UpdateNavUI();
    
    // Show viewer
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    is_visible_ = true;
    
    ESP_LOGI(TAG, "Opened image: %s (%d/%zu)", path.c_str(), current_index_ + 1, image_list_.size());
    return true;
}

void ImageViewer::Close() {
    FreeImageData();
    lv_image_set_src(image_obj_, NULL);
    image_list_.clear();
    current_index_ = -1;
    FileViewer::Close();
}

void ImageViewer::SetImageList(const std::vector<std::string>& images) {
    image_list_ = images;
    current_index_ = -1;
    UpdateNavUI();
}

void ImageViewer::NavigatePrev() {
    if (image_list_.empty() || current_index_ <= 0) {
        return;
    }
    
    int new_index = current_index_ - 1;
    const std::string& new_path = image_list_[new_index];
    
    if (navigate_callback_) {
        navigate_callback_(new_path);
    } else {
        Open(new_path);
    }
}

void ImageViewer::NavigateNext() {
    if (image_list_.empty() || current_index_ < 0 || 
        current_index_ >= (int)image_list_.size() - 1) {
        return;
    }
    
    int new_index = current_index_ + 1;
    const std::string& new_path = image_list_[new_index];
    
    if (navigate_callback_) {
        navigate_callback_(new_path);
    } else {
        Open(new_path);
    }
}

void ImageViewer::UpdateNavUI() {
    bool has_list = !image_list_.empty() && current_index_ >= 0;
    
    // Update navigation buttons visibility
    if (prev_btn_) {
        if (has_list && current_index_ > 0) {
            lv_obj_clear_flag(prev_btn_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(prev_btn_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    if (next_btn_) {
        if (has_list && current_index_ < (int)image_list_.size() - 1) {
            lv_obj_clear_flag(next_btn_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(next_btn_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // Update counter label
    if (nav_label_) {
        if (has_list && image_list_.size() > 1) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d/%zu", current_index_ + 1, image_list_.size());
            lv_label_set_text(nav_label_, buf);
        } else {
            lv_label_set_text(nav_label_, "");
        }
    }
}

void ImageViewer::OnSwipeEvent(lv_event_t* e) {
    ImageViewer* viewer = static_cast<ImageViewer*>(lv_event_get_user_data(e));
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    
    if (dir == LV_DIR_LEFT) {
        // Swipe left -> next image
        viewer->NavigateNext();
    } else if (dir == LV_DIR_RIGHT) {
        // Swipe right -> previous image
        viewer->NavigatePrev();
    }
}

void ImageViewer::OnPrevButtonClicked(lv_event_t* e) {
    ImageViewer* viewer = static_cast<ImageViewer*>(lv_event_get_user_data(e));
    viewer->NavigatePrev();
}

void ImageViewer::OnNextButtonClicked(lv_event_t* e) {
    ImageViewer* viewer = static_cast<ImageViewer*>(lv_event_get_user_data(e));
    viewer->NavigateNext();
}

//=============================================================================
// AudioPlayer
//=============================================================================

// Audio player callback and state for MP3/WAV playback
#ifdef AUDIO_PLAYER_SUPPORTED
static AudioPlayer* g_current_audio_player = nullptr;
static bool g_audio_player_initialized = false;
static int g_original_sample_rate = 0;  // Store original codec sample rate for restoration
static int g_original_channels = 0;     // Store original codec channel count for restoration

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->EnableOutput(setting == AUDIO_PLAYER_UNMUTE);
    }
    return ESP_OK;
}

static void audio_player_event_callback(audio_player_cb_ctx_t* ctx) {
    ESP_LOGI(TAG, "Audio player event: %d", (int)ctx->audio_event);
    
    audio_player_state_t state = audio_player_get_state();
    if (state == AUDIO_PLAYER_STATE_IDLE && g_current_audio_player) {
        // Playback finished - just disable output, don't restore sample rate
        // (audio_player caches rate internally - restoring causes mismatch on next play)
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->EnableOutput(false);
        }
        
        // Resume AudioService's power timer
        Application::GetInstance().GetAudioService().ResumePowerTimer();
        
        g_current_audio_player->is_playing_ = false;
        ESP_LOGI(TAG, "Audio playback completed");
    }
}

// I2S write wrapper for esp-audio-player
static esp_err_t audio_i2s_write(void* data, size_t size, size_t* bytes_written, uint32_t timeout_ms) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        // Enable output if not already enabled
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        // Convert raw bytes to int16_t samples and use public OutputData method
        int samples = size / sizeof(int16_t);
        std::vector<int16_t> audio_buffer((int16_t*)data, (int16_t*)data + samples);
        codec->OutputData(audio_buffer);
        *bytes_written = size;
        return ESP_OK;
    }
    *bytes_written = 0;
    return ESP_FAIL;
}

// I2S clock reconfiguration wrapper
static esp_err_t audio_i2s_reconfig_clk(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        // Save original settings before changing (only on first call per playback)
        if (g_original_sample_rate == 0) {
            g_original_sample_rate = codec->output_sample_rate();
            g_original_channels = codec->output_channels();
            ESP_LOGI(TAG, "Saved original codec settings: %d Hz, %d channels", 
                     g_original_sample_rate, g_original_channels);
        }
        
        // Reconfigure to match file's sample rate
        if (codec->SetOutputSampleRate((int)rate)) {
            ESP_LOGI(TAG, "Audio codec sample rate set to %lu Hz", rate);
        } else {
            ESP_LOGW(TAG, "Sample rate mismatch: file=%lu Hz, codec=%d Hz", 
                     rate, codec->output_sample_rate());
        }
        
        // Reconfigure to match file's channel count (mono=1, stereo=2)
        int channels = (ch == I2S_SLOT_MODE_STEREO) ? 2 : 1;
        if (codec->SetOutputChannels(channels)) {
            ESP_LOGI(TAG, "Audio codec channels set to %d", channels);
        } else {
            ESP_LOGW(TAG, "Channel mismatch: file=%d ch, codec=%d ch", 
                     channels, codec->output_channels());
        }
    }
    ESP_LOGI(TAG, "Audio player requested clock: rate=%lu, bits=%lu, ch=%d", rate, bits_cfg, (int)ch);
    return ESP_OK;
}
#endif

AudioPlayer::AudioPlayer() {}

AudioPlayer::~AudioPlayer() {
    audio_data_.clear();
#ifdef AUDIO_PLAYER_SUPPORTED
    // Stop playback if still playing - this handles file closing for played files
    StopMp3WavFile();
    // If we opened a file but never played it, close it ourselves
    if (audio_file_) {
        fclose(audio_file_);
        audio_file_ = nullptr;
    }
    if (g_current_audio_player == this) {
        g_current_audio_player = nullptr;
    }
    
    // Restore original sample rate and channels when leaving file browser
    // (for voice input to work correctly)
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        if (g_original_sample_rate > 0 && codec->output_sample_rate() != g_original_sample_rate) {
            ESP_LOGI(TAG, "Restoring original sample rate: %d Hz", g_original_sample_rate);
            codec->SetOutputSampleRate(g_original_sample_rate);
        }
        if (g_original_channels > 0 && codec->output_channels() != g_original_channels) {
            ESP_LOGI(TAG, "Restoring original channels: %d", g_original_channels);
            codec->SetOutputChannels(g_original_channels);
        }
    }
    g_original_sample_rate = 0;
    g_original_channels = 0;
#endif
}

void AudioPlayer::Init(lv_obj_t* parent) {
    FileViewer::Init(parent);
    
    // Center content setup
    lv_obj_clear_flag(content_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content_area_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_area_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content_area_, 20, 0);
    
    // File info icon
    lv_obj_t* icon = lv_label_create(content_area_);
    lv_label_set_text(icon, FONT_AWESOME_MUSIC);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xffa500), 0);
    
    // File info label
    file_info_label_ = lv_label_create(content_area_);
    lv_obj_set_width(file_info_label_, LV_HOR_RES - 100);
    lv_label_set_long_mode(file_info_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(file_info_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(file_info_label_, lv_color_white(), 0);
    lv_label_set_text(file_info_label_, "");
    
    // Progress bar
    progress_bar_ = lv_bar_create(content_area_);
    lv_obj_set_size(progress_bar_, LV_HOR_RES - 100, 8);
    lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0xffa500), LV_PART_INDICATOR);
    
    // Time label
    time_label_ = lv_label_create(content_area_);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(0x888888), 0);
    lv_label_set_text(time_label_, "");
    
    // Control buttons container
    lv_obj_t* btn_container = lv_obj_create(content_area_);
    lv_obj_set_size(btn_container, 200, 60);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_container, 0, 0);
    lv_obj_set_style_pad_all(btn_container, 0, 0);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_container, 20, 0);
    
    // Play button
    play_btn_ = lv_btn_create(btn_container);
    lv_obj_set_size(play_btn_, 60, 60);
    lv_obj_set_style_bg_color(play_btn_, lv_color_hex(0x4caf50), 0);
    lv_obj_set_style_radius(play_btn_, 30, 0);
    lv_obj_add_event_cb(play_btn_, OnPlayButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* play_label = lv_label_create(play_btn_);
    lv_label_set_text(play_label, FONT_AWESOME_PLAY);
    lv_obj_center(play_label);
    lv_obj_set_style_text_color(play_label, lv_color_white(), 0);
    
    // Stop button
    stop_btn_ = lv_btn_create(btn_container);
    lv_obj_set_size(stop_btn_, 60, 60);
    lv_obj_set_style_bg_color(stop_btn_, lv_color_hex(0xf44336), 0);
    lv_obj_set_style_radius(stop_btn_, 30, 0);
    lv_obj_add_event_cb(stop_btn_, OnStopButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* stop_label = lv_label_create(stop_btn_);
    lv_label_set_text(stop_label, FONT_AWESOME_STOP);
    lv_obj_center(stop_label);
    lv_obj_set_style_text_color(stop_label, lv_color_white(), 0);
    
    // Info message label
    info_label_ = lv_label_create(content_area_);
    lv_obj_set_width(info_label_, LV_HOR_RES - 60);
    lv_obj_set_style_text_align(info_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(info_label_, lv_color_hex(0x666666), 0);
    lv_label_set_text(info_label_, "");
    
    ESP_LOGI(TAG, "AudioPlayer initialized");
}

void AudioPlayer::SetInfoMessage(const char* msg, lv_color_t color) {
    if (info_label_) {
        lv_label_set_text(info_label_, msg);
        lv_obj_set_style_text_color(info_label_, color, 0);
    }
}

AudioFormat AudioPlayer::DetectAudioFormat(const std::string& filename) {
    std::string name_lower = filename;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
    
    if (name_lower.size() >= 4) {
        std::string ext = name_lower.substr(name_lower.size() - 4);
        if (ext == ".ogg") return AudioFormat::kOgg;
        if (ext == ".mp3") return AudioFormat::kMp3;
        if (ext == ".wav") return AudioFormat::kWav;
    }
    return AudioFormat::kUnknown;
}

const char* AudioPlayer::GetFormatName(AudioFormat format) {
    switch (format) {
        case AudioFormat::kOgg: return "OGG";
        case AudioFormat::kMp3: return "MP3";
        case AudioFormat::kWav: return "WAV";
        default: return "Unknown";
    }
}

bool AudioPlayer::PlayOggFile() {
    if (audio_data_.empty()) {
        SetInfoMessage("No audio data loaded", lv_color_hex(0xff6666));
        return false;
    }
    
    auto& app = Application::GetInstance();
    std::string_view audio_view(reinterpret_cast<const char*>(audio_data_.data()), audio_data_.size());
    app.GetAudioService().PlaySound(audio_view);
    
    SetInfoMessage("Playing...", lv_color_hex(0x4caf50));
    is_playing_ = true;
    lv_bar_set_value(progress_bar_, 100, LV_ANIM_ON);
    
    ESP_LOGI(TAG, "Playing OGG file: %zu bytes", audio_data_.size());
    return true;
}

bool AudioPlayer::PlayMp3WavFile() {
#ifdef AUDIO_PLAYER_SUPPORTED
    // Re-open file if it was closed by previous stop (audio_player closes it)
    if (!audio_file_ && !current_file_.empty()) {
        audio_file_ = fopen(current_file_.c_str(), "rb");
        if (!audio_file_) {
            ESP_LOGE(TAG, "Failed to re-open file: %s", current_file_.c_str());
            SetInfoMessage("Failed to open file", lv_color_hex(0xff6666));
            return false;
        }
        ESP_LOGI(TAG, "Re-opened file for playback: %s", current_file_.c_str());
    }
    
    if (!audio_file_) {
        SetInfoMessage("No audio file loaded", lv_color_hex(0xff6666));
        return false;
    }
    
    // Initialize audio player if not already done
    if (!g_audio_player_initialized) {
        audio_player_config_t config = {
            .mute_fn = audio_mute_function,
            .clk_set_fn = audio_i2s_reconfig_clk,
            .write_fn = audio_i2s_write,
            .priority = 5,
            .coreID = 1,
        };
        
        esp_err_t ret = audio_player_new(config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create audio player: %s", esp_err_to_name(ret));
            SetInfoMessage("Audio player init failed", lv_color_hex(0xff6666));
            return false;
        }
        
        audio_player_callback_register(audio_player_event_callback, NULL);
        g_audio_player_initialized = true;
        ESP_LOGI(TAG, "Audio player initialized");
    }
    
    // Reset file position to beginning (library handles ID3 tag skipping)
    fseek(audio_file_, 0, SEEK_SET);
    
    // Suspend AudioService's power timer FIRST to prevent race condition
    Application::GetInstance().GetAudioService().SuspendPowerTimer();
    
    // Enable audio output before starting playback
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->EnableOutput(true);
    }
    
    g_current_audio_player = this;
    
    esp_err_t ret = audio_player_play(audio_file_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start playback: %s", esp_err_to_name(ret));
        SetInfoMessage("Playback failed", lv_color_hex(0xff6666));
        return false;
    }
    
    SetInfoMessage("Playing...", lv_color_hex(0x4caf50));
    is_playing_ = true;
    lv_bar_set_value(progress_bar_, 100, LV_ANIM_ON);
    
    ESP_LOGI(TAG, "Started %s playback", GetFormatName(audio_format_));
    return true;
#else
    SetInfoMessage("MP3/WAV not supported\non this platform", lv_color_hex(0xff6666));
    return false;
#endif
}

void AudioPlayer::StopMp3WavFile() {
#ifdef AUDIO_PLAYER_SUPPORTED
    if (g_audio_player_initialized && is_playing_) {
        audio_player_stop();
        
        // Wait for the player to actually stop (it handles file closing)
        int timeout = 100; // 1 second max
        while (audio_player_get_state() != AUDIO_PLAYER_STATE_IDLE && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout--;
        }
        
        // The file handle is now invalid (audio_player closed it)
        audio_file_ = nullptr;
        
        // Resume AudioService's power timer now that playback is done
        Application::GetInstance().GetAudioService().ResumePowerTimer();
        
        // Don't restore sample rate here - audio_player caches it internally
        // and won't call reconfig_clk for next file at same rate.
        // Sample rate will be restored in destructor when leaving file browser.
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->EnableOutput(false);
        }
        is_playing_ = false;
        ESP_LOGI(TAG, "Stopped MP3/WAV playback");
    }
#endif
}

bool AudioPlayer::Open(const std::string& path) {
    current_file_ = path;
    audio_data_.clear();
    audio_format_ = AudioFormat::kUnknown;
    is_playing_ = false;
    
#ifdef AUDIO_PLAYER_SUPPORTED
    // Stop any ongoing playback first (audio_player handles file closing)
    StopMp3WavFile();
    // If we had a file open but never started playback, close it ourselves
    if (audio_file_) {
        fclose(audio_file_);
        audio_file_ = nullptr;
    }
#endif
    
    // Extract filename
    const char* filename = strrchr(path.c_str(), '/');
    filename = filename ? filename + 1 : path.c_str();
    lv_label_set_text(title_label_, filename);
    lv_label_set_text(file_info_label_, filename);
    
    // Reset UI state
    lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
    lv_label_set_text(time_label_, "");
    
    // Detect format
    audio_format_ = DetectAudioFormat(filename);
    
    // Check file size
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        SetInfoMessage("Failed to access file", lv_color_hex(0xff6666));
        goto show_and_return;
    }
    
    if (audio_format_ == AudioFormat::kOgg) {
        // OGG files are loaded into memory, so check size limit
        if (st.st_size > MAX_AUDIO_FILE_SIZE) {
            SetInfoMessage("File too large (max 4MB)", lv_color_hex(0xff6666));
            goto show_and_return;
        }
        
        // Read OGG file into memory for AudioService
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            SetInfoMessage("Failed to open file", lv_color_hex(0xff6666));
            goto show_and_return;
        }
        
        audio_data_.resize(st.st_size);
        size_t read_len = fread(audio_data_.data(), 1, st.st_size, f);
        fclose(f);
        
        if (read_len != (size_t)st.st_size) {
            audio_data_.clear();
            SetInfoMessage("Failed to read file", lv_color_hex(0xff6666));
            goto show_and_return;
        }
        
        ESP_LOGI(TAG, "Loaded OGG file: %s (%zu bytes)", path.c_str(), audio_data_.size());
    }
#ifdef AUDIO_PLAYER_SUPPORTED
    else if (audio_format_ == AudioFormat::kMp3 || audio_format_ == AudioFormat::kWav) {
        // Open file for streaming playback
        audio_file_ = fopen(path.c_str(), "rb");
        if (!audio_file_) {
            SetInfoMessage("Failed to open file", lv_color_hex(0xff6666));
            goto show_and_return;
        }
        
        ESP_LOGI(TAG, "Opened %s file: %s (%ld bytes)", GetFormatName(audio_format_), path.c_str(), st.st_size);
    }
#endif
    else {
        SetInfoMessage("Unsupported audio format", lv_color_hex(0xffaa00));
        goto show_and_return;
    }
    
    // Show file size info
    {
        char size_str[64];
        const char* fmt_name = GetFormatName(audio_format_);
        if (st.st_size < 1024) {
            snprintf(size_str, sizeof(size_str), "%s file - %ld bytes", fmt_name, st.st_size);
        } else if (st.st_size < 1024 * 1024) {
            snprintf(size_str, sizeof(size_str), "%s file - %.1f KB", fmt_name, st.st_size / 1024.0);
        } else {
            snprintf(size_str, sizeof(size_str), "%s file - %.1f MB", fmt_name, st.st_size / (1024.0 * 1024.0));
        }
        lv_label_set_text(time_label_, size_str);
        SetInfoMessage("Press play to start", lv_color_hex(0x4caf50));
    }
    
show_and_return:
    // Show player
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    is_visible_ = true;
    
    bool file_ready = (audio_format_ == AudioFormat::kOgg && !audio_data_.empty());
#ifdef AUDIO_PLAYER_SUPPORTED
    file_ready = file_ready || ((audio_format_ == AudioFormat::kMp3 || audio_format_ == AudioFormat::kWav) && audio_file_ != nullptr);
#endif
    return file_ready;
}

void AudioPlayer::Close() {
    StopMp3WavFile();
    is_playing_ = false;
    audio_data_.clear();
    audio_format_ = AudioFormat::kUnknown;
    
#ifdef AUDIO_PLAYER_SUPPORTED
    // audio_file_ is closed by audio_player when playing, but if we opened
    // a file without playing it, we need to close it ourselves
    if (audio_file_) {
        fclose(audio_file_);
        audio_file_ = nullptr;
    }
    if (g_current_audio_player == this) {
        g_current_audio_player = nullptr;
    }
    
    // Restore original sample rate and channels when closing audio viewer
    // (for voice input to work correctly when returning to main UI)
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        if (g_original_sample_rate > 0 && codec->output_sample_rate() != g_original_sample_rate) {
            ESP_LOGI(TAG, "Restoring original sample rate: %d Hz", g_original_sample_rate);
            codec->SetOutputSampleRate(g_original_sample_rate);
        }
        if (g_original_channels > 0 && codec->output_channels() != g_original_channels) {
            ESP_LOGI(TAG, "Restoring original channels: %d", g_original_channels);
            codec->SetOutputChannels(g_original_channels);
        }
    }
    g_original_sample_rate = 0;
    g_original_channels = 0;
#endif
    
    FileViewer::Close();
}

void AudioPlayer::OnPlayButtonClicked(lv_event_t* e) {
    AudioPlayer* player = static_cast<AudioPlayer*>(lv_event_get_user_data(e));
    
    if (player->audio_format_ == AudioFormat::kUnknown) {
        player->SetInfoMessage("Format not supported", lv_color_hex(0xff6666));
        return;
    }
    
    if (!player->is_playing_) {
        bool success = false;
        if (player->audio_format_ == AudioFormat::kOgg) {
            success = player->PlayOggFile();
        } else {
            success = player->PlayMp3WavFile();
        }
        if (!success) {
            ESP_LOGE(TAG, "Failed to start playback");
        }
    } else {
        // Audio is already playing
        player->SetInfoMessage("Audio is playing...", lv_color_hex(0x4caf50));
    }
}

void AudioPlayer::OnStopButtonClicked(lv_event_t* e) {
    AudioPlayer* player = static_cast<AudioPlayer*>(lv_event_get_user_data(e));
    
    if (player->audio_format_ == AudioFormat::kMp3 || player->audio_format_ == AudioFormat::kWav) {
        player->StopMp3WavFile();
    }
    
    player->is_playing_ = false;
    lv_bar_set_value(player->progress_bar_, 0, LV_ANIM_ON);
    player->SetInfoMessage("Stopped", lv_color_hex(0x888888));
    
    ESP_LOGI(TAG, "Playback stopped");
}

//=============================================================================
// VideoPlayer
//=============================================================================

VideoPlayer::VideoPlayer() {}

VideoPlayer::~VideoPlayer() {}

void VideoPlayer::Init(lv_obj_t* parent) {
    FileViewer::Init(parent);
    
    // Center content
    lv_obj_clear_flag(content_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content_area_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_area_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content_area_, 20, 0);
    
    // Video icon
    lv_obj_t* icon = lv_label_create(content_area_);
    lv_label_set_text(icon, FONT_AWESOME_PLAY);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xff6347), 0);
    
    // Info label
    info_label_ = lv_label_create(content_area_);
    lv_obj_set_width(info_label_, LV_HOR_RES - 100);
    lv_obj_set_style_text_align(info_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(info_label_, lv_color_white(), 0);
    lv_label_set_text(info_label_, "");
    
    // Limitation message
    lv_obj_t* msg = lv_label_create(content_area_);
    lv_obj_set_width(msg, LV_HOR_RES - 60);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0x888888), 0);
    lv_label_set_text(msg, "Video playback is limited on ESP32.\n"
                          "Consider using external video decoding\n"
                          "or converting to supported formats.");
    
    ESP_LOGI(TAG, "VideoPlayer initialized");
}

bool VideoPlayer::Open(const std::string& path) {
    current_file_ = path;
    
    // Extract filename
    const char* filename = strrchr(path.c_str(), '/');
    filename = filename ? filename + 1 : path.c_str();
    lv_label_set_text(title_label_, filename);
    lv_label_set_text(info_label_, filename);
    
    // Show player
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    is_visible_ = true;
    
    ESP_LOGI(TAG, "Opened video file: %s", path.c_str());
    return true;
}

void VideoPlayer::Close() {
    FileViewer::Close();
}

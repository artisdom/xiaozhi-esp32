#include "file_viewer.h"
#include <font_awesome.h>
#include <esp_log.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

static const char* TAG = "FileViewer";

// Maximum file size to load (64KB for text files)
#define MAX_TEXT_FILE_SIZE (64 * 1024)
// Maximum image file size (2MB)
#define MAX_IMAGE_FILE_SIZE (2 * 1024 * 1024)

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
    
    ESP_LOGI(TAG, "ImageViewer initialized");
}

void ImageViewer::FreeImageData() {
    if (image_data_) {
        free(image_data_);
        image_data_ = nullptr;
    }
}

bool ImageViewer::Open(const std::string& path) {
    FreeImageData();
    current_file_ = path;
    
    // Extract filename from path
    const char* filename = strrchr(path.c_str(), '/');
    filename = filename ? filename + 1 : path.c_str();
    lv_label_set_text(title_label_, filename);
    
    // Check file size
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        ESP_LOGE(TAG, "Failed to stat image file: %s", path.c_str());
        return false;
    }
    
    if (st.st_size > MAX_IMAGE_FILE_SIZE) {
        ESP_LOGE(TAG, "Image file too large: %ld bytes", st.st_size);
        return false;
    }
    
    // For LVGL 9, we need to use lv_image_set_src with a file path
    // Prefix with 'SD:' for SD card files
    std::string lvgl_path = "S:" + path;
    lv_image_set_src(image_obj_, lvgl_path.c_str());
    
    // Check if image loaded successfully
    const lv_image_dsc_t* img_dsc = (const lv_image_dsc_t*)lv_image_get_src(image_obj_);
    if (!img_dsc) {
        ESP_LOGE(TAG, "Failed to load image: %s", path.c_str());
        // Show error message
        lv_obj_t* error_label = lv_label_create(content_area_);
        lv_label_set_text(error_label, "Failed to load image\nFormat may not be supported");
        lv_obj_set_style_text_color(error_label, lv_color_hex(0xff6666), 0);
        lv_obj_center(error_label);
    }
    
    // Show viewer
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    is_visible_ = true;
    
    ESP_LOGI(TAG, "Opened image: %s", path.c_str());
    return true;
}

void ImageViewer::Close() {
    FreeImageData();
    lv_image_set_src(image_obj_, NULL);
    FileViewer::Close();
}

//=============================================================================
// AudioPlayer
//=============================================================================

AudioPlayer::AudioPlayer() {}

AudioPlayer::~AudioPlayer() {}

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
    lv_label_set_text(time_label_, "0:00 / 0:00");
    
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
    
    // Info message
    lv_obj_t* info_msg = lv_label_create(content_area_);
    lv_obj_set_style_text_color(info_msg, lv_color_hex(0x666666), 0);
    lv_label_set_text(info_msg, "(Audio playback requires system audio service)");
    
    ESP_LOGI(TAG, "AudioPlayer initialized");
}

bool AudioPlayer::Open(const std::string& path) {
    current_file_ = path;
    
    // Extract filename
    const char* filename = strrchr(path.c_str(), '/');
    filename = filename ? filename + 1 : path.c_str();
    lv_label_set_text(title_label_, filename);
    lv_label_set_text(file_info_label_, filename);
    
    // Reset UI state
    lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);
    lv_label_set_text(time_label_, "0:00 / 0:00");
    is_playing_ = false;
    
    // Show player
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    is_visible_ = true;
    
    ESP_LOGI(TAG, "Opened audio file: %s", path.c_str());
    return true;
}

void AudioPlayer::Close() {
    is_playing_ = false;
    // TODO: Stop any playing audio through audio service
    FileViewer::Close();
}

void AudioPlayer::OnPlayButtonClicked(lv_event_t* e) {
    AudioPlayer* player = static_cast<AudioPlayer*>(lv_event_get_user_data(e));
    player->is_playing_ = !player->is_playing_;
    
    // TODO: Integrate with audio service for actual playback
    ESP_LOGI(TAG, "Play/Pause toggled: %s", player->is_playing_ ? "playing" : "paused");
}

void AudioPlayer::OnStopButtonClicked(lv_event_t* e) {
    AudioPlayer* player = static_cast<AudioPlayer*>(lv_event_get_user_data(e));
    player->is_playing_ = false;
    lv_bar_set_value(player->progress_bar_, 0, LV_ANIM_ON);
    
    // TODO: Stop audio playback through audio service
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

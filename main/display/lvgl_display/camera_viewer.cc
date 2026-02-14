#include "camera_viewer.h"
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <dirent.h>
#include <ctime>

#include "board.h"
#include "camera.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/ppa.h"
#endif

static const char* TAG = "CameraViewer";

// Camera preview resolution - kept small to avoid DSI underrun
// Full camera resolution is still used for photo capture
#define CAMERA_PREVIEW_WIDTH  480
#define CAMERA_PREVIEW_HEIGHT 320

// SD card camera folder
#define CAMERA_FOLDER "/sdcard/Camera"

// Task configuration
#define CAMERA_TASK_STACK_SIZE (8 * 1024)
#define CAMERA_TASK_PRIORITY   3  // Lower priority to not starve LCD refresh
#define COMMAND_QUEUE_SIZE     10
#define CAMERA_FRAME_DELAY_MS  50  // ~20fps to reduce PSRAM bandwidth usage

//=============================================================================
// CameraViewer implementation
//=============================================================================

CameraViewer::CameraViewer() {
    command_queue_ = xQueueCreate(COMMAND_QUEUE_SIZE, sizeof(CameraCommand));
    if (command_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create command queue");
    }
}

CameraViewer::~CameraViewer() {
    // Ensure task is stopped
    if (task_running_.load()) {
        CameraCommand cmd = CameraCommand::EXIT;
        xQueueSend(command_queue_, &cmd, portMAX_DELAY);
        // Wait for task to exit
        while (task_running_.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    if (command_queue_) {
        vQueueDelete(command_queue_);
        command_queue_ = nullptr;
    }
    
    if (preview_buffer_) {
        heap_caps_free(preview_buffer_);
        preview_buffer_ = nullptr;
    }
    
    if (container_) {
        lv_obj_delete(container_);
        container_ = nullptr;
    }
}

void CameraViewer::Init(lv_obj_t* parent) {
    if (container_) return;
    if (parent == nullptr) {
        parent = lv_screen_active();
    }
    CreateUI(parent);
    ESP_LOGI(TAG, "CameraViewer initialized");
}

void CameraViewer::CreateUI(lv_obj_t* parent) {
    // Main fullscreen container
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    // Camera preview canvas - centered in container
    preview_canvas_ = lv_canvas_create(container_);
    lv_obj_align(preview_canvas_, LV_ALIGN_CENTER, 0, 0);
    
    // Close button (top right corner)
    close_btn_ = lv_btn_create(container_);
    lv_obj_set_size(close_btn_, 50, 50);
    lv_obj_align(close_btn_, LV_ALIGN_TOP_RIGHT, -16, 16);
    lv_obj_set_style_bg_color(close_btn_, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(close_btn_, LV_OPA_70, 0);
    lv_obj_set_style_radius(close_btn_, 25, 0);
    lv_obj_set_style_border_width(close_btn_, 0, 0);
    lv_obj_add_event_cb(close_btn_, OnCloseButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* close_label = lv_label_create(close_btn_);
    lv_label_set_text(close_label, FONT_AWESOME_XMARK);
    lv_obj_center(close_label);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);

    // Capture button (bottom center)
    capture_btn_ = lv_btn_create(container_);
    lv_obj_set_size(capture_btn_, 80, 80);
    lv_obj_align(capture_btn_, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_style_bg_color(capture_btn_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(capture_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(capture_btn_, 40, 0);
    lv_obj_set_style_border_color(capture_btn_, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(capture_btn_, 4, 0);
    lv_obj_add_event_cb(capture_btn_, OnCaptureButtonClicked, LV_EVENT_CLICKED, this);
    
    // Inner circle for capture button (camera style shutter)
    lv_obj_t* inner_circle = lv_obj_create(capture_btn_);
    lv_obj_set_size(inner_circle, 60, 60);
    lv_obj_center(inner_circle);
    lv_obj_set_style_bg_color(inner_circle, lv_color_white(), 0);
    lv_obj_set_style_radius(inner_circle, 30, 0);
    lv_obj_set_style_border_color(inner_circle, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_border_width(inner_circle, 2, 0);
    lv_obj_clear_flag(inner_circle, LV_OBJ_FLAG_CLICKABLE);

    // Gallery button (bottom left) - to navigate to Camera folder
    gallery_btn_ = lv_btn_create(container_);
    lv_obj_set_size(gallery_btn_, 60, 60);
    lv_obj_align(gallery_btn_, LV_ALIGN_BOTTOM_LEFT, 32, -42);
    lv_obj_set_style_bg_color(gallery_btn_, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(gallery_btn_, LV_OPA_70, 0);
    lv_obj_set_style_radius(gallery_btn_, 12, 0);
    lv_obj_set_style_border_width(gallery_btn_, 0, 0);
    lv_obj_add_event_cb(gallery_btn_, OnGalleryButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* gallery_label = lv_label_create(gallery_btn_);
    lv_label_set_text(gallery_label, FONT_AWESOME_IMAGE);
    lv_obj_center(gallery_label);
    lv_obj_set_style_text_color(gallery_label, lv_color_white(), 0);

    // Status label (top center)
    status_label_ = lv_label_create(container_);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(status_label_, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(status_label_, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(status_label_, 8, 0);
    lv_obj_set_style_radius(status_label_, 8, 0);
    lv_label_set_text(status_label_, "");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
}

void CameraViewer::Show() {
    if (!container_) {
        ESP_LOGE(TAG, "CameraViewer not initialized");
        return;
    }
    
    // Create Camera folder if it doesn't exist
    struct stat st;
    if (stat(CAMERA_FOLDER, &st) != 0) {
        if (mkdir(CAMERA_FOLDER, 0755) != 0) {
            ESP_LOGW(TAG, "Failed to create Camera folder: %s", CAMERA_FOLDER);
        } else {
            ESP_LOGI(TAG, "Created Camera folder: %s", CAMERA_FOLDER);
        }
    }
    
    is_visible_.store(true);
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    
    // Start camera preview
    StartPreview();
}

void CameraViewer::Hide() {
    if (!container_) return;
    
    // Stop camera preview first
    StopPreview();
    
    is_visible_.store(false);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    
    if (close_callback_) {
        close_callback_();
    }
}

void CameraViewer::StartPreview() {
#if CONFIG_IDF_TARGET_ESP32P4
    if (task_running_.load()) {
        ESP_LOGW(TAG, "Camera task already running");
        return;
    }
    
    // Check if board has camera
    Camera* camera = Board::GetInstance().GetCamera();
    if (!camera || !camera->IsReady()) {
        ESP_LOGE(TAG, "Camera not available or not ready");
        ShowStatus("Camera not available", 3000);
        return;
    }
    
    // Allocate preview buffer if not already allocated
    if (!preview_buffer_) {
        preview_width_ = CAMERA_PREVIEW_WIDTH;
        preview_height_ = CAMERA_PREVIEW_HEIGHT;
        preview_buffer_size_ = preview_width_ * preview_height_ * 2; // RGB565
        preview_buffer_ = (uint8_t*)heap_caps_calloc(preview_buffer_size_, 1, 
                                                      MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        if (!preview_buffer_) {
            ESP_LOGE(TAG, "Failed to allocate preview buffer");
            return;
        }
    }
    
    // Set canvas buffer
    lv_canvas_set_buffer(preview_canvas_, preview_buffer_, 
                         preview_width_, preview_height_, 
                         LV_COLOR_FORMAT_RGB565);
    
    // Create camera task
    xTaskCreatePinnedToCore(CameraTaskFunc, "camera_preview", 
                            CAMERA_TASK_STACK_SIZE, this, 
                            CAMERA_TASK_PRIORITY, &camera_task_handle_, 1);
    
    // Send start command
    CameraCommand cmd = CameraCommand::START;
    xQueueSend(command_queue_, &cmd, portMAX_DELAY);
#else
    ESP_LOGW(TAG, "Camera preview not supported on this platform");
    ShowStatus("Camera not supported", 3000);
#endif
}

void CameraViewer::StopPreview() {
#if CONFIG_IDF_TARGET_ESP32P4
    if (!task_running_.load()) {
        return;
    }
    
    // Send stop command
    CameraCommand cmd = CameraCommand::STOP;
    xQueueSend(command_queue_, &cmd, portMAX_DELAY);
    
    // Send exit command
    cmd = CameraCommand::EXIT;
    xQueueSend(command_queue_, &cmd, portMAX_DELAY);
    
    // Wait for task to finish
    int timeout = 100; // 1 second timeout
    while (task_running_.load() && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout--;
    }
    
    if (task_running_.load()) {
        ESP_LOGW(TAG, "Camera task didn't exit gracefully");
    }
#endif
}

#if CONFIG_IDF_TARGET_ESP32P4
void CameraViewer::CameraTaskFunc(void* arg) {
    CameraViewer* viewer = static_cast<CameraViewer*>(arg);
    viewer->task_running_.store(true);
    
    ESP_LOGI(TAG, "Camera task started");
    
    // Get camera from board
    Camera* camera = Board::GetInstance().GetCamera();
    if (!camera || !camera->IsReady()) {
        ESP_LOGE(TAG, "Camera not available");
        viewer->task_running_.store(false);
        vTaskDelete(nullptr);
        return;
    }
    
    // Initialize PPA for scaling/rotation
    ppa_client_handle_t ppa_handle = nullptr;
    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ppa_register_client(&ppa_config, &ppa_handle);
    
    bool streaming = false;
    bool should_exit = false;
    
    while (!should_exit) {
        // Check for commands
        CameraCommand cmd;
        if (xQueueReceive(viewer->command_queue_, &cmd, 0) == pdPASS) {
            switch (cmd) {
                case CameraCommand::START:
                    streaming = true;
                    ESP_LOGI(TAG, "Preview started");
                    break;
                case CameraCommand::STOP:
                    streaming = false;
                    ESP_LOGI(TAG, "Preview stopped");
                    break;
                case CameraCommand::CAPTURE:
                    viewer->is_capturing_.store(true);
                    break;
                case CameraCommand::EXIT:
                    should_exit = true;
                    break;
                default:
                    break;
            }
        }
        
        if (!streaming || should_exit) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // Get frame from board's camera
        uint8_t* frame_data = nullptr;
        size_t frame_len = 0;
        uint16_t frame_width = 0;
        uint16_t frame_height = 0;
        
        if (!camera->GetPreviewFrame(&frame_data, &frame_len, &frame_width, &frame_height)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Check if we need to capture a photo
        if (viewer->is_capturing_.load()) {
            viewer->is_capturing_.store(false);
            
            // Save photo using camera's SaveJpegToFile
            std::string filename = viewer->GeneratePhotoFilename();
            std::string filepath = std::string(CAMERA_FOLDER) + "/" + filename;
            
            if (camera->SaveJpegToFile(filepath, 90)) {
                viewer->ShowStatus("Photo saved!", 2000);
                ESP_LOGI(TAG, "Photo saved: %s", filepath.c_str());
            } else {
                viewer->ShowStatus("Failed to save photo", 2000);
                ESP_LOGE(TAG, "Failed to save photo");
            }
        }
        
        // Scale/rotate frame to preview buffer using PPA
        if (ppa_handle && viewer->preview_buffer_ && frame_data) {
            ppa_srm_oper_config_t srm_config = {};
            srm_config.in.buffer = frame_data;
            srm_config.in.pic_w = frame_width;
            srm_config.in.pic_h = frame_height;
            srm_config.in.block_w = frame_width;
            srm_config.in.block_h = frame_height;
            srm_config.in.block_offset_x = 0;
            srm_config.in.block_offset_y = 0;
            srm_config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            
            srm_config.out.buffer = viewer->preview_buffer_;
            srm_config.out.buffer_size = viewer->preview_buffer_size_;
            srm_config.out.pic_w = viewer->preview_width_;
            srm_config.out.pic_h = viewer->preview_height_;
            srm_config.out.block_offset_x = 0;
            srm_config.out.block_offset_y = 0;
            srm_config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
            
            // Calculate scaling factors
            float scale_x = (float)viewer->preview_width_ / frame_width;
            float scale_y = (float)viewer->preview_height_ / frame_height;
            float scale = (scale_x < scale_y) ? scale_x : scale_y;  // Fit to preview
            
            srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
            srm_config.scale_x = scale;
            srm_config.scale_y = scale;
            srm_config.mirror_x = true;  // Mirror for selfie view
            srm_config.mirror_y = false;
            srm_config.rgb_swap = false;
            srm_config.byte_swap = false;
            srm_config.mode = PPA_TRANS_MODE_BLOCKING;
            
            ppa_do_scale_rotate_mirror(ppa_handle, &srm_config);
            
            // Update LVGL canvas
            lv_obj_invalidate(viewer->preview_canvas_);
        }
        
        // Release the frame back to camera
        camera->ReleasePreviewFrame();
        
        vTaskDelay(pdMS_TO_TICKS(CAMERA_FRAME_DELAY_MS)); // ~20fps to reduce PSRAM load
    }
    
    // Cleanup
    ESP_LOGI(TAG, "Camera task exiting");
    
    if (ppa_handle) {
        ppa_unregister_client(ppa_handle);
    }
    
    viewer->task_running_.store(false);
    vTaskDelete(nullptr);
}
#endif // CONFIG_IDF_TARGET_ESP32P4

bool CameraViewer::TakePhoto() {
#if CONFIG_IDF_TARGET_ESP32P4
    if (!task_running_.load()) {
        ESP_LOGW(TAG, "Camera task not running");
        return false;
    }
    
    CameraCommand cmd = CameraCommand::CAPTURE;
    xQueueSend(command_queue_, &cmd, portMAX_DELAY);
    return true;
#else
    ESP_LOGW(TAG, "Camera not supported on this platform");
    return false;
#endif
}

std::string CameraViewer::GeneratePhotoFilename() {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    
    char filename[64];
    snprintf(filename, sizeof(filename), "IMG_%04d%02d%02d_%02d%02d%02d.jpg",
             timeinfo->tm_year + 1900,
             timeinfo->tm_mon + 1,
             timeinfo->tm_mday,
             timeinfo->tm_hour,
             timeinfo->tm_min,
             timeinfo->tm_sec);
    
    return std::string(filename);
}

void CameraViewer::ShowStatus(const char* message, uint32_t duration_ms) {
    if (!status_label_) return;
    
    lv_label_set_text(status_label_, message);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    
    // Create a timer to hide the status message
    lv_timer_t* timer = lv_timer_create([](lv_timer_t* t) {
        lv_obj_t* label = (lv_obj_t*)lv_timer_get_user_data(t);
        if (label) {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_timer_delete(t);
    }, duration_ms, status_label_);
    lv_timer_set_repeat_count(timer, 1);
}

void CameraViewer::OnCaptureButtonClicked(lv_event_t* e) {
    CameraViewer* viewer = static_cast<CameraViewer*>(lv_event_get_user_data(e));
    if (viewer) {
        ESP_LOGI(TAG, "Capture button clicked");
        viewer->TakePhoto();
    }
}

void CameraViewer::OnCloseButtonClicked(lv_event_t* e) {
    CameraViewer* viewer = static_cast<CameraViewer*>(lv_event_get_user_data(e));
    if (viewer) {
        ESP_LOGI(TAG, "Close button clicked");
        viewer->Hide();
    }
}

void CameraViewer::OnGalleryButtonClicked(lv_event_t* e) {
    CameraViewer* viewer = static_cast<CameraViewer*>(lv_event_get_user_data(e));
    if (viewer) {
        ESP_LOGI(TAG, "Gallery button clicked - navigating to Camera folder");
        // TODO: Integrate with file browser to show Camera folder
        // For now, just show a status message
        viewer->ShowStatus("Open SD Card > Camera", 2000);
    }
}

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
#include "esp_video.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"
#include "jpg/image_to_jpeg.h"
#include "driver/ppa.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static const char* TAG = "CameraViewer";

// Camera preview resolution (can be adjusted for performance)
#define CAMERA_PREVIEW_WIDTH  1280
#define CAMERA_PREVIEW_HEIGHT 720

// SD card camera folder
#define CAMERA_FOLDER "/sdcard/Camera"

// Task configuration
#define CAMERA_TASK_STACK_SIZE (8 * 1024)
#define CAMERA_TASK_PRIORITY   5
#define COMMAND_QUEUE_SIZE     10

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
    
    // Open video device
    int video_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (video_fd < 0) {
        ESP_LOGE(TAG, "Failed to open video device");
        viewer->task_running_.store(false);
        vTaskDelete(nullptr);
        return;
    }
    
    // Get current format
    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        close(video_fd);
        viewer->task_running_.store(false);
        vTaskDelete(nullptr);
        return;
    }
    
    // Set to RGB565 format for preview
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(video_fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed");
        close(video_fd);
        viewer->task_running_.store(false);
        vTaskDelete(nullptr);
        return;
    }
    
    uint32_t cam_width = format.fmt.pix.width;
    uint32_t cam_height = format.fmt.pix.height;
    ESP_LOGI(TAG, "Camera resolution: %lux%lu", cam_width, cam_height);
    
    // Request buffers
    struct v4l2_requestbuffers req = {};
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        close(video_fd);
        viewer->task_running_.store(false);
        vTaskDelete(nullptr);
        return;
    }
    
    // Map buffers
    struct MmapBuffer { void* start; size_t length; };
    MmapBuffer mmap_buffers[2] = {};
    
    for (int i = 0; i < 2; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        
        if (ioctl(video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed for buffer %d", i);
            close(video_fd);
            viewer->task_running_.store(false);
            vTaskDelete(nullptr);
            return;
        }
        
        mmap_buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, 
                                      MAP_SHARED, video_fd, buf.m.offset);
        mmap_buffers[i].length = buf.length;
        
        if (mmap_buffers[i].start == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap failed for buffer %d", i);
            close(video_fd);
            viewer->task_running_.store(false);
            vTaskDelete(nullptr);
            return;
        }
        
        // Queue buffer
        if (ioctl(video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed for buffer %d", i);
        }
    }
    
    // Start streaming
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        for (int i = 0; i < 2; i++) {
            if (mmap_buffers[i].start) {
                munmap(mmap_buffers[i].start, mmap_buffers[i].length);
            }
        }
        close(video_fd);
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
        
        // Dequeue buffer
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (ioctl(video_fd, VIDIOC_DQBUF, &buf) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Check if we need to capture a photo
        if (viewer->is_capturing_.load()) {
            viewer->is_capturing_.store(false);
            
            // Convert frame to JPEG and save
            uint8_t* jpeg_data = nullptr;
            size_t jpeg_size = 0;
            
            // Use image_to_jpeg to encode
            // RGB565 format: 2 bytes per pixel
            size_t src_len = cam_width * cam_height * 2;
            bool success = image_to_jpeg((uint8_t*)mmap_buffers[buf.index].start,
                                          src_len,
                                          cam_width, cam_height,
                                          V4L2_PIX_FMT_RGB565,
                                          80,  // quality
                                          &jpeg_data, &jpeg_size);
            
            if (success && jpeg_data && jpeg_size > 0) {
                if (viewer->SaveJpegToSdCard(jpeg_data, jpeg_size)) {
                    viewer->ShowStatus("Photo saved!", 2000);
                } else {
                    viewer->ShowStatus("Failed to save photo", 2000);
                }
                heap_caps_free(jpeg_data);
            } else {
                ESP_LOGE(TAG, "Failed to encode JPEG");
                viewer->ShowStatus("Failed to encode photo", 2000);
            }
        }
        
        // Scale/rotate frame to preview buffer using PPA
        if (ppa_handle && viewer->preview_buffer_) {
            ppa_srm_oper_config_t srm_config = {};
            srm_config.in.buffer = mmap_buffers[buf.index].start;
            srm_config.in.pic_w = cam_width;
            srm_config.in.pic_h = cam_height;
            srm_config.in.block_w = cam_width;
            srm_config.in.block_h = cam_height;
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
            
            srm_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
            srm_config.scale_x = 1.0f;
            srm_config.scale_y = 1.0f;
            srm_config.mirror_x = true;  // Mirror for selfie view
            srm_config.mirror_y = false;
            srm_config.rgb_swap = false;
            srm_config.byte_swap = false;
            srm_config.mode = PPA_TRANS_MODE_BLOCKING;
            
            ppa_do_scale_rotate_mirror(ppa_handle, &srm_config);
            
            // Update LVGL canvas
            lv_obj_invalidate(viewer->preview_canvas_);
        }
        
        // Re-queue buffer
        if (ioctl(video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed");
        }
        
        vTaskDelay(pdMS_TO_TICKS(16)); // ~60fps target
    }
    
    // Cleanup
    ESP_LOGI(TAG, "Camera task exiting");
    
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(video_fd, VIDIOC_STREAMOFF, &type);
    
    if (ppa_handle) {
        ppa_unregister_client(ppa_handle);
    }
    
    for (int i = 0; i < 2; i++) {
        if (mmap_buffers[i].start && mmap_buffers[i].start != MAP_FAILED) {
            munmap(mmap_buffers[i].start, mmap_buffers[i].length);
        }
    }
    
    close(video_fd);
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

bool CameraViewer::SaveJpegToSdCard(uint8_t* jpeg_data, size_t jpeg_size) {
    if (!jpeg_data || jpeg_size == 0) {
        return false;
    }
    
    std::string filename = GeneratePhotoFilename();
    std::string filepath = std::string(CAMERA_FOLDER) + "/" + filename;
    
    FILE* f = fopen(filepath.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath.c_str());
        return false;
    }
    
    size_t written = fwrite(jpeg_data, 1, jpeg_size, f);
    fclose(f);
    
    if (written != jpeg_size) {
        ESP_LOGE(TAG, "Failed to write all data: wrote %zu of %zu bytes", written, jpeg_size);
        return false;
    }
    
    ESP_LOGI(TAG, "Photo saved: %s (%zu bytes)", filepath.c_str(), jpeg_size);
    return true;
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

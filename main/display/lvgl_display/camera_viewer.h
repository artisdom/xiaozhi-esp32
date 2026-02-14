#ifndef CAMERA_VIEWER_H
#define CAMERA_VIEWER_H

#include <lvgl.h>
#include <string>
#include <functional>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class Camera;

/**
 * @brief Camera viewer with live preview and photo capture functionality
 * 
 * Provides a fullscreen camera preview with ability to take photos
 * and save them to the SD card in the /sdcard/Camera folder.
 */
class CameraViewer {
public:
    using CloseCallback = std::function<void()>;

    CameraViewer();
    ~CameraViewer();

    /**
     * @brief Initialize the camera viewer UI components
     * @param parent The parent LVGL object (defaults to active screen)
     */
    void Init(lv_obj_t* parent = nullptr);

    /**
     * @brief Show the camera viewer and start preview
     */
    void Show();

    /**
     * @brief Hide the camera viewer and stop preview
     */
    void Hide();

    /**
     * @brief Check if camera viewer is currently visible
     */
    bool IsVisible() const { return is_visible_; }

    /**
     * @brief Set callback for when camera viewer is closed
     */
    void SetCloseCallback(CloseCallback callback) {
        close_callback_ = callback;
    }

    /**
     * @brief Take a photo and save to SD card
     * @return true if photo was saved successfully
     */
    bool TakePhoto();

private:
    // Control commands for camera task
    enum class CameraCommand {
        NONE,
        START,
        STOP,
        CAPTURE,
        EXIT
    };

    // UI components
    lv_obj_t* container_ = nullptr;      // Main fullscreen container
    lv_obj_t* preview_canvas_ = nullptr; // Camera preview canvas
    lv_obj_t* capture_btn_ = nullptr;    // Capture photo button
    lv_obj_t* close_btn_ = nullptr;      // Close button
    lv_obj_t* status_label_ = nullptr;   // Status message label
    lv_obj_t* gallery_btn_ = nullptr;    // Gallery button (navigate to Camera folder)

    // State
    std::atomic<bool> is_visible_{false};
    std::atomic<bool> is_capturing_{false};
    std::atomic<bool> task_running_{false};
    CloseCallback close_callback_;
    
    // Camera task
    TaskHandle_t camera_task_handle_ = nullptr;
    QueueHandle_t command_queue_ = nullptr;
    
    // Preview buffer
    uint8_t* preview_buffer_ = nullptr;
    size_t preview_buffer_size_ = 0;
    uint16_t preview_width_ = 0;
    uint16_t preview_height_ = 0;

    // Internal methods
    void CreateUI(lv_obj_t* parent);
    void StartPreview();
    void StopPreview();
    std::string GeneratePhotoFilename();
    void ShowStatus(const char* message, uint32_t duration_ms = 2000);
    
    // Camera task function
    static void CameraTaskFunc(void* arg);
    
    // Event handlers
    static void OnCaptureButtonClicked(lv_event_t* e);
    static void OnCloseButtonClicked(lv_event_t* e);
    static void OnGalleryButtonClicked(lv_event_t* e);
};

#endif // CAMERA_VIEWER_H

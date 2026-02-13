#ifndef FILE_VIEWER_H
#define FILE_VIEWER_H

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>

/**
 * @brief Base class for file viewers
 */
class FileViewer {
public:
    using CloseCallback = std::function<void()>;

    FileViewer();
    virtual ~FileViewer();

    /**
     * @brief Initialize the viewer UI components
     * @param parent The parent LVGL object (defaults to active screen)
     */
    virtual void Init(lv_obj_t* parent = nullptr);

    /**
     * @brief Open and display a file
     * @param path Path to the file
     * @return true if successful
     */
    virtual bool Open(const std::string& path) = 0;

    /**
     * @brief Close the viewer
     */
    virtual void Close();

    /**
     * @brief Check if viewer is visible
     */
    bool IsVisible() const { return is_visible_; }

    /**
     * @brief Set close callback
     */
    void SetCloseCallback(CloseCallback callback) {
        close_callback_ = callback;
    }

protected:
    lv_obj_t* container_ = nullptr;
    lv_obj_t* title_bar_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* close_btn_ = nullptr;
    lv_obj_t* content_area_ = nullptr;
    
    bool is_visible_ = false;
    CloseCallback close_callback_;
    std::string current_file_;

    void CreateBaseUI(lv_obj_t* parent);
    static void OnCloseButtonClicked(lv_event_t* e);
};

/**
 * @brief Text file viewer with scrolling support
 */
class TextFileViewer : public FileViewer {
public:
    TextFileViewer();
    virtual ~TextFileViewer();

    void Init(lv_obj_t* parent = nullptr) override;
    bool Open(const std::string& path) override;
    void Close() override;

private:
    lv_obj_t* text_label_ = nullptr;
    char* file_content_ = nullptr;
    
    void FreeContent();
};

/**
 * @brief Image viewer with swipe navigation support
 */
class ImageViewer : public FileViewer {
public:
    using NavigateCallback = std::function<void(const std::string& path)>;

    ImageViewer();
    virtual ~ImageViewer();

    void Init(lv_obj_t* parent = nullptr) override;
    bool Open(const std::string& path) override;
    void Close() override;

    /**
     * @brief Set list of images for navigation
     * @param images List of image paths in the same directory
     */
    void SetImageList(const std::vector<std::string>& images);

    /**
     * @brief Set callback for when navigating to a new image
     */
    void SetNavigateCallback(NavigateCallback callback) {
        navigate_callback_ = callback;
    }

private:
    lv_obj_t* image_obj_ = nullptr;
    lv_obj_t* error_label_ = nullptr;
    lv_obj_t* prev_btn_ = nullptr;
    lv_obj_t* next_btn_ = nullptr;
    lv_obj_t* nav_label_ = nullptr;  // Shows "3/10" style counter
    uint8_t* image_data_ = nullptr;
    lv_image_dsc_t image_dsc_ = {};
    
    // Navigation state
    std::vector<std::string> image_list_;
    int current_index_ = -1;
    NavigateCallback navigate_callback_;
    
    void FreeImageData();
    void ShowError(const char* message);
    void NavigatePrev();
    void NavigateNext();
    void UpdateNavUI();
    
    static void OnSwipeEvent(lv_event_t* e);
    static void OnPrevButtonClicked(lv_event_t* e);
    static void OnNextButtonClicked(lv_event_t* e);
};

/**
 * @brief Audio player with playback controls
 */
class AudioPlayer : public FileViewer {
public:
    AudioPlayer();
    virtual ~AudioPlayer();

    void Init(lv_obj_t* parent = nullptr) override;
    bool Open(const std::string& path) override;
    void Close() override;

private:
    lv_obj_t* play_btn_ = nullptr;
    lv_obj_t* stop_btn_ = nullptr;
    lv_obj_t* progress_bar_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* file_info_label_ = nullptr;
    
    bool is_playing_ = false;
    
    static void OnPlayButtonClicked(lv_event_t* e);
    static void OnStopButtonClicked(lv_event_t* e);
};

/**
 * @brief Video player (placeholder - limited on ESP32)
 */
class VideoPlayer : public FileViewer {
public:
    VideoPlayer();
    virtual ~VideoPlayer();

    void Init(lv_obj_t* parent = nullptr) override;
    bool Open(const std::string& path) override;
    void Close() override;

private:
    lv_obj_t* info_label_ = nullptr;
};

#endif // FILE_VIEWER_H

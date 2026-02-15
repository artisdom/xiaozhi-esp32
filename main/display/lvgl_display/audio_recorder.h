#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

/**
 * @brief Recording entry structure for grid display
 */
struct RecordingEntry {
    std::string name;
    std::string path;
    size_t size;
    time_t timestamp;
};

/**
 * @brief Audio recorder with recording functionality and file browser
 * 
 * Provides a fullscreen UI for recording audio and browsing/playing
 * previously recorded audio files stored in /sdcard/Recording folder.
 */
class AudioRecorder {
public:
    using CloseCallback = std::function<void()>;
    using PlayCallback = std::function<void(const std::string& path)>;

    AudioRecorder();
    ~AudioRecorder();

    /**
     * @brief Initialize the audio recorder UI components
     * @param parent The parent LVGL object (defaults to active screen)
     */
    void Init(lv_obj_t* parent = nullptr);

    /**
     * @brief Show the audio recorder and refresh recording list
     */
    void Show();

    /**
     * @brief Hide the audio recorder and stop any recording
     */
    void Hide();

    /**
     * @brief Check if audio recorder is currently visible
     */
    bool IsVisible() const { return is_visible_; }

    /**
     * @brief Set callback for when audio recorder is closed
     */
    void SetCloseCallback(CloseCallback callback) {
        close_callback_ = callback;
    }

    /**
     * @brief Set callback for when a recording is selected for playback
     */
    void SetPlayCallback(PlayCallback callback) {
        play_callback_ = callback;
    }

    /**
     * @brief Start audio recording
     * @return true if recording started successfully
     */
    bool StartRecording();

    /**
     * @brief Stop audio recording and save file
     * @return true if recording was saved successfully
     */
    bool StopRecording();

    /**
     * @brief Check if currently recording
     */
    bool IsRecording() const { return is_recording_.load(); }

private:
    // Control commands for recording task
    enum class RecordCommand {
        NONE,
        START,
        STOP,
        EXIT
    };

    // UI components
    lv_obj_t* container_ = nullptr;          // Main fullscreen container
    lv_obj_t* title_bar_ = nullptr;          // Title bar
    lv_obj_t* title_label_ = nullptr;        // Title text
    lv_obj_t* close_btn_ = nullptr;          // Close button
    lv_obj_t* record_btn_ = nullptr;         // Record/Stop button
    lv_obj_t* record_btn_label_ = nullptr;   // Record button icon
    lv_obj_t* status_label_ = nullptr;       // Status message label
    lv_obj_t* timer_label_ = nullptr;        // Recording timer display
    lv_obj_t* recordings_grid_ = nullptr;    // Grid container for recordings
    lv_obj_t* no_recordings_label_ = nullptr; // "No recordings" placeholder

    // State
    std::atomic<bool> is_visible_{false};
    std::atomic<bool> is_recording_{false};
    std::atomic<bool> task_running_{false};
    CloseCallback close_callback_;
    PlayCallback play_callback_;
    std::vector<RecordingEntry> recordings_;
    std::string current_recording_path_;
    uint32_t recording_start_time_ = 0;
    lv_timer_t* timer_update_timer_ = nullptr;
    
    // Recording task
    TaskHandle_t record_task_handle_ = nullptr;
    QueueHandle_t command_queue_ = nullptr;
    
    // Recording buffer
    FILE* recording_file_ = nullptr;
    size_t samples_recorded_ = 0;
    int recording_sample_rate_ = 0;  // Actual sample rate from codec
    int recording_channels_ = 2;     // Recording output channels (stereo for best quality)
    int original_input_channels_ = 0; // Original codec input channels to restore

    // Internal methods
    void CreateUI(lv_obj_t* parent);
    void RefreshRecordingsList();
    void PopulateRecordingsGrid();
    std::vector<RecordingEntry> ScanRecordings();
    std::string GenerateRecordingFilename();
    void ShowStatus(const char* message, uint32_t duration_ms = 2000);
    void UpdateRecordingTimer();
    void PlayRecording(const std::string& path);
    bool WriteWavHeader(FILE* file, int sample_rate, int channels, int bits_per_sample);
    bool UpdateWavHeader(FILE* file, size_t data_size);
    
    // Recording task function
    static void RecordingTaskFunc(void* arg);
    
    // Event handlers
    static void OnRecordButtonClicked(lv_event_t* e);
    static void OnCloseButtonClicked(lv_event_t* e);
    static void OnRecordingItemClicked(lv_event_t* e);
    static void OnTimerUpdate(lv_timer_t* timer);
};

#endif // AUDIO_RECORDER_H

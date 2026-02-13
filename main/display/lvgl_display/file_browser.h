#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <lvgl.h>
#include <string>
#include <vector>
#include <functional>
#include <dirent.h>
#include <sys/stat.h>

/**
 * @brief File entry structure with type information
 */
struct FileEntry {
    std::string name;
    std::string path;
    bool is_dir;
    size_t size;
};

/**
 * @brief File type enumeration for viewer selection
 */
enum class FileType {
    UNKNOWN,
    TEXT,
    CSV,
    IMAGE,
    AUDIO,
    VIDEO,
    DIRECTORY
};

/**
 * @brief LVGL-based file browser overlay component
 * 
 * Provides a touch-enabled file browser UI that can be shown
 * over the main application UI. Supports navigation through
 * directories and file selection.
 */
class FileBrowser {
public:
    using FileSelectedCallback = std::function<void(const std::string& path, FileType type)>;
    using CloseCallback = std::function<void()>;

    FileBrowser();
    ~FileBrowser();

    /**
     * @brief Initialize the file browser UI components
     * @param parent The parent LVGL object (usually screen)
     */
    void Init(lv_obj_t* parent = nullptr);

    /**
     * @brief Show the file browser starting at specified path
     * @param root_path The starting directory path
     */
    void Show(const std::string& root_path = "/sdcard");

    /**
     * @brief Hide the file browser overlay
     */
    void Hide();

    /**
     * @brief Check if file browser is currently visible
     */
    bool IsVisible() const { return is_visible_; }

    /**
     * @brief Set callback for when a file is selected
     */
    void SetFileSelectedCallback(FileSelectedCallback callback) {
        file_selected_callback_ = callback;
    }

    /**
     * @brief Set callback for when file browser is closed
     */
    void SetCloseCallback(CloseCallback callback) {
        close_callback_ = callback;
    }

    /**
     * @brief Navigate to a directory
     */
    void NavigateTo(const std::string& path);

    /**
     * @brief Navigate up to parent directory
     */
    void NavigateUp();

    /**
     * @brief Get file type from filename extension
     */
    static FileType GetFileType(const std::string& filename);

    /**
     * @brief Get current directory path
     */
    const std::string& GetCurrentPath() const { return current_path_; }

private:
    // UI components
    lv_obj_t* container_ = nullptr;      // Main container (overlay)
    lv_obj_t* title_bar_ = nullptr;      // Title bar with path and close button
    lv_obj_t* path_label_ = nullptr;     // Current path display
    lv_obj_t* back_btn_ = nullptr;       // Back/up button
    lv_obj_t* close_btn_ = nullptr;      // Close button
    lv_obj_t* file_list_ = nullptr;      // File listing area

    // State
    std::string root_path_;
    std::string current_path_;
    std::vector<FileEntry> entries_;
    bool is_visible_ = false;

    // Callbacks
    FileSelectedCallback file_selected_callback_;
    CloseCallback close_callback_;

    // Internal methods
    void CreateUI(lv_obj_t* parent);
    void RefreshFileList();
    void PopulateFileList();
    std::vector<FileEntry> ScanDirectory(const std::string& path);
    
    // Event handlers
    static void OnFileItemClicked(lv_event_t* e);
    static void OnBackButtonClicked(lv_event_t* e);
    static void OnCloseButtonClicked(lv_event_t* e);

    // Icon helpers
    static const char* GetFileIcon(FileType type);
    static lv_color_t GetFileIconColor(FileType type);
};

#endif // FILE_BROWSER_H

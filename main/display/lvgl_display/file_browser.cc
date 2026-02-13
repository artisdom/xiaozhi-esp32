#include "file_browser.h"
#include <font_awesome.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>

static const char* TAG = "FileBrowser";

FileBrowser::FileBrowser() {}

FileBrowser::~FileBrowser() {
    if (container_) {
        lv_obj_delete(container_);
        container_ = nullptr;
    }
}

void FileBrowser::Init(lv_obj_t* parent) {
    if (container_) {
        return; // Already initialized
    }
    if (parent == nullptr) {
        parent = lv_screen_active();
    }
    CreateUI(parent);
}

void FileBrowser::CreateUI(lv_obj_t* parent) {
    // Create semi-transparent background overlay
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_90, 0);
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

    // Back button
    back_btn_ = lv_btn_create(title_bar_);
    lv_obj_set_size(back_btn_, 50, 44);
    lv_obj_align(back_btn_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn_, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_radius(back_btn_, 8, 0);
    lv_obj_add_event_cb(back_btn_, OnBackButtonClicked, LV_EVENT_CLICKED, this);
    
    lv_obj_t* back_label = lv_label_create(back_btn_);
    lv_label_set_text(back_label, FONT_AWESOME_ARROW_LEFT);
    lv_obj_center(back_label);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);

    // Path label
    path_label_ = lv_label_create(title_bar_);
    lv_obj_set_width(path_label_, LV_HOR_RES - 140);
    lv_label_set_long_mode(path_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(path_label_, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_color(path_label_, lv_color_white(), 0);
    lv_label_set_text(path_label_, "/sdcard");

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

    // File list area
    file_list_ = lv_obj_create(container_);
    lv_obj_set_size(file_list_, LV_HOR_RES, LV_VER_RES - 60);
    lv_obj_align(file_list_, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(file_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(file_list_, 0, 0);
    lv_obj_set_style_pad_all(file_list_, 8, 0);
    lv_obj_set_flex_flow(file_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(file_list_, 4, 0);
    lv_obj_set_scrollbar_mode(file_list_, LV_SCROLLBAR_MODE_AUTO);

    ESP_LOGI(TAG, "File browser UI created");
}

void FileBrowser::Show(const std::string& root_path) {
    root_path_ = root_path;
    current_path_ = root_path;
    
    if (container_) {
        lv_obj_clear_flag(container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(container_);
        RefreshFileList();
        is_visible_ = true;
        ESP_LOGI(TAG, "File browser shown at: %s", root_path.c_str());
    }
}

void FileBrowser::Hide() {
    if (container_) {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = false;
        ESP_LOGI(TAG, "File browser hidden");
    }
    
    if (close_callback_) {
        close_callback_();
    }
}

void FileBrowser::NavigateTo(const std::string& path) {
    current_path_ = path;
    RefreshFileList();
    ESP_LOGI(TAG, "Navigated to: %s", path.c_str());
}

void FileBrowser::NavigateUp() {
    if (current_path_ == root_path_ || current_path_ == "/") {
        return; // Already at root
    }
    
    // Find last separator
    size_t pos = current_path_.rfind('/');
    if (pos == 0) {
        current_path_ = "/";
    } else if (pos != std::string::npos) {
        current_path_ = current_path_.substr(0, pos);
    }
    
    RefreshFileList();
    ESP_LOGI(TAG, "Navigated up to: %s", current_path_.c_str());
}

void FileBrowser::RefreshFileList() {
    if (!file_list_) return;
    
    // Update path label
    lv_label_set_text(path_label_, current_path_.c_str());
    
    // Scan directory
    entries_ = ScanDirectory(current_path_);
    
    // Update UI
    PopulateFileList();
}

std::vector<FileEntry> FileBrowser::ScanDirectory(const std::string& path) {
    std::vector<FileEntry> entries;
    
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path.c_str());
        return entries;
    }
    
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        FileEntry entry;
        entry.name = ent->d_name;
        entry.path = path + "/" + entry.name;
        
        struct stat st;
        if (stat(entry.path.c_str(), &st) == 0) {
            entry.is_dir = S_ISDIR(st.st_mode);
            entry.size = st.st_size;
        } else {
            entry.is_dir = (ent->d_type == DT_DIR);
            entry.size = 0;
        }
        
        entries.push_back(entry);
    }
    
    closedir(dir);
    
    // Sort: directories first, then files alphabetically
    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.is_dir != b.is_dir) {
            return a.is_dir > b.is_dir; // Directories first
        }
        return a.name < b.name; // Alphabetical
    });
    
    ESP_LOGI(TAG, "Found %d entries in %s", (int)entries.size(), path.c_str());
    return entries;
}

void FileBrowser::PopulateFileList() {
    if (!file_list_) return;
    
    // Clear existing items
    lv_obj_clean(file_list_);
    
    // Create list items for each entry
    for (size_t i = 0; i < entries_.size(); i++) {
        const auto& entry = entries_[i];
        FileType type = entry.is_dir ? FileType::DIRECTORY : GetFileType(entry.name);
        
        // Create item container
        lv_obj_t* item = lv_obj_create(file_list_);
        lv_obj_set_size(item, LV_HOR_RES - 24, 56);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x16213e), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 8, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        
        // Pressed state style
        lv_obj_set_style_bg_color(item, lv_color_hex(0x0f3460), LV_STATE_PRESSED);
        
        // Store entry index in user data
        lv_obj_set_user_data(item, (void*)(uintptr_t)i);
        lv_obj_add_event_cb(item, OnFileItemClicked, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        
        // Icon
        lv_obj_t* icon = lv_label_create(item);
        lv_label_set_text(icon, GetFileIcon(type));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_color(icon, GetFileIconColor(type), 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        
        // Filename
        lv_obj_t* name_label = lv_label_create(item);
        lv_obj_set_width(name_label, LV_HOR_RES - 120);
        lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(name_label, entry.name.c_str());
        lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 40, 0);
        lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
        lv_obj_remove_flag(name_label, LV_OBJ_FLAG_CLICKABLE);
        
        // Size (for files only)
        if (!entry.is_dir) {
            lv_obj_t* size_label = lv_label_create(item);
            char size_str[32];
            if (entry.size < 1024) {
                snprintf(size_str, sizeof(size_str), "%u B", (unsigned)entry.size);
            } else if (entry.size < 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f KB", entry.size / 1024.0);
            } else {
                snprintf(size_str, sizeof(size_str), "%.1f MB", entry.size / (1024.0 * 1024.0));
            }
            lv_label_set_text(size_label, size_str);
            lv_obj_align(size_label, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_text_color(size_label, lv_color_hex(0x888888), 0);
            lv_obj_remove_flag(size_label, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    
    // Empty directory message
    if (entries_.empty()) {
        lv_obj_t* empty_label = lv_label_create(file_list_);
        lv_label_set_text(empty_label, "Empty folder");
        lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x888888), 0);
    }
}

FileType FileBrowser::GetFileType(const std::string& filename) {
    size_t dot_pos = filename.rfind('.');
    if (dot_pos == std::string::npos) {
        return FileType::UNKNOWN;
    }
    
    std::string ext = filename.substr(dot_pos + 1);
    // Convert to lowercase
    for (auto& c : ext) {
        c = tolower(c);
    }
    
    // Text files
    if (ext == "txt" || ext == "log" || ext == "md" || ext == "json" || 
        ext == "xml" || ext == "ini" || ext == "cfg" || ext == "conf") {
        return FileType::TEXT;
    }
    
    // CSV files
    if (ext == "csv" || ext == "tsv") {
        return FileType::CSV;
    }
    
    // Image files
    if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "bmp" || 
        ext == "gif" || ext == "webp") {
        return FileType::IMAGE;
    }
    
    // Audio files
    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "aac" ||
        ext == "ogg" || ext == "m4a" || ext == "wma") {
        return FileType::AUDIO;
    }
    
    // Video files
    if (ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" ||
        ext == "wmv" || ext == "flv" || ext == "webm") {
        return FileType::VIDEO;
    }
    
    return FileType::UNKNOWN;
}

const char* FileBrowser::GetFileIcon(FileType type) {
    switch (type) {
        case FileType::DIRECTORY: return FONT_AWESOME_SD_CARD;  // Using SD card icon for folders
        case FileType::TEXT:      return FONT_AWESOME_PEN_TO_SQUARE;  // Edit/document icon
        case FileType::CSV:       return FONT_AWESOME_CALCULATOR;  // Calculator for data
        case FileType::IMAGE:     return FONT_AWESOME_IMAGE;
        case FileType::AUDIO:     return FONT_AWESOME_MUSIC;
        case FileType::VIDEO:     return FONT_AWESOME_PLAY;  // Play icon for video
        default:                  return FONT_AWESOME_CIRCLE_QUESTION;  // Question mark for unknown
    }
}

lv_color_t FileBrowser::GetFileIconColor(FileType type) {
    switch (type) {
        case FileType::DIRECTORY: return lv_color_hex(0xffd700); // Gold
        case FileType::TEXT:      return lv_color_hex(0x87ceeb); // Sky blue
        case FileType::CSV:       return lv_color_hex(0x90ee90); // Light green
        case FileType::IMAGE:     return lv_color_hex(0xff69b4); // Hot pink
        case FileType::AUDIO:     return lv_color_hex(0xffa500); // Orange
        case FileType::VIDEO:     return lv_color_hex(0xff6347); // Tomato
        default:                  return lv_color_hex(0xaaaaaa); // Gray
    }
}

// Event handlers
void FileBrowser::OnFileItemClicked(lv_event_t* e) {
    FileBrowser* browser = static_cast<FileBrowser*>(lv_event_get_user_data(e));
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_target(e));
    
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(item);
    if (index >= browser->entries_.size()) {
        return;
    }
    
    const FileEntry& entry = browser->entries_[index];
    
    if (entry.is_dir) {
        // Navigate into directory
        browser->NavigateTo(entry.path);
    } else {
        // File selected - invoke callback
        if (browser->file_selected_callback_) {
            FileType type = GetFileType(entry.name);
            browser->file_selected_callback_(entry.path, type);
            ESP_LOGI(TAG, "File selected: %s (type: %d)", entry.path.c_str(), (int)type);
        }
    }
}

void FileBrowser::OnBackButtonClicked(lv_event_t* e) {
    FileBrowser* browser = static_cast<FileBrowser*>(lv_event_get_user_data(e));
    browser->NavigateUp();
}

void FileBrowser::OnCloseButtonClicked(lv_event_t* e) {
    FileBrowser* browser = static_cast<FileBrowser*>(lv_event_get_user_data(e));
    browser->Hide();
}

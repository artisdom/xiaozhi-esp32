#ifndef WIFI_MANAGER_SCREEN_H
#define WIFI_MANAGER_SCREEN_H

#include <lvgl.h>
#include <wifi_manager.h>
#include <ssid_manager.h>

#include <functional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class WifiManagerScreen {
public:
    using CloseCallback = std::function<void()>;

    WifiManagerScreen();
    ~WifiManagerScreen();

    void Init(lv_obj_t* parent = nullptr);
    void Show();
    void Hide();

    bool IsVisible() const { return is_visible_; }

    void SetCloseCallback(CloseCallback callback) {
        close_callback_ = std::move(callback);
    }

private:
    struct ScanTaskResult {
        WifiManagerScreen* screen = nullptr;
        bool success = false;
        std::vector<WifiScanResult> results;
    };

    // Main UI
    lv_obj_t* container_ = nullptr;
    lv_obj_t* title_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* current_ssid_label_ = nullptr;
    lv_obj_t* current_state_label_ = nullptr;
    lv_obj_t* current_ip_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* scan_btn_ = nullptr;
    lv_obj_t* scan_btn_label_ = nullptr;
    lv_obj_t* saved_list_ = nullptr;
    lv_obj_t* scan_list_ = nullptr;

    // Password dialog
    lv_obj_t* password_modal_ = nullptr;
    lv_obj_t* password_ssid_label_ = nullptr;
    lv_obj_t* password_textarea_ = nullptr;
    lv_obj_t* password_keypad_ = nullptr;

    // State
    lv_timer_t* status_timer_ = nullptr;
    std::vector<SsidItem> saved_hotspots_;
    std::vector<WifiScanResult> scan_results_;
    std::string selected_ssid_;
    std::string pending_connect_ssid_;
    std::string pending_connect_password_;
    int64_t pending_connect_started_us_ = 0;
    bool is_visible_ = false;
    bool scan_in_progress_ = false;
    bool connect_in_progress_ = false;
    CloseCallback close_callback_;

    // Internal behavior
    void CreateUI(lv_obj_t* parent);
    void CreatePasswordDialog();
    bool EnsureWifiReady();
    void RefreshCurrentNetwork();
    void LoadSavedHotspots();
    void PopulateSavedHotspots();
    void PopulateScanResults();
    bool FindSavedPassword(const std::string& ssid, std::string& password) const;
    void UpdateScanButton(bool scanning);
    void SetHint(const std::string& text);

    void StartScan();
    void HandleScanFinished(bool success, std::vector<WifiScanResult>&& results);
    void RequestConnection(const std::string& ssid, const std::string& password);
    void HandleConnectionProgress();

    void ShowPasswordDialog(const std::string& ssid, const std::string& preset_password = "");
    void HidePasswordDialog();
    void SubmitPasswordConnect();

    // Async callbacks
    static void ScanTask(void* arg);
    static void OnScanTaskFinished(void* user_data);

    // UI callbacks
    static void OnCloseButtonClicked(lv_event_t* e);
    static void OnScanButtonClicked(lv_event_t* e);
    static void OnSavedItemClicked(lv_event_t* e);
    static void OnScanItemClicked(lv_event_t* e);
    static void OnPasswordConnectClicked(lv_event_t* e);
    static void OnPasswordCancelClicked(lv_event_t* e);
    static void OnPasswordKeypadClicked(lv_event_t* e);
    static void OnStatusTimer(lv_timer_t* timer);
};

#endif // WIFI_MANAGER_SCREEN_H

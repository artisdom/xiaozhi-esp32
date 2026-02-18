#ifndef WIFI_MANAGER_SCREEN_H
#define WIFI_MANAGER_SCREEN_H

#include <lvgl.h>

#include <string>
#include <vector>
#include <functional>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <wifi_manager.h>

struct WifiNetworkEntry {
    std::string ssid;
    int rssi = 0;
    int channel = 0;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

class WifiManagerScreen {
public:
    using CloseCallback = std::function<void()>;

    WifiManagerScreen();
    ~WifiManagerScreen();

    void Init(lv_obj_t* parent = nullptr);
    void Show();
    void Hide();

    bool IsVisible() const { return is_visible_.load(); }

    void SetCloseCallback(CloseCallback callback) {
        close_callback_ = callback;
    }

private:
    lv_obj_t* container_ = nullptr;
    lv_obj_t* title_bar_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* close_btn_ = nullptr;

    lv_obj_t* info_bar_ = nullptr;
    lv_obj_t* current_ssid_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* scan_btn_ = nullptr;
    lv_obj_t* scan_btn_label_ = nullptr;

    lv_obj_t* network_list_ = nullptr;

    lv_obj_t* password_overlay_ = nullptr;
    lv_obj_t* password_panel_ = nullptr;
    lv_obj_t* password_title_label_ = nullptr;
    lv_obj_t* password_textarea_ = nullptr;
    lv_obj_t* keyboard_container_ = nullptr;
    lv_obj_t* connect_btn_ = nullptr;
    lv_obj_t* cancel_btn_ = nullptr;

    std::vector<WifiNetworkEntry> networks_;
    std::string selected_ssid_;
    wifi_auth_mode_t selected_authmode_ = WIFI_AUTH_OPEN;

    std::atomic<bool> is_visible_{false};
    std::atomic<bool> scan_in_progress_{false};
    TaskHandle_t scan_task_handle_ = nullptr;

    lv_timer_t* connect_timer_ = nullptr;
    std::string connect_target_ssid_;
    uint32_t connect_start_tick_ = 0;

    bool keyboard_symbols_ = false;
    bool keyboard_uppercase_ = false;

    CloseCallback close_callback_;

    void CreateUI(lv_obj_t* parent);
    void UpdateCurrentSsid();
    void ShowStatus(const char* text);
    void PopulateNetworkList();

    void StartScan();
    void HandleScanResults(const std::vector<WifiScanResult>& results);

    void ShowPasswordPrompt(const WifiNetworkEntry& network);
    void HidePasswordPrompt();
    void BuildKeyboard();

    void StartConnect(const std::string& ssid, const std::string& password);

    static void OnCloseButtonClicked(lv_event_t* e);
    static void OnScanButtonClicked(lv_event_t* e);
    static void OnNetworkItemClicked(lv_event_t* e);
    static void OnKeyButtonClicked(lv_event_t* e);
    static void OnConnectButtonClicked(lv_event_t* e);
    static void OnCancelButtonClicked(lv_event_t* e);
    static void OnConnectTimer(lv_timer_t* timer);

    static void ScanTask(void* arg);
};

#endif // WIFI_MANAGER_SCREEN_H

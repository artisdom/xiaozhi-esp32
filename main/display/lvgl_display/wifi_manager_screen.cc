#include "wifi_manager_screen.h"

#include <font_awesome.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <widgets/buttonmatrix/lv_buttonmatrix.h>
#include <widgets/textarea/lv_textarea.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "WifiManagerScreen";

static constexpr uint32_t STATUS_TIMER_INTERVAL_MS = 1000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 25000;
static constexpr uint32_t SCAN_TASK_STACK_SIZE = 6144;
static constexpr UBaseType_t SCAN_TASK_PRIORITY = 2;

static bool RequiresPassword(wifi_auth_mode_t authmode) {
    if (authmode == WIFI_AUTH_OPEN) {
        return false;
    }
#ifdef WIFI_AUTH_OWE
    if (authmode == WIFI_AUTH_OWE) {
        return false;
    }
#endif
    return true;
}

WifiManagerScreen::WifiManagerScreen() = default;

WifiManagerScreen::~WifiManagerScreen() {
    if (status_timer_) {
        lv_timer_delete(status_timer_);
        status_timer_ = nullptr;
    }
    if (container_) {
        lv_obj_delete(container_);
        container_ = nullptr;
    }
}

void WifiManagerScreen::Init(lv_obj_t* parent) {
    if (container_) {
        return;
    }
    if (parent == nullptr) {
        parent = lv_screen_active();
    }
    CreateUI(parent);
    LoadSavedHotspots();
    PopulateSavedHotspots();
    PopulateScanResults();
    RefreshCurrentNetwork();
    ESP_LOGI(TAG, "WiFi manager screen initialized");
}

void WifiManagerScreen::CreateUI(lv_obj_t* parent) {
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x10162b), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_clear_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    title_bar_ = lv_obj_create(container_);
    lv_obj_set_size(title_bar_, LV_HOR_RES, 60);
    lv_obj_align(title_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(title_bar_, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(title_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(title_bar_, 0, 0);
    lv_obj_set_style_border_width(title_bar_, 0, 0);
    lv_obj_set_style_pad_all(title_bar_, 8, 0);
    lv_obj_clear_flag(title_bar_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title_label = lv_label_create(title_bar_);
    lv_label_set_text(title_label, FONT_AWESOME_WIFI "  WiFi Manager");
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t* close_btn = lv_btn_create(title_bar_);
    lv_obj_set_size(close_btn, 50, 44);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, OnCloseButtonClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, FONT_AWESOME_XMARK);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
    lv_obj_center(close_label);

    content_ = lv_obj_create(container_);
    lv_obj_set_size(content_, LV_HOR_RES, LV_VER_RES - 60);
    lv_obj_align(content_, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 12, 0);
    lv_obj_set_style_pad_row(content_, 10, 0);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* current_card = lv_obj_create(content_);
    lv_obj_set_width(current_card, lv_pct(100));
    lv_obj_set_height(current_card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(current_card, lv_color_hex(0x1e2b4f), 0);
    lv_obj_set_style_bg_opa(current_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(current_card, 10, 0);
    lv_obj_set_style_border_width(current_card, 0, 0);
    lv_obj_set_style_pad_all(current_card, 12, 0);
    lv_obj_set_style_pad_row(current_card, 6, 0);
    lv_obj_set_flex_flow(current_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(current_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* current_title = lv_label_create(current_card);
    lv_label_set_text(current_title, "Current WiFi");
    lv_obj_set_style_text_color(current_title, lv_color_hex(0xb9d1ff), 0);

    current_ssid_label_ = lv_label_create(current_card);
    lv_label_set_text(current_ssid_label_, "SSID: Not connected");
    lv_obj_set_style_text_color(current_ssid_label_, lv_color_white(), 0);

    current_state_label_ = lv_label_create(current_card);
    lv_label_set_text(current_state_label_, "State: Disconnected");
    lv_obj_set_style_text_color(current_state_label_, lv_color_hex(0xa2acc7), 0);

    current_ip_label_ = lv_label_create(current_card);
    lv_label_set_text(current_ip_label_, "IP: -");
    lv_obj_set_style_text_color(current_ip_label_, lv_color_hex(0xa2acc7), 0);

    hint_label_ = lv_label_create(current_card);
    lv_label_set_text(hint_label_, "");
    lv_obj_set_style_text_color(hint_label_, lv_color_hex(0x9ce3b0), 0);

    scan_btn_ = lv_btn_create(content_);
    lv_obj_set_width(scan_btn_, lv_pct(100));
    lv_obj_set_height(scan_btn_, 50);
    lv_obj_set_style_bg_color(scan_btn_, lv_color_hex(0x1f6aa5), 0);
    lv_obj_set_style_bg_opa(scan_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(scan_btn_, 8, 0);
    lv_obj_set_style_border_width(scan_btn_, 0, 0);
    lv_obj_add_event_cb(scan_btn_, OnScanButtonClicked, LV_EVENT_CLICKED, this);

    scan_btn_label_ = lv_label_create(scan_btn_);
    lv_obj_set_style_text_color(scan_btn_label_, lv_color_white(), 0);
    lv_obj_center(scan_btn_label_);
    UpdateScanButton(false);

    lv_obj_t* saved_title = lv_label_create(content_);
    lv_label_set_text(saved_title, "Saved Hotspots (tap to connect)");
    lv_obj_set_style_text_color(saved_title, lv_color_hex(0xb9d1ff), 0);

    saved_list_ = lv_obj_create(content_);
    lv_obj_set_width(saved_list_, lv_pct(100));
    lv_obj_set_height(saved_list_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(saved_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(saved_list_, 0, 0);
    lv_obj_set_style_pad_all(saved_list_, 0, 0);
    lv_obj_set_style_pad_row(saved_list_, 8, 0);
    lv_obj_set_flex_flow(saved_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(saved_list_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* scan_title = lv_label_create(content_);
    lv_label_set_text(scan_title, "Available Networks");
    lv_obj_set_style_text_color(scan_title, lv_color_hex(0xb9d1ff), 0);

    scan_list_ = lv_obj_create(content_);
    lv_obj_set_width(scan_list_, lv_pct(100));
    lv_obj_set_height(scan_list_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(scan_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scan_list_, 0, 0);
    lv_obj_set_style_pad_all(scan_list_, 0, 0);
    lv_obj_set_style_pad_row(scan_list_, 8, 0);
    lv_obj_set_flex_flow(scan_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(scan_list_, LV_OBJ_FLAG_SCROLLABLE);

    CreatePasswordDialog();
}

void WifiManagerScreen::CreatePasswordDialog() {
    password_modal_ = lv_obj_create(container_);
    lv_obj_set_size(password_modal_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(password_modal_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(password_modal_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(password_modal_, LV_OPA_70, 0);
    lv_obj_set_style_radius(password_modal_, 0, 0);
    lv_obj_set_style_border_width(password_modal_, 0, 0);
    lv_obj_set_style_pad_all(password_modal_, 0, 0);
    lv_obj_clear_flag(password_modal_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(password_modal_, LV_OBJ_FLAG_HIDDEN);

    int panel_width = LV_HOR_RES - 40;
    if (panel_width > 460) {
        panel_width = 460;
    }

    lv_obj_t* panel = lv_obj_create(password_modal_);
    lv_obj_set_size(panel, panel_width, 210);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1a274a), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel_title = lv_label_create(panel);
    lv_label_set_text(panel_title, FONT_AWESOME_LOCK "  Enter Password");
    lv_obj_set_style_text_color(panel_title, lv_color_white(), 0);
    lv_obj_align(panel_title, LV_ALIGN_TOP_LEFT, 0, 0);

    password_ssid_label_ = lv_label_create(panel);
    lv_obj_set_width(password_ssid_label_, panel_width - 24);
    lv_label_set_long_mode(password_ssid_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(password_ssid_label_, "SSID: ");
    lv_obj_set_style_text_color(password_ssid_label_, lv_color_hex(0xb9d1ff), 0);
    lv_obj_align(password_ssid_label_, LV_ALIGN_TOP_LEFT, 0, 30);

    password_textarea_ = lv_textarea_create(panel);
    lv_obj_set_size(password_textarea_, panel_width - 24, 52);
    lv_obj_align(password_textarea_, LV_ALIGN_TOP_MID, 0, 62);
    lv_textarea_set_one_line(password_textarea_, true);
    lv_textarea_set_password_mode(password_textarea_, true);
    lv_textarea_set_placeholder_text(password_textarea_, "WiFi password");
    lv_textarea_set_max_length(password_textarea_, 64);

    lv_obj_t* cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, (panel_width - 36) / 2, 44);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 132);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x7a3241), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, OnPasswordCancelClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_center(cancel_label);

    lv_obj_t* connect_btn = lv_btn_create(panel);
    lv_obj_set_size(connect_btn, (panel_width - 36) / 2, 44);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, 0, 132);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x228b5a), 0);
    lv_obj_set_style_bg_opa(connect_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(connect_btn, 8, 0);
    lv_obj_set_style_border_width(connect_btn, 0, 0);
    lv_obj_add_event_cb(connect_btn, OnPasswordConnectClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_set_style_text_color(connect_label, lv_color_white(), 0);
    lv_obj_center(connect_label);

    static const char* key_map[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
        "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
        "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
        "z", "x", "c", "v", "b", "n", "m", "\n",
        "Space", "Del", "Clear", "Done", ""
    };
    password_keypad_ = lv_buttonmatrix_create(password_modal_);
    lv_obj_set_size(password_keypad_, LV_HOR_RES, LV_VER_RES / 2);
    lv_obj_align(password_keypad_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_buttonmatrix_set_map(password_keypad_, key_map);
    lv_buttonmatrix_set_button_ctrl_all(password_keypad_, LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
    lv_obj_add_event_cb(password_keypad_, OnPasswordKeypadClicked, LV_EVENT_VALUE_CHANGED, this);
}

bool WifiManagerScreen::EnsureWifiReady() {
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsInitialized()) {
        return true;
    }

    WifiManagerConfig config;
    if (!wifi.Initialize(config)) {
        ESP_LOGE(TAG, "Failed to initialize WiFi manager");
        SetHint("WiFi is unavailable");
        return false;
    }
    return true;
}

void WifiManagerScreen::Show() {
    if (!container_) {
        ESP_LOGE(TAG, "WiFi manager screen is not initialized");
        return;
    }

    is_visible_ = true;
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    HidePasswordDialog();

    EnsureWifiReady();
    LoadSavedHotspots();
    PopulateSavedHotspots();
    PopulateScanResults();
    RefreshCurrentNetwork();

    if (!status_timer_) {
        status_timer_ = lv_timer_create(OnStatusTimer, STATUS_TIMER_INTERVAL_MS, this);
    } else {
        lv_timer_resume(status_timer_);
    }

    if (!scan_in_progress_) {
        StartScan();
    }

    ESP_LOGI(TAG, "WiFi manager screen shown");
}

void WifiManagerScreen::Hide() {
    if (!container_) {
        return;
    }

    HidePasswordDialog();
    is_visible_ = false;
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (status_timer_) {
        lv_timer_pause(status_timer_);
    }

    if (close_callback_) {
        close_callback_();
    }
    ESP_LOGI(TAG, "WiFi manager screen hidden");
}

void WifiManagerScreen::LoadSavedHotspots() {
    saved_hotspots_.clear();
    const auto& saved = SsidManager::GetInstance().GetSsidList();
    saved_hotspots_.assign(saved.begin(), saved.end());
}

bool WifiManagerScreen::FindSavedPassword(const std::string& ssid, std::string& password) const {
    for (const auto& item : saved_hotspots_) {
        if (item.ssid == ssid) {
            password = item.password;
            return true;
        }
    }
    return false;
}

void WifiManagerScreen::PopulateSavedHotspots() {
    if (!saved_list_) {
        return;
    }

    lv_obj_clean(saved_list_);

    if (saved_hotspots_.empty()) {
        lv_obj_t* empty_label = lv_label_create(saved_list_);
        lv_label_set_text(empty_label, "No saved hotspots yet.");
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x8d96ad), 0);
        return;
    }

    std::string connected_ssid;
    bool is_connected = false;
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsInitialized() && wifi.IsConnected()) {
        connected_ssid = wifi.GetSsid();
        is_connected = true;
    }

    for (size_t i = 0; i < saved_hotspots_.size(); ++i) {
        const auto& item = saved_hotspots_[i];
        lv_obj_t* btn = lv_btn_create(saved_list_);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 56);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x203560), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2b4f87), LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, (void*)(uintptr_t)i);
        lv_obj_add_event_cb(btn, OnSavedItemClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon = lv_label_create(btn);
        lv_label_set_text(icon, FONT_AWESOME_KEY);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xffd166), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* ssid_label = lv_label_create(btn);
        lv_obj_set_width(ssid_label, LV_HOR_RES - 250);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(ssid_label, item.ssid.c_str());
        lv_obj_set_style_text_color(ssid_label, lv_color_white(), 0);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 30, 0);

        lv_obj_t* status = lv_label_create(btn);
        if (is_connected && connected_ssid == item.ssid) {
            lv_label_set_text(status, "Connected");
            lv_obj_set_style_text_color(status, lv_color_hex(0x8bffb9), 0);
        } else {
            lv_label_set_text(status, "Saved");
            lv_obj_set_style_text_color(status, lv_color_hex(0x95b7ef), 0);
        }
        lv_obj_align(status, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void WifiManagerScreen::PopulateScanResults() {
    if (!scan_list_) {
        return;
    }

    lv_obj_clean(scan_list_);

    if (scan_results_.empty()) {
        lv_obj_t* empty_label = lv_label_create(scan_list_);
        if (scan_in_progress_) {
            lv_label_set_text(empty_label, "Scanning...");
        } else {
            lv_label_set_text(empty_label, "No networks found.");
        }
        lv_obj_set_style_text_color(empty_label, lv_color_hex(0x8d96ad), 0);
        return;
    }

    std::string connected_ssid;
    bool is_connected = false;
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsInitialized() && wifi.IsConnected()) {
        connected_ssid = wifi.GetSsid();
        is_connected = true;
    }

    for (size_t i = 0; i < scan_results_.size(); ++i) {
        const auto& item = scan_results_[i];
        lv_obj_t* btn = lv_btn_create(scan_list_);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 58);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1f2e52), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2b4f87), LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(btn, (void*)(uintptr_t)i);
        lv_obj_add_event_cb(btn, OnScanItemClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* security_icon = lv_label_create(btn);
        lv_label_set_text(security_icon, RequiresPassword(item.authmode) ? FONT_AWESOME_LOCK : FONT_AWESOME_UNLOCK);
        lv_obj_set_style_text_color(security_icon, lv_color_hex(0xd9e1ff), 0);
        lv_obj_align(security_icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* ssid_label = lv_label_create(btn);
        lv_obj_set_width(ssid_label, LV_HOR_RES - 300);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(ssid_label, item.ssid.c_str());
        lv_obj_set_style_text_color(ssid_label, lv_color_white(), 0);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 30, 0);

        lv_obj_t* right_label = lv_label_create(btn);
        std::string saved_password;
        bool is_saved = FindSavedPassword(item.ssid, saved_password);
        char info[64];
        if (is_connected && connected_ssid == item.ssid) {
            snprintf(info, sizeof(info), "%ddBm  Connected", item.rssi);
            lv_obj_set_style_text_color(right_label, lv_color_hex(0x8bffb9), 0);
        } else if (is_saved) {
            snprintf(info, sizeof(info), "%ddBm  Saved", item.rssi);
            lv_obj_set_style_text_color(right_label, lv_color_hex(0x95b7ef), 0);
        } else {
            snprintf(info, sizeof(info), "%ddBm", item.rssi);
            lv_obj_set_style_text_color(right_label, lv_color_hex(0xbec7dd), 0);
        }
        lv_label_set_text(right_label, info);
        lv_obj_align(right_label, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void WifiManagerScreen::UpdateScanButton(bool scanning) {
    if (!scan_btn_label_) {
        return;
    }
    if (scanning) {
        lv_label_set_text(scan_btn_label_, FONT_AWESOME_ARROWS_ROTATE "  Scanning...");
    } else {
        lv_label_set_text(scan_btn_label_, FONT_AWESOME_ARROWS_ROTATE "  Scan Hotspots");
    }
    lv_obj_center(scan_btn_label_);
}

void WifiManagerScreen::SetHint(const std::string& text) {
    if (hint_label_) {
        lv_label_set_text(hint_label_, text.c_str());
    }
}

void WifiManagerScreen::RefreshCurrentNetwork() {
    if (!current_ssid_label_ || !current_state_label_ || !current_ip_label_) {
        return;
    }

    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        lv_label_set_text(current_ssid_label_, "SSID: -");
        lv_label_set_text(current_state_label_, "State: WiFi not initialized");
        lv_label_set_text(current_ip_label_, "IP: -");
        return;
    }

    if (wifi.IsConnected()) {
        std::string ssid = wifi.GetSsid();
        std::string ip = wifi.GetIpAddress();
        int rssi = wifi.GetRssi();
        int channel = wifi.GetChannel();
        char state_text[96];
        char ip_text[96];
        snprintf(state_text, sizeof(state_text), "State: Connected (%ddBm, Ch %d)", rssi, channel);
        snprintf(ip_text, sizeof(ip_text), "IP: %s", ip.empty() ? "-" : ip.c_str());
        lv_label_set_text_fmt(current_ssid_label_, "SSID: %s", ssid.empty() ? "-" : ssid.c_str());
        lv_label_set_text(current_state_label_, state_text);
        lv_label_set_text(current_ip_label_, ip_text);
        return;
    }

    if (connect_in_progress_ && !pending_connect_ssid_.empty()) {
        lv_label_set_text_fmt(current_ssid_label_, "SSID: %s", pending_connect_ssid_.c_str());
        lv_label_set_text(current_state_label_, "State: Connecting...");
    } else {
        lv_label_set_text(current_ssid_label_, "SSID: Not connected");
        lv_label_set_text(current_state_label_, "State: Disconnected");
    }
    lv_label_set_text(current_ip_label_, "IP: -");
}

void WifiManagerScreen::StartScan() {
    if (scan_in_progress_) {
        return;
    }
    if (!EnsureWifiReady()) {
        return;
    }

    auto* task_data = new ScanTaskResult();
    task_data->screen = this;
    scan_in_progress_ = true;
    UpdateScanButton(true);
    scan_results_.clear();
    PopulateScanResults();
    SetHint("Scanning nearby networks...");

    if (xTaskCreate(ScanTask, "wifi_scan_ui", SCAN_TASK_STACK_SIZE, task_data,
                    SCAN_TASK_PRIORITY, nullptr) != pdPASS) {
        delete task_data;
        scan_in_progress_ = false;
        UpdateScanButton(false);
        SetHint("Unable to start scan task");
        ESP_LOGE(TAG, "Failed to create WiFi scan task");
        return;
    }
}

void WifiManagerScreen::ScanTask(void* arg) {
    auto* result = static_cast<ScanTaskResult*>(arg);
    if (!result || !result->screen) {
        delete result;
        vTaskDelete(nullptr);
        return;
    }

    auto& wifi = WifiManager::GetInstance();
    result->success = wifi.Scan(result->results, 8000);
    lv_async_call(OnScanTaskFinished, result);
    vTaskDelete(nullptr);
}

void WifiManagerScreen::OnScanTaskFinished(void* user_data) {
    auto* result = static_cast<ScanTaskResult*>(user_data);
    if (!result) {
        return;
    }

    if (result->screen && result->screen->container_ && lv_obj_is_valid(result->screen->container_)) {
        result->screen->HandleScanFinished(result->success, std::move(result->results));
    }
    delete result;
}

void WifiManagerScreen::HandleScanFinished(bool success, std::vector<WifiScanResult>&& results) {
    scan_in_progress_ = false;
    UpdateScanButton(false);

    if (!success) {
        scan_results_.clear();
        PopulateScanResults();
        SetHint("Scan failed");
        ESP_LOGW(TAG, "WiFi scan failed");
        return;
    }

    std::vector<WifiScanResult> deduplicated;
    deduplicated.reserve(results.size());
    for (const auto& item : results) {
        if (item.ssid.empty()) {
            continue;
        }
        auto found = std::find_if(deduplicated.begin(), deduplicated.end(),
                                  [&item](const WifiScanResult& existing) {
                                      return existing.ssid == item.ssid;
                                  });
        if (found == deduplicated.end()) {
            deduplicated.push_back(item);
        } else if (item.rssi > found->rssi) {
            *found = item;
        }
    }

    std::sort(deduplicated.begin(), deduplicated.end(),
              [](const WifiScanResult& a, const WifiScanResult& b) {
                  return a.rssi > b.rssi;
              });

    scan_results_ = std::move(deduplicated);
    PopulateScanResults();
    char hint[64];
    snprintf(hint, sizeof(hint), "Found %u hotspot(s)", (unsigned)scan_results_.size());
    SetHint(hint);
}

void WifiManagerScreen::RequestConnection(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) {
        SetHint("SSID is empty");
        return;
    }
    if (password.size() > 64) {
        SetHint("Password is too long");
        return;
    }
    if (!EnsureWifiReady()) {
        return;
    }

    auto& wifi = WifiManager::GetInstance();
    if (!wifi.ConnectTo(ssid, password)) {
        SetHint("Failed to start connection");
        ESP_LOGE(TAG, "ConnectTo failed for SSID: %s", ssid.c_str());
        return;
    }

    pending_connect_ssid_ = ssid;
    pending_connect_password_ = password;
    pending_connect_started_us_ = esp_timer_get_time();
    connect_in_progress_ = true;
    SetHint("Connecting to " + ssid + "...");
    RefreshCurrentNetwork();
}

void WifiManagerScreen::HandleConnectionProgress() {
    if (!connect_in_progress_) {
        return;
    }
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        return;
    }

    if (wifi.IsConnected() && wifi.GetSsid() == pending_connect_ssid_) {
        connect_in_progress_ = false;
        SsidManager::GetInstance().AddSsid(pending_connect_ssid_, pending_connect_password_);
        SetHint("Connected to " + pending_connect_ssid_);
        pending_connect_ssid_.clear();
        pending_connect_password_.clear();
        pending_connect_started_us_ = 0;

        LoadSavedHotspots();
        PopulateSavedHotspots();
        PopulateScanResults();
        RefreshCurrentNetwork();
        return;
    }

    int64_t elapsed_us = esp_timer_get_time() - pending_connect_started_us_;
    if (elapsed_us > (int64_t)CONNECT_TIMEOUT_MS * 1000) {
        std::string failed_ssid = pending_connect_ssid_;
        connect_in_progress_ = false;
        pending_connect_ssid_.clear();
        pending_connect_password_.clear();
        pending_connect_started_us_ = 0;
        SetHint("Connection timed out: " + failed_ssid);
        RefreshCurrentNetwork();
    }
}

void WifiManagerScreen::ShowPasswordDialog(const std::string& ssid, const std::string& preset_password) {
    if (!password_modal_ || !password_textarea_ || !password_ssid_label_) {
        return;
    }
    selected_ssid_ = ssid;
    lv_label_set_text_fmt(password_ssid_label_, "SSID: %s", ssid.c_str());
    lv_textarea_set_text(password_textarea_, preset_password.c_str());
    lv_obj_remove_flag(password_modal_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(password_modal_);
}

void WifiManagerScreen::HidePasswordDialog() {
    if (!password_modal_) {
        return;
    }
    lv_obj_add_flag(password_modal_, LV_OBJ_FLAG_HIDDEN);
    selected_ssid_.clear();
}

void WifiManagerScreen::SubmitPasswordConnect() {
    if (selected_ssid_.empty() || !password_textarea_) {
        HidePasswordDialog();
        return;
    }

    std::string password = lv_textarea_get_text(password_textarea_);
    if (password.empty()) {
        SetHint("Password is required for this hotspot");
        return;
    }

    std::string target_ssid = selected_ssid_;
    HidePasswordDialog();
    RequestConnection(target_ssid, password);
}

void WifiManagerScreen::OnCloseButtonClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    if (screen) {
        screen->Hide();
    }
}

void WifiManagerScreen::OnScanButtonClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    if (screen) {
        screen->StartScan();
    }
}

void WifiManagerScreen::OnSavedItemClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    if (!screen) {
        return;
    }
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(target);
    if (index >= screen->saved_hotspots_.size()) {
        return;
    }
    const auto& item = screen->saved_hotspots_[index];
    screen->RequestConnection(item.ssid, item.password);
}

void WifiManagerScreen::OnScanItemClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    if (!screen) {
        return;
    }
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(target);
    if (index >= screen->scan_results_.size()) {
        return;
    }

    const auto& item = screen->scan_results_[index];
    if (item.ssid.empty()) {
        return;
    }

    std::string saved_password;
    bool has_saved = screen->FindSavedPassword(item.ssid, saved_password);
    if (!RequiresPassword(item.authmode)) {
        screen->RequestConnection(item.ssid, "");
        return;
    }

    if (has_saved && !saved_password.empty()) {
        screen->RequestConnection(item.ssid, saved_password);
    } else {
        screen->ShowPasswordDialog(item.ssid, saved_password);
    }
}

void WifiManagerScreen::OnPasswordConnectClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    if (screen) {
        screen->SubmitPasswordConnect();
    }
}

void WifiManagerScreen::OnPasswordCancelClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    if (screen) {
        screen->HidePasswordDialog();
    }
}

void WifiManagerScreen::OnPasswordKeypadClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    if (!screen || !screen->password_textarea_) {
        return;
    }

    auto* keypad = static_cast<lv_obj_t*>(lv_event_get_target(e));
    uint32_t id = lv_buttonmatrix_get_selected_button(keypad);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }

    const char* key = lv_buttonmatrix_get_button_text(keypad, id);
    if (key == nullptr) {
        return;
    }

    if (strcmp(key, "Del") == 0) {
        lv_textarea_delete_char(screen->password_textarea_);
    } else if (strcmp(key, "Clear") == 0) {
        lv_textarea_set_text(screen->password_textarea_, "");
    } else if (strcmp(key, "Space") == 0) {
        lv_textarea_add_char(screen->password_textarea_, ' ');
    } else if (strcmp(key, "Done") == 0) {
        screen->SubmitPasswordConnect();
    } else {
        lv_textarea_add_text(screen->password_textarea_, key);
    }
}

void WifiManagerScreen::OnStatusTimer(lv_timer_t* timer) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_timer_get_user_data(timer));
    if (!screen || !screen->container_ || !lv_obj_is_valid(screen->container_)) {
        return;
    }
    if (!screen->is_visible_) {
        return;
    }
    screen->HandleConnectionProgress();
    screen->RefreshCurrentNetwork();
}

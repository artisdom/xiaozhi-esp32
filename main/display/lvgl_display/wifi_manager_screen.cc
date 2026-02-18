#include "wifi_manager_screen.h"

#include <algorithm>
#include <map>
#include <cctype>
#include <cstring>
#include <cstdio>

#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <esp_lvgl_port.h>

#include <ssid_manager.h>

static const char* TAG = "WifiManagerScreen";

static constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
static constexpr int SCAN_TASK_STACK_SIZE = 4096;
static constexpr int SCAN_TASK_PRIORITY = 4;

static const char* SignalIconForRssi(int rssi) {
    if (rssi >= -60) {
        return FONT_AWESOME_SIGNAL_STRONG;
    }
    if (rssi >= -70) {
        return FONT_AWESOME_SIGNAL_GOOD;
    }
    if (rssi >= -80) {
        return FONT_AWESOME_SIGNAL_FAIR;
    }
    return FONT_AWESOME_SIGNAL_WEAK;
}

WifiManagerScreen::WifiManagerScreen() = default;

WifiManagerScreen::~WifiManagerScreen() {
    if (scan_in_progress_.load()) {
        int timeout = 50;
        while (scan_in_progress_.load() && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    if (connect_timer_) {
        lv_timer_del(connect_timer_);
        connect_timer_ = nullptr;
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
    ESP_LOGI(TAG, "WiFi manager screen initialized");
}

void WifiManagerScreen::CreateUI(lv_obj_t* parent) {
    container_ = lv_obj_create(parent);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0x1a1a2e), 0);
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

    title_label_ = lv_label_create(title_bar_);
    lv_obj_set_width(title_label_, LV_HOR_RES - 140);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(title_label_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_label_set_text(title_label_, "WiFi Manager");

    close_btn_ = lv_btn_create(title_bar_);
    lv_obj_set_size(close_btn_, 50, 44);
    lv_obj_align(close_btn_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close_btn_, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_radius(close_btn_, 8, 0);
    lv_obj_set_style_border_width(close_btn_, 0, 0);
    lv_obj_add_event_cb(close_btn_, OnCloseButtonClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* close_label = lv_label_create(close_btn_);
    lv_label_set_text(close_label, FONT_AWESOME_XMARK);
    lv_obj_center(close_label);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);

    info_bar_ = lv_obj_create(container_);
    lv_obj_set_size(info_bar_, LV_HOR_RES, 90);
    lv_obj_align(info_bar_, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(info_bar_, lv_color_hex(0x1a1a3e), 0);
    lv_obj_set_style_bg_opa(info_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_bar_, 0, 0);
    lv_obj_set_style_border_width(info_bar_, 0, 0);
    lv_obj_set_style_pad_all(info_bar_, 12, 0);
    lv_obj_clear_flag(info_bar_, LV_OBJ_FLAG_SCROLLABLE);

    current_ssid_label_ = lv_label_create(info_bar_);
    lv_obj_set_width(current_ssid_label_, LV_HOR_RES - 160);
    lv_label_set_long_mode(current_ssid_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(current_ssid_label_, LV_ALIGN_TOP_LEFT, 8, 0);
    lv_obj_set_style_text_color(current_ssid_label_, lv_color_white(), 0);
    lv_label_set_text(current_ssid_label_, "Current: --");

    status_label_ = lv_label_create(info_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES - 160);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_LEFT, 8, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(status_label_, "Ready to scan");

    scan_btn_ = lv_btn_create(info_bar_);
    lv_obj_set_size(scan_btn_, 110, 50);
    lv_obj_align(scan_btn_, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(scan_btn_, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_radius(scan_btn_, 8, 0);
    lv_obj_set_style_border_width(scan_btn_, 0, 0);
    lv_obj_add_event_cb(scan_btn_, OnScanButtonClicked, LV_EVENT_CLICKED, this);

    scan_btn_label_ = lv_label_create(scan_btn_);
    lv_label_set_text(scan_btn_label_, "Scan");
    lv_obj_center(scan_btn_label_);
    lv_obj_set_style_text_color(scan_btn_label_, lv_color_white(), 0);

    network_list_ = lv_obj_create(container_);
    lv_obj_set_size(network_list_, LV_HOR_RES, LV_VER_RES - 150);
    lv_obj_align(network_list_, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_bg_opa(network_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(network_list_, 0, 0);
    lv_obj_set_style_pad_all(network_list_, 12, 0);
    lv_obj_set_flex_flow(network_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(network_list_, 8, 0);
    lv_obj_set_scrollbar_mode(network_list_, LV_SCROLLBAR_MODE_AUTO);

    password_overlay_ = lv_obj_create(container_);
    lv_obj_set_size(password_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(password_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(password_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(password_overlay_, LV_OPA_70, 0);
    lv_obj_set_style_border_width(password_overlay_, 0, 0);
    lv_obj_set_style_pad_all(password_overlay_, 0, 0);
    lv_obj_add_flag(password_overlay_, LV_OBJ_FLAG_HIDDEN);

    password_panel_ = lv_obj_create(password_overlay_);
    lv_obj_set_size(password_panel_, LV_HOR_RES - 80, LV_VER_RES - 120);
    lv_obj_align(password_panel_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(password_panel_, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(password_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(password_panel_, 12, 0);
    lv_obj_set_style_border_width(password_panel_, 0, 0);
    lv_obj_set_style_pad_all(password_panel_, 12, 0);
    lv_obj_clear_flag(password_panel_, LV_OBJ_FLAG_SCROLLABLE);

    password_title_label_ = lv_label_create(password_panel_);
    lv_obj_set_width(password_title_label_, LV_HOR_RES - 120);
    lv_label_set_long_mode(password_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(password_title_label_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_color(password_title_label_, lv_color_white(), 0);
    lv_label_set_text(password_title_label_, "Enter password");

    password_textarea_ = lv_textarea_create(password_panel_);
    lv_obj_set_width(password_textarea_, LV_HOR_RES - 140);
    lv_obj_align(password_textarea_, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_textarea_set_one_line(password_textarea_, true);
    lv_textarea_set_password_mode(password_textarea_, true);
    lv_textarea_set_placeholder_text(password_textarea_, "Password");

    keyboard_container_ = lv_obj_create(password_panel_);
    lv_obj_set_size(keyboard_container_, LV_HOR_RES - 140, LV_VER_RES - 260);
    lv_obj_align(keyboard_container_, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_obj_set_style_bg_opa(keyboard_container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keyboard_container_, 0, 0);
    lv_obj_set_style_pad_all(keyboard_container_, 0, 0);
    lv_obj_set_flex_flow(keyboard_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(keyboard_container_, 6, 0);

    lv_obj_t* action_row = lv_obj_create(password_panel_);
    lv_obj_set_size(action_row, LV_HOR_RES - 140, 50);
    lv_obj_align(action_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(action_row, 12, 0);

    connect_btn_ = lv_btn_create(action_row);
    lv_obj_set_flex_grow(connect_btn_, 1);
    lv_obj_set_style_bg_color(connect_btn_, lv_color_hex(0x2e7d32), 0);
    lv_obj_set_style_radius(connect_btn_, 8, 0);
    lv_obj_set_style_border_width(connect_btn_, 0, 0);
    lv_obj_add_event_cb(connect_btn_, OnConnectButtonClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* connect_label = lv_label_create(connect_btn_);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_set_style_text_color(connect_label, lv_color_white(), 0);

    cancel_btn_ = lv_btn_create(action_row);
    lv_obj_set_flex_grow(cancel_btn_, 1);
    lv_obj_set_style_bg_color(cancel_btn_, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(cancel_btn_, 8, 0);
    lv_obj_set_style_border_width(cancel_btn_, 0, 0);
    lv_obj_add_event_cb(cancel_btn_, OnCancelButtonClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* cancel_label = lv_label_create(cancel_btn_);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);

    BuildKeyboard();
    PopulateNetworkList();
}

void WifiManagerScreen::Show() {
    if (!container_) {
        ESP_LOGE(TAG, "Container not initialized");
        return;
    }
    is_visible_.store(true);
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(container_);
    UpdateCurrentSsid();
    ShowStatus("Ready to scan");
}

void WifiManagerScreen::Hide() {
    if (container_) {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
    is_visible_.store(false);
    if (close_callback_) {
        close_callback_();
    }
}

void WifiManagerScreen::UpdateCurrentSsid() {
    auto& wifi = WifiManager::GetInstance();
    std::string label = "Current: ";
    if (wifi.IsConfigMode()) {
        label += "Config AP ";
        label += wifi.GetApSsid();
    } else if (wifi.IsConnected()) {
        label += wifi.GetSsid();
    } else {
        label += "Not connected";
    }
    lv_label_set_text(current_ssid_label_, label.c_str());
}

void WifiManagerScreen::ShowStatus(const char* text) {
    if (status_label_) {
        lv_label_set_text(status_label_, text);
    }
}

void WifiManagerScreen::PopulateNetworkList() {
    if (!network_list_) {
        return;
    }

    lv_obj_clean(network_list_);

    if (networks_.empty()) {
        lv_obj_t* empty = lv_label_create(network_list_);
        lv_label_set_text(empty, "No networks found");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    std::string current_ssid;
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) {
        current_ssid = wifi.GetSsid();
    }

    for (size_t i = 0; i < networks_.size(); ++i) {
        const auto& network = networks_[i];

        lv_obj_t* item = lv_obj_create(network_list_);
        lv_obj_set_size(item, LV_HOR_RES - 24, 64);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x16213e), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 10, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x0f3460), LV_STATE_PRESSED);

        if (!current_ssid.empty() && network.ssid == current_ssid) {
            lv_obj_set_style_bg_color(item, lv_color_hex(0x1f3a5a), 0);
        }

        lv_obj_set_user_data(item, (void*)(uintptr_t)i);
        lv_obj_add_event_cb(item, OnNetworkItemClicked, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lock_label = lv_label_create(item);
        lv_label_set_text(lock_label, network.authmode == WIFI_AUTH_OPEN ? FONT_AWESOME_UNLOCK : FONT_AWESOME_LOCK);
        lv_obj_align(lock_label, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_color(lock_label, lv_color_hex(0xffd700), 0);
        lv_obj_remove_flag(lock_label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* ssid_label = lv_label_create(item);
        lv_obj_set_width(ssid_label, LV_HOR_RES - 200);
        lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(ssid_label, network.ssid.c_str());
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 30, 0);
        lv_obj_set_style_text_color(ssid_label, lv_color_white(), 0);
        lv_obj_remove_flag(ssid_label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* signal_label = lv_label_create(item);
        lv_label_set_text(signal_label, SignalIconForRssi(network.rssi));
        lv_obj_align(signal_label, LV_ALIGN_RIGHT_MID, -50, 0);
        lv_obj_set_style_text_color(signal_label, lv_color_hex(0x6ab7ff), 0);
        lv_obj_remove_flag(signal_label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* rssi_label = lv_label_create(item);
        char rssi_text[16];
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", network.rssi);
        lv_label_set_text(rssi_label, rssi_text);
        lv_obj_align(rssi_label, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_text_color(rssi_label, lv_color_hex(0xcccccc), 0);
        lv_obj_remove_flag(rssi_label, LV_OBJ_FLAG_CLICKABLE);
    }
}

void WifiManagerScreen::StartScan() {
    if (scan_in_progress_.load()) {
        return;
    }
    scan_in_progress_.store(true);
    ShowStatus("Scanning...");
    lv_obj_add_state(scan_btn_, LV_STATE_DISABLED);

    xTaskCreate(ScanTask, "wifi_scan", SCAN_TASK_STACK_SIZE, this, SCAN_TASK_PRIORITY, &scan_task_handle_);
}

void WifiManagerScreen::HandleScanResults(const std::vector<WifiScanResult>& results) {
    std::map<std::string, WifiNetworkEntry> best;
    for (const auto& entry : results) {
        if (entry.ssid.empty()) {
            continue;
        }
        auto it = best.find(entry.ssid);
        if (it == best.end() || entry.rssi > it->second.rssi) {
            WifiNetworkEntry network;
            network.ssid = entry.ssid;
            network.rssi = entry.rssi;
            network.channel = entry.channel;
            network.authmode = entry.authmode;
            best[entry.ssid] = network;
        }
    }

    networks_.clear();
    networks_.reserve(best.size());
    for (const auto& item : best) {
        networks_.push_back(item.second);
    }

    std::sort(networks_.begin(), networks_.end(), [](const WifiNetworkEntry& a, const WifiNetworkEntry& b) {
        return a.rssi > b.rssi;
    });

    PopulateNetworkList();
}

void WifiManagerScreen::ShowPasswordPrompt(const WifiNetworkEntry& network) {
    selected_ssid_ = network.ssid;
    selected_authmode_ = network.authmode;

    std::string title = "Connect to ";
    title += network.ssid;
    lv_label_set_text(password_title_label_, title.c_str());

    lv_textarea_set_text(password_textarea_, "");
    keyboard_symbols_ = false;
    keyboard_uppercase_ = false;
    BuildKeyboard();

    lv_obj_remove_flag(password_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(password_overlay_);
}

void WifiManagerScreen::HidePasswordPrompt() {
    if (password_overlay_) {
        lv_obj_add_flag(password_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
}

void WifiManagerScreen::BuildKeyboard() {
    if (!keyboard_container_) {
        return;
    }

    lv_obj_clean(keyboard_container_);

    std::vector<std::vector<std::string>> rows;

    if (keyboard_symbols_) {
        rows = {
            {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"},
            {"-", "_", "=", "+", "[", "]", "{", "}", "\\", "|"},
            {";", ":", "'", "\"", ",", ".", "/", "?"},
            {"ABC", "SPACE", "BKSP", "CLEAR", "OK"}
        };
    } else {
        rows = {
            {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
            {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
            {"a", "s", "d", "f", "g", "h", "j", "k", "l"},
            {"SHIFT", "z", "x", "c", "v", "b", "n", "m", "BKSP"},
            {"SYMB", "SPACE", "CLEAR", "OK"}
        };
    }

    for (auto& row_keys : rows) {
        lv_obj_t* row = lv_obj_create(keyboard_container_);
        lv_obj_set_width(row, LV_HOR_RES - 160);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 6, 0);

        for (auto& key : row_keys) {
            std::string label = key;
            if (!keyboard_symbols_ && keyboard_uppercase_ && label.size() == 1 && isalpha(label[0])) {
                label[0] = static_cast<char>(toupper(label[0]));
            }

            lv_obj_t* btn = lv_btn_create(row);
            lv_obj_set_height(btn, 38);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x0f3460), 0);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_add_event_cb(btn, OnKeyButtonClicked, LV_EVENT_CLICKED, this);

            if (key == "SPACE") {
                lv_obj_set_flex_grow(btn, 4);
            } else if (key == "OK" || key == "CLEAR" || key == "SHIFT" || key == "BKSP" || key == "SYMB" || key == "ABC") {
                lv_obj_set_flex_grow(btn, 2);
            } else {
                lv_obj_set_flex_grow(btn, 1);
            }

            lv_obj_t* key_label = lv_label_create(btn);
            lv_label_set_text(key_label, label.c_str());
            lv_obj_center(key_label);
            lv_obj_set_style_text_color(key_label, lv_color_white(), 0);
        }
    }
}

void WifiManagerScreen::StartConnect(const std::string& ssid, const std::string& password) {
    if (ssid.empty()) {
        ShowStatus("Invalid SSID");
        return;
    }

    std::string status = "Connecting to ";
    status += ssid;
    ShowStatus(status.c_str());

    SsidManager::GetInstance().AddSsid(ssid, password);

    auto& wifi = WifiManager::GetInstance();
    if (!wifi.ConnectTo(ssid, password)) {
        ShowStatus("Failed to start connection");
        return;
    }

    connect_target_ssid_ = ssid;
    connect_start_tick_ = lv_tick_get();

    if (connect_timer_) {
        lv_timer_del(connect_timer_);
    }
    connect_timer_ = lv_timer_create(OnConnectTimer, 1000, this);
}

void WifiManagerScreen::OnCloseButtonClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    screen->Hide();
}

void WifiManagerScreen::OnScanButtonClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    screen->StartScan();
}

void WifiManagerScreen::OnNetworkItemClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    lv_obj_t* item = static_cast<lv_obj_t*>(lv_event_get_target(e));
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(item);
    if (index >= screen->networks_.size()) {
        return;
    }

    const auto& network = screen->networks_[index];
    if (network.authmode == WIFI_AUTH_OPEN) {
        screen->StartConnect(network.ssid, "");
        return;
    }

    screen->ShowPasswordPrompt(network);
}

void WifiManagerScreen::OnKeyButtonClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_obj_t* label_obj = lv_obj_get_child(btn, 0);
    if (!label_obj) {
        return;
    }
    const char* key = lv_label_get_text(label_obj);
    if (!key || !screen->password_textarea_) {
        return;
    }

    if (strcmp(key, "BKSP") == 0) {
        lv_textarea_delete_char(screen->password_textarea_);
        return;
    }
    if (strcmp(key, "SPACE") == 0) {
        lv_textarea_add_char(screen->password_textarea_, ' ');
        return;
    }
    if (strcmp(key, "CLEAR") == 0) {
        lv_textarea_set_text(screen->password_textarea_, "");
        return;
    }
    if (strcmp(key, "OK") == 0) {
        std::string password = lv_textarea_get_text(screen->password_textarea_);
        screen->HidePasswordPrompt();
        screen->StartConnect(screen->selected_ssid_, password);
        return;
    }
    if (strcmp(key, "SHIFT") == 0) {
        screen->keyboard_uppercase_ = !screen->keyboard_uppercase_;
        screen->BuildKeyboard();
        return;
    }
    if (strcmp(key, "SYMB") == 0) {
        screen->keyboard_symbols_ = true;
        screen->keyboard_uppercase_ = false;
        screen->BuildKeyboard();
        return;
    }
    if (strcmp(key, "ABC") == 0) {
        screen->keyboard_symbols_ = false;
        screen->keyboard_uppercase_ = false;
        screen->BuildKeyboard();
        return;
    }

    lv_textarea_add_text(screen->password_textarea_, key);
}

void WifiManagerScreen::OnConnectButtonClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    std::string password = lv_textarea_get_text(screen->password_textarea_);
    screen->HidePasswordPrompt();
    screen->StartConnect(screen->selected_ssid_, password);
}

void WifiManagerScreen::OnCancelButtonClicked(lv_event_t* e) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_event_get_user_data(e));
    screen->HidePasswordPrompt();
}

void WifiManagerScreen::OnConnectTimer(lv_timer_t* timer) {
    auto* screen = static_cast<WifiManagerScreen*>(lv_timer_get_user_data(timer));
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConnected() && wifi.GetSsid() == screen->connect_target_ssid_) {
        screen->ShowStatus("Connected");
        screen->UpdateCurrentSsid();
        lv_timer_del(screen->connect_timer_);
        screen->connect_timer_ = nullptr;
        return;
    }

    uint32_t elapsed = lv_tick_elaps(screen->connect_start_tick_);
    if (elapsed > CONNECT_TIMEOUT_MS) {
        screen->ShowStatus("Connection failed");
        lv_timer_del(screen->connect_timer_);
        screen->connect_timer_ = nullptr;
    }
}

void WifiManagerScreen::ScanTask(void* arg) {
    auto* screen = static_cast<WifiManagerScreen*>(arg);

    std::vector<WifiScanResult> results;
    auto& wifi = WifiManager::GetInstance();
    bool ok = wifi.Scan(results);

    if (lvgl_port_lock(0)) {
        if (ok) {
            screen->HandleScanResults(results);
            screen->ShowStatus("Scan complete");
        } else {
            screen->ShowStatus("Scan failed");
        }
        lv_obj_clear_state(screen->scan_btn_, LV_STATE_DISABLED);
        lvgl_port_unlock();
    }

    screen->scan_in_progress_.store(false);
    screen->scan_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

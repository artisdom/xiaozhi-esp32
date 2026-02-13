#include "ina226.h"
#include <cmath>
#include <driver/i2c_master.h>
#include <esp_log.h>

#define TAG "INA226"

// Voltage to battery level mapping for NP-F550 2S Li-Ion battery (7.4V nominal)
// Full charge: 8.23V, Shutdown threshold: 6.0V
// Based on M5Stack Tab5 official documentation
static const struct {
    float voltage;
    int level;
} kVoltageLevelTable[] = {
    {8.40f, 100},
    {8.30f, 95},
    {8.20f, 90},
    {8.10f, 85},
    {8.00f, 80},
    {7.90f, 75},
    {7.80f, 70},
    {7.70f, 65},
    {7.60f, 60},
    {7.50f, 55},
    {7.40f, 50},
    {7.30f, 45},
    {7.20f, 40},
    {7.10f, 35},
    {7.00f, 30},
    {6.80f, 25},
    {6.60f, 20},
    {6.40f, 15},
    {6.20f, 10},
    {6.10f, 5},
    {6.00f, 0},
};

Ina226::Ina226(i2c_master_bus_handle_t i2c_bus, uint8_t addr) 
    : I2cDevice(i2c_bus, addr) {
    ESP_LOGI(TAG, "INA226 initialized at address 0x%02X", addr);
}

void Ina226::Configure(ina226_averages_t avg,
                       ina226_bus_conv_time_t bus_conv_time,
                       ina226_shunt_conv_time_t shunt_conv_time,
                       ina226_mode_t mode) {
    uint16_t config = 0;
    config |= (avg << 9 | bus_conv_time << 6 | shunt_conv_time << 3 | mode);
    WriteRegister16(INA226_REG_CONFIG, config);
    ESP_LOGI(TAG, "INA226 configured: avg=%d, bus_conv=%d, shunt_conv=%d, mode=%d", 
             avg, bus_conv_time, shunt_conv_time, mode);
}

void Ina226::Calibrate(float r_shunt, float i_max_expected) {
    r_shunt_ = r_shunt;
    
    float minimum_lsb = i_max_expected / 32767.0f;
    
    // Round to nearest value
    current_lsb_ = (uint32_t)(minimum_lsb * 100000000);
    current_lsb_ /= 100000000;
    current_lsb_ /= 0.0001f;
    current_lsb_ = ceilf(current_lsb_);
    current_lsb_ *= 0.0001f;
    
    power_lsb_ = current_lsb_ * 25.0f;
    
    uint16_t calibration_value = (uint16_t)((0.00512f) / (current_lsb_ * r_shunt));
    WriteRegister16(INA226_REG_CALIBRATION, calibration_value);
    
    ESP_LOGI(TAG, "INA226 calibrated: r_shunt=%.4f, i_max=%.2f, current_lsb=%.6f", 
             r_shunt, i_max_expected, current_lsb_);
}

int16_t Ina226::ReadRegister16(uint8_t reg) {
    uint8_t data[2] = {0};
    i2c_master_transmit_receive(i2c_device_, &reg, 1, data, 2, 100);
    return data[0] << 8 | data[1];
}

void Ina226::WriteRegister16(uint8_t reg, uint16_t value) {
    uint8_t data[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    i2c_master_transmit(i2c_device_, data, 3, 100);
}

float Ina226::ReadBusVoltage() {
    int16_t voltage = ReadRegister16(INA226_REG_BUSVOLTAGE);
    float voltage_v = voltage * 0.00125f;  // 1.25mV per bit
    // ESP_LOGI(TAG, "ReadBusVoltage: raw=%d, voltage=%.4fV", voltage, voltage_v);
    return voltage_v;
}

float Ina226::ReadShuntVoltage() {
    int16_t voltage = ReadRegister16(INA226_REG_SHUNTVOLTAGE);
    return voltage * 0.0000025f;  // 2.5uV per bit
}

float Ina226::ReadCurrent() {
    int16_t current = ReadRegister16(INA226_REG_CURRENT);
    // Negate to match Tab5 hardware wiring convention:
    // After negation: Positive = charging, Negative = discharging
    float current_amps = -current * current_lsb_;
    // ESP_LOGI(TAG, "ReadCurrent: raw=%d, current=%.4fA", current, current_amps);
    return current_amps;
}

float Ina226::ReadPower() {
    return ReadRegister16(INA226_REG_POWER) * power_lsb_;
}

int Ina226::VoltageToBatteryLevel(float voltage) {
    const int table_size = sizeof(kVoltageLevelTable) / sizeof(kVoltageLevelTable[0]);
    
    // Handle edge cases
    if (voltage >= kVoltageLevelTable[0].voltage) {
        return 100;
    }
    if (voltage <= kVoltageLevelTable[table_size - 1].voltage) {
        return 0;
    }
    
    // Find the two points to interpolate between
    for (int i = 0; i < table_size - 1; i++) {
        if (voltage <= kVoltageLevelTable[i].voltage && 
            voltage > kVoltageLevelTable[i + 1].voltage) {
            // Linear interpolation
            float v1 = kVoltageLevelTable[i].voltage;
            float v2 = kVoltageLevelTable[i + 1].voltage;
            int l1 = kVoltageLevelTable[i].level;
            int l2 = kVoltageLevelTable[i + 1].level;
            
            float ratio = (voltage - v2) / (v1 - v2);
            return (int)(l2 + ratio * (l1 - l2));
        }
    }
    
    return 0;
}

bool Ina226::IsCharging() {
    float current = ReadCurrent();
    // ESP_LOGI(TAG, "IsCharging: current=%.4fA", current);
    // Consider charging if current is above threshold (account for noise)
    // Positive current = charging
    return current > 0.05f;  // 50mA threshold
}

bool Ina226::IsDischarging() {
    float current = ReadCurrent();
    // ESP_LOGI(TAG, "IsDischarging: current=%.4fA", current);
    // Consider discharging if current is below negative threshold
    // Negative current = discharging
    return current < -0.05f;  // -50mA threshold
}

int Ina226::GetBatteryLevel() {
    float voltage = ReadBusVoltage();
    int level = VoltageToBatteryLevel(voltage);
    // ESP_LOGI(TAG, "GetBatteryLevel: voltage=%.4fV, level=%d%%", voltage, level);
    return level;
}

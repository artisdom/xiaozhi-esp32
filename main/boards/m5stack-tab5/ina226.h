#ifndef INA226_H
#define INA226_H

#include <cstdint>
#include "i2c_device.h"

// INA226 I2C Address
#define INA226_DEFAULT_ADDR 0x41

// INA226 Registers
#define INA226_REG_CONFIG       0x00
#define INA226_REG_SHUNTVOLTAGE 0x01
#define INA226_REG_BUSVOLTAGE   0x02
#define INA226_REG_POWER        0x03
#define INA226_REG_CURRENT      0x04
#define INA226_REG_CALIBRATION  0x05
#define INA226_REG_MASKENABLE   0x06
#define INA226_REG_ALERTLIMIT   0x07

// INA226 Configuration settings
typedef enum {
    INA226_AVERAGES_1    = 0b000,
    INA226_AVERAGES_4    = 0b001,
    INA226_AVERAGES_16   = 0b010,
    INA226_AVERAGES_64   = 0b011,
    INA226_AVERAGES_128  = 0b100,
    INA226_AVERAGES_256  = 0b101,
    INA226_AVERAGES_512  = 0b110,
    INA226_AVERAGES_1024 = 0b111
} ina226_averages_t;

typedef enum {
    INA226_BUS_CONV_TIME_140US  = 0b000,
    INA226_BUS_CONV_TIME_204US  = 0b001,
    INA226_BUS_CONV_TIME_332US  = 0b010,
    INA226_BUS_CONV_TIME_588US  = 0b011,
    INA226_BUS_CONV_TIME_1100US = 0b100,
    INA226_BUS_CONV_TIME_2116US = 0b101,
    INA226_BUS_CONV_TIME_4156US = 0b110,
    INA226_BUS_CONV_TIME_8244US = 0b111
} ina226_bus_conv_time_t;

typedef enum {
    INA226_SHUNT_CONV_TIME_140US  = 0b000,
    INA226_SHUNT_CONV_TIME_204US  = 0b001,
    INA226_SHUNT_CONV_TIME_332US  = 0b010,
    INA226_SHUNT_CONV_TIME_588US  = 0b011,
    INA226_SHUNT_CONV_TIME_1100US = 0b100,
    INA226_SHUNT_CONV_TIME_2116US = 0b101,
    INA226_SHUNT_CONV_TIME_4156US = 0b110,
    INA226_SHUNT_CONV_TIME_8244US = 0b111
} ina226_shunt_conv_time_t;

typedef enum {
    INA226_MODE_POWER_DOWN       = 0b000,
    INA226_MODE_SHUNT_TRIG       = 0b001,
    INA226_MODE_BUS_TRIG         = 0b010,
    INA226_MODE_SHUNT_BUS_TRIG   = 0b011,
    INA226_MODE_ADC_OFF          = 0b100,
    INA226_MODE_SHUNT_CONT       = 0b101,
    INA226_MODE_BUS_CONT         = 0b110,
    INA226_MODE_SHUNT_BUS_CONT   = 0b111
} ina226_mode_t;

/**
 * @brief INA226 Power Monitor class
 * 
 * This class interfaces with the INA226 power monitor IC to read
 * bus voltage, shunt voltage, current, and power.
 */
class Ina226 : public I2cDevice {
public:
    Ina226(i2c_master_bus_handle_t i2c_bus, uint8_t addr = INA226_DEFAULT_ADDR);
    
    /**
     * @brief Configure the INA226
     */
    void Configure(ina226_averages_t avg = INA226_AVERAGES_16,
                   ina226_bus_conv_time_t bus_conv_time = INA226_BUS_CONV_TIME_1100US,
                   ina226_shunt_conv_time_t shunt_conv_time = INA226_SHUNT_CONV_TIME_1100US,
                   ina226_mode_t mode = INA226_MODE_SHUNT_BUS_CONT);
    
    /**
     * @brief Calibrate the INA226
     * @param r_shunt Shunt resistor value in ohms
     * @param i_max_expected Maximum expected current in amps
     */
    void Calibrate(float r_shunt, float i_max_expected);
    
    /**
     * @brief Read bus voltage
     * @return Voltage in volts
     */
    float ReadBusVoltage();
    
    /**
     * @brief Read shunt voltage
     * @return Shunt voltage in volts
     */
    float ReadShuntVoltage();
    
    /**
     * @brief Read current
     * @return Current in amps (positive = charging, negative = discharging)
     */
    float ReadCurrent();
    
    /**
     * @brief Read power
     * @return Power in watts
     */
    float ReadPower();
    
    /**
     * @brief Convert voltage to battery level percentage
     * @param voltage Battery voltage
     * @return Battery level (0-100)
     */
    int VoltageToBatteryLevel(float voltage);
    
    /**
     * @brief Check if battery is charging
     * @return true if charging (positive current flow)
     */
    bool IsCharging();
    
    /**
     * @brief Check if battery is discharging
     * @return true if discharging (negative current flow)
     */
    bool IsDischarging();
    
    /**
     * @brief Get battery level percentage
     * @return Battery level (0-100)
     */
    int GetBatteryLevel();

private:
    float current_lsb_ = 0;
    float power_lsb_ = 0;
    float r_shunt_ = 0;
    
    int16_t ReadRegister16(uint8_t reg);
    void WriteRegister16(uint8_t reg, uint16_t value);
};

#endif // INA226_H

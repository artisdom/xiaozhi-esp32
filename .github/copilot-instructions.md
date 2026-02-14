# Xiaozhi ESP32 - AI Coding Assistant Instructions

## Project Overview

This is **xiaozhi-esp32**, an open-source AI voice chatbot project for ESP32 series microcontrollers. The project implements voice interaction capabilities using the Model Context Protocol (MCP) to communicate with AI backend services.

**Key Features:**
- Voice wake word detection and speech recognition
- Real-time audio streaming with Opus codec
- Support for 70+ ESP32 hardware boards
- LVGL-based display UI with emoji animations
- OTA (Over-The-Air) firmware updates
- MCP protocol for AI tool integration

## Technology Stack

| Component | Technology |
|-----------|------------|
| Framework | ESP-IDF v5.x (v5.5.2 for ESP32-P4) |
| Language | C++17, C99 |
| RTOS | FreeRTOS |
| Display | LVGL 9 |
| Audio Codec | Opus, ES8311/ES8374/ES8388/ES8389 |
| Networking | WebSocket, MQTT |
| Build System | CMake, ESP-IDF components |
| Targets | ESP32, ESP32-S3, ESP32-C3, ESP32-C5, ESP32-C6, ESP32-P4 |

## Project Structure

```
xiaozhi-esp32/
├── main/
│   ├── application.cc/h      # Main application singleton, event loop
│   ├── main.cc               # Entry point
│   ├── mcp_server.cc/h       # MCP protocol server implementation
│   ├── ota.cc/h              # OTA update handling
│   ├── settings.cc/h         # NVS-based settings storage
│   ├── system_info.cc/h      # System information utilities
│   ├── device_state_machine.cc/h  # State machine for device modes
│   ├── audio/                # Audio subsystem
│   │   ├── audio_service.cc/h    # Main audio orchestrator
│   │   ├── audio_codec.cc/h      # Audio codec abstraction
│   │   ├── codecs/               # Hardware codec implementations
│   │   ├── demuxer/              # OGG demuxer
│   │   └── processors/           # Audio processing utilities
│   ├── boards/               # Hardware board abstractions (70+ boards)
│   │   ├── common/           # Shared board utilities
│   │   │   ├── board.cc/h        # Base Board class
│   │   │   ├── wifi_board.cc/h   # WiFi-enabled board base
│   │   │   ├── ml307_board.cc/h  # 4G modem board base
│   │   │   └── ...
│   │   └── [board-name]/     # Per-board implementations
│   │       ├── [board-name].cc   # Board initialization
│   │       ├── config.h          # Pin mappings and hardware config
│   │       └── config.json       # Build configuration
│   ├── display/              # Display subsystem
│   │   ├── display.cc/h          # Display abstraction
│   │   ├── lcd_display.cc/h      # LCD display implementation
│   │   ├── oled_display.cc/h     # OLED display implementation
│   │   └── lvgl_display/         # LVGL-specific components
│   │       ├── lvgl_display.cc/h
│   │       ├── file_browser.cc/h # SD card file browser
│   │       ├── file_viewer.cc/h  # File viewer (image/audio/video)
│   │       └── jpg/              # JPEG encoder/decoder
│   ├── led/                  # LED indicator implementations
│   ├── protocols/            # Communication protocols
│   │   ├── protocol.cc/h         # Protocol base class
│   │   ├── websocket_protocol.cc/h
│   │   └── mqtt_protocol.cc/h
│   └── assets/               # Embedded assets (fonts, sounds)
├── docs/                     # Documentation
├── scripts/                  # Build and release scripts
├── partitions/               # Partition table definitions
└── managed_components/       # ESP-IDF component dependencies
```

## Architecture Patterns

### 1. Application Singleton
The main `Application` class follows singleton pattern and manages:
- Event loop processing (FreeRTOS event groups)
- Device state machine transitions
- Audio service lifecycle
- Network callbacks coordination

```cpp
Application& app = Application::GetInstance();
app.Schedule([](){ /* callback runs in main task */ });
```

### 2. Board Abstraction
Each hardware board inherits from a base class hierarchy:
- `Board` → Base class (UUID, display, LED, battery)
- `WifiBoard` → WiFi connectivity
- `Ml307Board` → 4G modem connectivity
- `DualNetworkBoard` → WiFi + 4G

To add a new board:
1. Create directory: `main/boards/[vendor]-[model]/`
2. Add `config.h` with pin definitions
3. Add `config.json` with build config
4. Implement `[name].cc` inheriting appropriate base class
5. Register in `main/CMakeLists.txt`

### 3. Audio Service Architecture
Three FreeRTOS tasks handle audio:
- **Input Task**: Captures microphone data
- **Output Task**: Plays audio to speakers
- **Opus Codec Task**: Encoding/decoding

Key classes:
- `AudioService`: Orchestrates audio pipeline
- `AudioCodec`: Hardware codec abstraction
- `AudioProcessor`: Wake word, VAD processing

### 4. Display Subsystem
LVGL 9 runs in a dedicated FreeRTOS task:
- `Display`: Abstract base class
- `LcdDisplay`: LCD panel implementation (SPI, RGB, MIPI-DSI)
- `OledDisplay`: OLED display implementation
- LVGL task requires adequate stack (10KB+ for MIPI displays)

### 5. Protocol Layer
Communication with AI backend uses:
- `WebSocketProtocol`: Primary WebSocket connection
- `MqttProtocol`: MQTT alternative
- JSON-RPC 2.0 over MCP for tool calls

## Code Style Guidelines

### Formatting
- Use `clang-format` with project `.clang-format` file
- 4-space indentation
- 100-character line width
- Run before committing: `clang-format -i <file>`

### Naming Conventions
- Classes: `PascalCase` (e.g., `AudioService`, `WifiBoard`)
- Functions/Methods: `PascalCase` (e.g., `Initialize()`, `GetInstance()`)
- Variables: `snake_case` (e.g., `sample_rate`, `audio_codec_`)
- Member variables: `snake_case_` with trailing underscore
- Constants/Macros: `SCREAMING_SNAKE_CASE` (e.g., `AUDIO_INPUT_SAMPLE_RATE`)
- Files: `snake_case.cc`, `snake_case.h`

### C++ Standards
- Use C++17 features (structured bindings, if-init, etc.)
- Prefer `std::string_view` for read-only string parameters
- Use smart pointers (`std::unique_ptr`, `std::shared_ptr`)
- Avoid raw `new`/`delete` where possible
- Use `ESP_LOG*` macros for logging

### ESP-IDF Patterns
- Check return values with `ESP_ERROR_CHECK()` for critical calls
- Use `ESP_LOGI/W/E/D` for logging with appropriate TAG
- Define `#define TAG "ComponentName"` at file top
- Use NVS (Non-Volatile Storage) via `Settings` class for persistence

## Build Instructions

### Prerequisites
- ESP-IDF v5.x installed and configured
- Python 3.8+

### Building
```bash
source ~/.espressif/tools/activate_idf_v5.5.2.sh

# 1. Incremental build (fast)
$IDF_PATH/tools/idf.py -DBOARD_NAME="m5stack-tab5" -DBOARD_TYPE="m5stack-tab5" build

# 2. Fresh build using ESP-IDF build system for the "M5Stack Tab5" board
$IDF_PATH/tools/idf.py set-target esp32p4

Append the following "CONFIG" to `sdkconfig`:

CONFIG_BOARD_TYPE_M5STACK_CORE_TAB5=y
CONFIG_CAMERA_SC202CS=y
CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE=y
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=13
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=12
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=11
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1=10
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1=9
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1=8

# Then run:
$IDF_PATH/tools/idf.py -DBOARD_NAME="m5stack-tab5" -DBOARD_TYPE="m5stack-tab5" build

# 3. Or use release script for "M5Stack Tab5" board
python scripts/release.py m5stack-tab5
```

### Flashing
```bash
$IDF_PATH/tools/idf.py -p /dev/ttyACM0 flash monitor
```

## Important Files for Modifications

| Task | Primary Files |
|------|---------------|
| Add new board | `main/boards/[new-board]/`, `main/CMakeLists.txt` |
| Modify audio | `main/audio/audio_service.cc`, `main/audio/codecs/` |
| Change display UI | `main/display/lvgl_display/` |
| Protocol changes | `main/protocols/`, `main/mcp_server.cc` |
| Add MCP tools | `main/mcp_server.cc`, board-specific files |
| OTA logic | `main/ota.cc` |
| State machine | `main/device_state_machine.cc` |

## Testing Considerations

- Test on actual hardware when possible
- Use `ESP_LOGI/D` for debugging, not `printf`
- Monitor heap usage with `heap_caps_get_free_size()`
- Check stack usage with FreeRTOS stack watermark APIs
- For LVGL issues, ensure adequate task stack size (8KB-12KB)

## Common Pitfalls

1. **Stack Overflow**: LVGL tasks need 8-12KB stack on complex displays
2. **Dangling References**: Pass `std::string` by value in callbacks/lambda captures
3. **Hardware JPEG Decoder**: ESP32-P4 doesn't support progressive JPEGs
4. **I2C Conflicts**: Audio codec and touch controller often share I2C bus
5. **Audio Buffer Timing**: Opus frames must align with server expectations (60ms default)
6. **PSRAM Usage**: Large buffers (display, audio) should use PSRAM when available

## Documentation References

- [Code Style Guide](docs/code_style.md)
- [Custom Board Guide](docs/custom-board.md)
- [MCP Protocol](docs/mcp-protocol.md)
- [WebSocket Protocol](docs/websocket.md)
- [MQTT/UDP Protocol](docs/mqtt-udp.md)
- [Audio Architecture](main/audio/README.md)

## Language Note

Code comments and documentation may be in Chinese (中文) or English. The codebase is bilingual - when adding comments, match the surrounding context or use English for technical terms.

## Reference Implementations
Camera, microphone and other functionalities
You can always use /home/ted/w/esp32/M5Tab5-UserDemo as a reference, that already has camera, microphone and other functionalities inside.
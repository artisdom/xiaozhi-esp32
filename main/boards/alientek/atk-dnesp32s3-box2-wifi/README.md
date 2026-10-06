# ATK-DNESP32S3-BOX2 Wi-Fi

Build with ESP-IDF v6.1:

```sh
source /home/nws/esp/esp-idf-v6.1/export.sh
python3 scripts/build.py alientek/atk-dnesp32s3-box2-wifi --name atk-dnesp32s3-box2-wifi
```

## SD-card MP3 playback

Insert a FAT32 card and put `.mp3` files in a `MUSIC` directory at its root.
For example: `MUSIC/Artist - Song.mp3`. Files may have UTF-8 names, with a maximum
of 255 bytes. Subdirectories and other audio formats are not supported.
Use a card reader to copy files; this firmware does not export USB storage.
Do not remove the card during playback. After removing or replacing a mounted
card, restart the board.

The onboard slot uses SPI2: CS GPIO15, MOSI GPIO16, SCLK GPIO17 and MISO GPIO18.
These assignments come from Espressif's
[BOX2 peripherals](https://github.com/espressif/esp-board-manager/blob/a49bf6e8aeadd8f60faad0a8e86383159c4816f2/esp_friends_boards/esp32_s3_box_2/board_peripherals.yaml)
and [SD device definition](https://github.com/espressif/esp-board-manager/blob/a49bf6e8aeadd8f60faad0a8e86383159c4816f2/esp_friends_boards/esp32_s3_box_2/board_devices.yaml).
Mounting is lazy; a missing or unreadable card leaves voice-assistant operation
available and is reported through `self.music.status`. Cards are never formatted
automatically.

### Controls

- Double-click **M** to play the first MP3 in directory order, or stop/cancel music.
- Single-click **M** to stop music and toggle chat, as before.
- Volume buttons and their long-press actions remain available.
- Ask the assistant to list SD-card music, play a named track, or stop music.
  This requires the connected assistant to support device MCP tool calls.

Available MCP tools:

| Tool | Arguments | Result |
| --- | --- | --- |
| `self.music.list` | `offset` (default 0), `limit` (default 20, maximum 64) | `files`, `next_offset` |
| `self.music.play` | `filename` (exact basename; empty selects first track) | Queued request |
| `self.music.stop` | None | Cancels pending/current playback |
| `self.music.status` | None | `busy`, `phase`, `filename`, `error` |

Listing uses directory order and scans at most 4096 directory entries. Continue
with `next_offset` until an empty `files` array is returned. Stop current music
before requesting another track. The play response acknowledges a queued request;
check status for mount, file or decoding errors.

Playback waits for two seconds without queued conversation audio, then ends
auto-listening and starts while the application is idle. Requests time out after
30 seconds if the device stays busy. Wake-word detection stays available; waking
the assistant, starting chat, alerts, notifications and incoming speech audio
interrupt music. Interrupted tracks do not resume automatically.

MP3 decoding and SD reads run in a dedicated task. Mono/stereo 16-bit MP3 at
8–48 kHz is downmixed to mono and resampled to the codec's existing 24 kHz output.
PCM uses the audio service's bounded playback queue; the player never writes
directly to I2S or changes the codec sample rate. Input, decoded and resampled
buffers are allocated once per track. Cancellation discards queued music, but a
small PCM chunk already being written to the codec may finish.

### Hardware validation

Before release, verify card mounting, mono/stereo MP3 at 8/16/24/44.1/48 kHz,
UTF-8 filenames, ID3-tagged/VBR tracks, end of file, invalid files, absent cards,
play/stop/restart, volume, and M-button single/double/long presses. Also verify
microphone capture, speech playback, wake/VAD interruption, reconnection, battery
sleep/shutdown behavior and applicable AEC modes. A successful firmware build
does not validate these on physical hardware.

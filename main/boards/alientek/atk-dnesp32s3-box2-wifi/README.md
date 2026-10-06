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

### Music screen and buttons

The 240×320 music screen shows scrolling title, artist and album/filename, a
progress bar, elapsed/total time, playback state, track number, volume, repeat
and shuffle. It appears for music and yields to the conversation screen while
the assistant is active. It returns when the assistant becomes idle.

| Button | Action |
| --- | --- |
| Double-click **M** from chat | Open music and start/resume the selected track; initially selects the first track |
| Single-click **M** in music | Pause/resume; starts the selected track if stopped |
| Double-click **M** in music | Stop and return to chat |
| Single-click **M** from chat | Existing chat toggle |
| Single-click **L/R** | Volume down/up |
| Double-click **L/R** | Previous/next track |
| Long-press **L/R** | Existing mute/maximum volume actions |
| Long-press **M** | Existing power-off action |

Previous restarts the current track after three seconds of playback; before
that it selects the previous track. Manual navigation wraps at the ends.
Button single-click actions run after the double-click detection interval.
Wake-word detection remains available during music.

### Voice and MCP controls

Ask the connected assistant to list/search SD-card music, play a named track,
pause, resume, skip tracks, seek to a time, or enable shuffle/repeat. Voice
commands require an assistant that supports device MCP calls.

| Tool | Arguments | Result/behavior |
| --- | --- | --- |
| `self.music.list` | `offset` (default 0), `limit` (default 20, maximum 64), `query` (optional filename substring), `refresh` (default false) | `files`, `total`, `next_offset`, `truncated`, `scanning` |
| `self.music.play` | `filename` (exact basename; empty starts selected/first track) | Start or replace playback, then continue through the playlist |
| `self.music.pause` | None | Retain track and position |
| `self.music.resume` | None | Resume retained position |
| `self.music.stop` | None | Cancel playback and reset position |
| `self.music.next` / `self.music.previous` | None | Same navigation as the buttons |
| `self.music.seek` | `seconds` (integer 0–86400) | Seek within duration; a paused track stays paused |
| `self.music.set_mode` | `repeat` (`off`, `all`, `one`), `shuffle` (boolean), both required | Save modes across restarts |
| `self.music.status` | None | State, filename, title/artist/album, position/duration in ms, zero-based library index, total, modes, source sample rate/average bitrate, busy/scanning/truncated flags and last error |

First listing and explicit refresh queue an asynchronous mount/scan. When
`scanning` is true, call list again with `refresh=false` until it is false;
the pending response contains an empty `files` array and `next_offset=0`.
Once ready, paginate with `next_offset` while it is below `total`. An empty
library is valid. Filename search is case-insensitive for ASCII; non-ASCII
characters match their UTF-8 bytes exactly. Listing/search order is sorted by
filename bytes, independently of playback shuffle order.

The cached library holds at most 512 tracks and scans at most 4096 directory
entries. `truncated=true` means some entries were omitted; the retained subset
is sorted after scanning. Stop playback and wait for `busy=false` before
refreshing after changing files on the card. Refresh also invalidates cached
metadata/indexes. A missing/unreadable card or bad file reports an error without
formatting the card. Play/resume/seek replies acknowledge a queued request;
check status for completion or errors.

With repeat `off`, playback stops at the end of the current playlist order;
`all` wraps the playlist and `one` repeats the current track. Shuffle builds
an order without duplicates and reshuffles on wrap, avoiding an immediate
repeat when multiple tracks exist. Enabling/disabling shuffle retains the
selected track. Preferences use new NVS namespace `box2_music`, keys `repeat`
and `shuffle`. Track and position are retained in RAM; playback does not
resume automatically after a restart.

### Playback and limits

Playback waits for two seconds without queued conversation audio, then ends
auto-listening and starts while the application is idle. Requests time out after
30 seconds if the device stays busy. Waking the assistant, starting chat, alerts,
notifications and incoming speech audio interrupt music and retain its position.
Resume explicitly with M or `self.music.resume` after the conversation. A small
queued/already-playing PCM chunk can cause a slight overlap or replay at pause.

MP3 decoding and SD reads run in dedicated tasks. MPEG-1/2/2.5 Layer III
mono/stereo 16-bit output at 8–48 kHz is downmixed to mono and resampled to the
codec's existing 24 kHz output. Free-format MP3 is unsupported. PCM uses the
audio service's bounded queue; the player never writes directly to I2S or
changes the codec sample rate. Buffers are allocated once per decode session,
and pausing releases the task and decoder memory. Music prevents idle sleep;
paused music allows normal battery sleep/shutdown.

The player scans frame headers to measure VBR duration and create a sparse seek
index. The first play can therefore spend time indexing a large file. Cached
metadata/indexes allow subsequent seeks and resumes without a full rescan.
Seeking decodes discarded preceding frames to restore the bit reservoir and
resampler. Position/duration follow encoded frames, including encoder padding;
playback is not gapless. Files are limited to 128 MiB, leading ID3 tags to 16 MiB,
and seek indexes to 4096 points (approximately 5.7 hours).

Title, artist and album are read from plain ID3v2.3/v2.4 text frames using
Latin-1, UTF-8 or UTF-16. Text is capped at 255 UTF-8 bytes; compressed/encrypted
or unsynchronised metadata is skipped. Untagged files use their filename. Font
glyph availability determines how non-ASCII text appears on the display.
Malformed, truncated or changing-format MP3 files stop with a reported error;
other files are not automatically skipped. Nested directories, cover art and
other audio formats are unsupported.

### Validation

Host tests cover filename boundaries/downmix, sorted playlists, repeat and
shuffle navigation, refresh selection, MP3 frame formats, VBR duration/seek
points, ID3 text encodings/footer and malformed/truncated/cancelled scans.
The index was also checked with generated mono/stereo MP3 at
8/16/24/44.1/48 kHz. These checks do not execute the ESP32 decoder or speaker.

Before release, verify SD mounting, decoding at those rates, metadata/display
layout, seek accuracy, repeat/shuffle, end of file, invalid files, absent cards,
rapid play/pause/seek/skip/stop, all button gestures and preference retention.
Also verify microphone capture, speech playback, wake/VAD interruption,
reconnection, battery sleep/shutdown and applicable AEC modes. A successful
firmware build does not validate these on physical hardware.

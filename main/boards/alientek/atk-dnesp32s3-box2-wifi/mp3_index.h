#pragma once

#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace box2_music {
struct Mp3Frame {
    uint32_t sample_rate;
    uint32_t bitrate;
    uint16_t bytes;
    uint16_t samples;
    uint8_t channels;
};
struct SeekPoint {
    uint32_t offset;
    uint32_t time_ms;
    uint64_t sample_offset = 0;
};
struct TrackInfo {
    std::string title;
    std::string artist;
    std::string album;
    uint32_t duration_ms = 0;
    uint32_t sample_rate = 0;
    uint32_t bitrate = 0;
    uint8_t channels = 0;
    std::vector<SeekPoint> points;
    SeekPoint SeekStart(uint32_t time_ms) const;
};
std::optional<Mp3Frame> ParseMp3Frame(const uint8_t* header);
std::expected<TrackInfo, std::string> InspectMp3(FILE* file, const std::function<bool()>& cancelled,
                                                 const std::function<void()>& yield);
}  // namespace box2_music

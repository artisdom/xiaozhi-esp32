#include "mp3_index.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace box2_music {
namespace {
constexpr uint32_t kMaxFileBytes = 128 * 1024 * 1024;
constexpr size_t kMaxSeekPoints = 4096;
uint32_t BigEndian(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
std::optional<uint32_t> Synchsafe(const uint8_t* p) {
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80)
        return std::nullopt;
    return (uint32_t(p[0]) << 21) | (uint32_t(p[1]) << 14) | (uint32_t(p[2]) << 7) | p[3];
}
void AppendUtf8(std::string& s, uint32_t c) {
    if (c < 32 || c == 127 || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
        return;
    std::string bytes;
    if (c < 0x80)
        bytes += char(c);
    else if (c < 0x800) {
        bytes += char(0xc0 | (c >> 6));
        bytes += char(0x80 | (c & 63));
    } else if (c < 0x10000) {
        bytes += char(0xe0 | (c >> 12));
        bytes += char(0x80 | ((c >> 6) & 63));
        bytes += char(0x80 | (c & 63));
    } else {
        bytes += char(0xf0 | (c >> 18));
        bytes += char(0x80 | ((c >> 12) & 63));
        bytes += char(0x80 | ((c >> 6) & 63));
        bytes += char(0x80 | (c & 63));
    }
    if (s.size() + bytes.size() <= 255)
        s += bytes;
}
std::string Text(const uint8_t* p, size_t size) {
    if (size < 2)
        return {};
    uint8_t encoding = *p++;
    --size;
    std::string out;
    if (encoding == 0) {
        for (size_t i = 0; i < size && p[i]; ++i)
            AppendUtf8(out, p[i]);
    } else if (encoding == 3) {
        for (size_t i = 0; i < size && p[i] && out.size() < 255;) {
            unsigned n =
                p[i] < 0x80
                    ? 1
                    : (p[i] >= 0xc2 && p[i] < 0xe0
                           ? 2
                           : (p[i] >= 0xe0 && p[i] < 0xf0 ? 3
                                                          : (p[i] >= 0xf0 && p[i] < 0xf5 ? 4 : 0)));
            if (!n || i + n > size || out.size() + n > 255)
                break;
            uint32_t c = p[i] & (n == 1 ? 127 : ((1u << (7 - n)) - 1));
            bool valid = true;
            for (unsigned j = 1; j < n; ++j) {
                if ((p[i + j] & 0xc0) != 0x80)
                    valid = false;
                c = (c << 6) | (p[i + j] & 63);
            }
            if (!valid || (n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000))
                break;
            AppendUtf8(out, c);
            i += n;
        }
    } else if (encoding == 1 || encoding == 2) {
        bool little = false;
        if (encoding == 1) {
            if (size < 2)
                return {};
            if (p[0] == 0xff && p[1] == 0xfe)
                little = true;
            else if (p[0] != 0xfe || p[1] != 0xff)
                return {};
            p += 2;
            size -= 2;
        }
        auto word = [little](const uint8_t* q) {
            return uint16_t(little ? q[0] | (q[1] << 8) : (q[0] << 8) | q[1]);
        };
        for (size_t i = 0; i + 1 < size;) {
            uint32_t c = word(p + i);
            i += 2;
            if (!c)
                break;
            if (c >= 0xd800 && c <= 0xdbff) {
                if (i + 1 >= size)
                    break;
                uint32_t low = word(p + i);
                i += 2;
                if (low < 0xdc00 || low > 0xdfff)
                    break;
                c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
            }
            AppendUtf8(out, c);
        }
    }
    return out;
}
std::expected<uint32_t, std::string> ReadId3(FILE* f, TrackInfo& info, uint32_t file_size,
                                             const std::function<bool()>& cancelled,
                                             const std::function<void()>& yield) {
    std::array<uint8_t, 10> h{};
    if (fread(h.data(), 1, h.size(), f) != h.size())
        return uint32_t(0);
    if (memcmp(h.data(), "ID3", 3) != 0)
        return uint32_t(0);
    auto length = Synchsafe(h.data() + 6);
    if (!length || *length > 16 * 1024 * 1024 || uint64_t(*length) + 10 > file_size)
        return std::unexpected("Invalid ID3 tag size");
    uint32_t end = 10 + *length;
    if (h[3] == 4 && (h[5] & 0x10)) {
        if (uint64_t(end) + 10 > file_size)
            return std::unexpected("Truncated ID3 footer");
        end += 10;
    }
    if ((h[3] != 3 && h[3] != 4) || (h[5] & 0x80))
        return end;
    const uint8_t version = h[3];
    uint32_t pos = 10;
    if (h[5] & 0x40) {
        uint8_t ext[4];
        if (fread(ext, 1, 4, f) != 4)
            return std::unexpected("Truncated ID3 extension");
        auto n = h[3] == 4 ? Synchsafe(ext) : std::optional<uint32_t>(BigEndian(ext));
        if (!n)
            return std::unexpected("Invalid ID3 extension");
        uint64_t next = uint64_t(pos) + *n + (h[3] == 3 ? 4 : 0);
        if (next > 10 + *length || next < 14)
            return std::unexpected("Invalid ID3 extension size");
        pos = next;
    }
    std::array<uint8_t, 4096> data{};
    unsigned frames = 0;
    while (pos + 10 <= 10 + *length) {
        if (cancelled())
            return std::unexpected("Cancelled");
        if (++frames % 128 == 0)
            yield();
        if (fseek(f, pos, SEEK_SET) != 0 || fread(h.data(), 1, 10, f) != 10)
            break;
        if (h[0] == 0)
            break;
        auto n = version == 4 ? Synchsafe(h.data() + 4)
                              : std::optional<uint32_t>(BigEndian(h.data() + 4));
        if (!n || uint64_t(pos) + 10 + *n > 10 + *length)
            break;
        std::string* target = nullptr;
        if (memcmp(h.data(), "TIT2", 4) == 0)
            target = &info.title;
        if (memcmp(h.data(), "TPE1", 4) == 0)
            target = &info.artist;
        if (memcmp(h.data(), "TALB", 4) == 0)
            target = &info.album;
        if (target && *n > 0 && *n <= data.size() && h[9] == 0) {
            if (fread(data.data(), 1, *n, f) != *n)
                break;
            *target = Text(data.data(), *n);
        }
        pos += 10 + *n;
    }
    return end;
}
}  // namespace

std::optional<Mp3Frame> ParseMp3Frame(const uint8_t* h) {
    if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0)
        return std::nullopt;
    unsigned version = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    unsigned br = h[2] >> 4, sr = (h[2] >> 2) & 3;
    if (version == 1 || layer != 1 || br == 0 || br == 15 || sr == 3 || (h[3] & 3) == 2)
        return std::nullopt;
    constexpr unsigned rates[] = {44100, 48000, 32000};
    constexpr unsigned v1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    constexpr unsigned v2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    uint32_t rate = rates[sr] / (version == 3 ? 1 : (version == 2 ? 2 : 4));
    uint32_t bitrate = (version == 3 ? v1[br] : v2[br]) * 1000;
    return Mp3Frame{rate, bitrate,
                    uint16_t((version == 3 ? 144 : 72) * bitrate / rate + ((h[2] >> 1) & 1)),
                    uint16_t(version == 3 ? 1152 : 576), uint8_t((h[3] >> 6) == 3 ? 1 : 2)};
}

SeekPoint TrackInfo::SeekStart(uint32_t time_ms) const {
    // Decode preceding frames to warm the MP3 bit reservoir and resampler.
    uint32_t target = time_ms > 1000 ? time_ms - 1000 : 0;
    SeekPoint point = points.empty() ? SeekPoint{} : points.front();
    for (const auto& p : points) {
        if (p.time_ms > target)
            break;
        point = p;
    }
    return point;
}

std::expected<TrackInfo, std::string> InspectMp3(FILE* file, const std::function<bool()>& cancelled,
                                                 const std::function<void()>& yield) {
    if (fseek(file, 0, SEEK_END) != 0)
        return std::unexpected("Cannot seek MP3 file");
    long length = ftell(file);
    if (length < 4 || uint64_t(length) > kMaxFileBytes)
        return std::unexpected("MP3 must be 4 bytes to 128 MiB");
    uint32_t size = length;
    rewind(file);
    TrackInfo info;
    auto start = ReadId3(file, info, size, cancelled, yield);
    if (!start)
        return std::unexpected(start.error());
    uint32_t pos = *start;
    uint64_t samples = 0, audio_bytes = 0;
    uint32_t frames = 0, junk = 0, next_point = 0;
    info.points.reserve(256);
    std::array<uint8_t, 4> h{};
    while (pos + 4 <= size) {
        if (cancelled())
            return std::unexpected("Cancelled");
        if (fseek(file, pos, SEEK_SET) != 0 || fread(h.data(), 1, 4, file) != 4)
            return std::unexpected("SD read failed while indexing MP3");
        if (frames && (memcmp(h.data(), "TAG", 3) == 0 || memcmp(h.data(), "APET", 4) == 0))
            break;
        auto frame = ParseMp3Frame(h.data());
        if (!frame) {
            if (frames || ++junk > 4096)
                return std::unexpected("Invalid MP3 frame sequence");
            ++pos;
            continue;
        }
        if (uint64_t(pos) + frame->bytes > size)
            return std::unexpected("Truncated MP3 frame");
        if (!frames) {
            info.sample_rate = frame->sample_rate;
            info.channels = frame->channels;
        } else if (info.sample_rate != frame->sample_rate || info.channels != frame->channels)
            return std::unexpected("MP3 format changed mid-stream");
        uint32_t ms = samples * 1000 / info.sample_rate;
        if (ms >= next_point) {
            if (info.points.size() == kMaxSeekPoints)
                return std::unexpected("MP3 exceeds seek index capacity");
            info.points.push_back({pos, ms, samples});
            next_point = ms + 5000;
        }
        samples += frame->samples;
        audio_bytes += frame->bytes;
        pos += frame->bytes;
        if ((++frames % 128) == 0)
            yield();
    }
    if (!frames)
        return std::unexpected("File contained no MP3 frames");
    info.duration_ms = samples * 1000 / info.sample_rate;
    info.bitrate = info.duration_ms ? audio_bytes * 8000 / info.duration_ms : 0;
    rewind(file);
    return info;
}
}  // namespace box2_music

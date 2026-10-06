#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace box2_music {

// Only a basename is accepted here. Paths and embedded NULs must never reach VFS.
inline bool IsMp3Filename(std::string_view name) {
    if (name.size() < 5 || name.size() > 255 || name.front() == '.' ||
        name.find_first_of("/\\") != std::string_view::npos ||
        name.find('\0') != std::string_view::npos) {
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    auto ext = name.substr(name.size() - 4);
    return ext[0] == '.' && (ext[1] == 'm' || ext[1] == 'M') && (ext[2] == 'p' || ext[2] == 'P') &&
           ext[3] == '3';
}

// A track is addressed by its path relative to the SD card root, e.g. "Rock/Song.mp3".
// Every component must be a valid basename, so ".." and hidden entries never reach VFS.
inline bool IsMp3RelativePath(std::string_view path) {
    if (path.size() > 255)
        return false;
    size_t start = 0;
    while (true) {
        auto end = path.find('/', start);
        auto part = path.substr(start, end == std::string_view::npos ? end : end - start);
        if (end == std::string_view::npos)
            return IsMp3Filename(part);
        if (part.empty() || part.front() == '.')
            return false;
        for (unsigned char c : part)
            if (c < 0x20 || c == 0x7f || c == '\\')
                return false;
        start = end + 1;
    }
}

// Short label for lists: basename without the .mp3 extension.
inline std::string TrackLabel(std::string_view path) {
    auto slash = path.rfind('/');
    if (slash != std::string_view::npos)
        path.remove_prefix(slash + 1);
    if (path.size() > 4)
        path.remove_suffix(4);
    return std::string(path);
}

inline int16_t StereoToMono(int16_t left, int16_t right) {
    return static_cast<int16_t>((static_cast<int32_t>(left) + right) / 2);
}

}  // namespace box2_music

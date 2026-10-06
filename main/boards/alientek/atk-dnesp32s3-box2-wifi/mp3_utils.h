#pragma once

#include <cstdint>
#include <string_view>

namespace box2_music {

// Only a basename in MUSIC is accepted. Paths and embedded NULs must never reach VFS.
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

inline int16_t StereoToMono(int16_t left, int16_t right) {
    return static_cast<int16_t>((static_cast<int32_t>(left) + right) / 2);
}

}  // namespace box2_music

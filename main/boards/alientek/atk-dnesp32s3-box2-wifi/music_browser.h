#pragma once

#include <set>
#include <string>
#include <vector>

#include "mp3_utils.h"

namespace box2_music {

// One row of the folder browser: the ".." row, a sub-folder or an MP3 file.
struct BrowseEntry {
    enum class Kind { Up, Folder, File };
    Kind kind = Kind::File;
    std::string name;  // Label shown on screen.
    std::string path;  // Folder or file path relative to the card root ("" for the root).
};

inline std::string ParentDirectory(const std::string& dir) {
    auto slash = dir.rfind('/');
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

// Immediate children of `dir` derived from the flat sorted track list: ".." (unless at the
// root), then sub-folders, then files.
inline std::vector<BrowseEntry> ListDirectory(const std::vector<std::string>& tracks,
                                              const std::string& dir) {
    const std::string prefix = dir.empty() ? std::string() : dir + "/";
    std::set<std::string> folders;
    std::vector<BrowseEntry> files;
    for (const auto& track : tracks) {
        if (track.compare(0, prefix.size(), prefix) != 0)
            continue;
        auto rest = track.substr(prefix.size());
        auto slash = rest.find('/');
        if (slash != std::string::npos)
            folders.insert(rest.substr(0, slash));
        else
            files.push_back({BrowseEntry::Kind::File, TrackLabel(rest), track});
    }
    std::vector<BrowseEntry> entries;
    if (!dir.empty())
        entries.push_back({BrowseEntry::Kind::Up, "..", ParentDirectory(dir)});
    for (const auto& folder : folders)
        entries.push_back({BrowseEntry::Kind::Folder, folder, prefix + folder});
    for (auto& file : files)
        entries.push_back(std::move(file));
    return entries;
}

}  // namespace box2_music

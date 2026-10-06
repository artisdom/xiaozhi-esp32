#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string>
#include <vector>

namespace box2_music {
enum class Repeat { Off, All, One };
class Playlist {
public:
    static constexpr size_t kCapacity = 512;
    void SetTracks(std::vector<std::string> tracks) {
        std::string selected = Current();
        if (tracks.size() > kCapacity)
            tracks.resize(kCapacity);
        std::sort(tracks.begin(), tracks.end());
        tracks.erase(std::unique(tracks.begin(), tracks.end()), tracks.end());
        tracks_ = std::move(tracks);
        Reorder();
        Select(selected);
    }
    const std::vector<std::string>& Tracks() const { return tracks_; }
    std::string Current() const {
        return order_.empty() ? std::string() : tracks_[order_[cursor_]];
    }
    size_t Index() const { return order_.empty() ? 0 : order_[cursor_]; }
    bool Select(const std::string& name) {
        auto it = std::lower_bound(tracks_.begin(), tracks_.end(), name);
        if (it == tracks_.end() || *it != name)
            return false;
        auto index = size_t(it - tracks_.begin());
        cursor_ = std::find(order_.begin(), order_.end(), index) - order_.begin();
        return true;
    }
    void SetRepeat(Repeat repeat) { repeat_ = repeat; }
    Repeat GetRepeat() const { return repeat_; }
    bool Shuffle() const { return shuffle_; }
    void SetShuffle(bool enabled, unsigned seed) {
        std::string selected = Current();
        shuffle_ = enabled;
        random_ = seed ? seed : 1;
        Reorder();
        Select(selected);
    }
    bool Advance(int direction, bool manual) {
        if (order_.empty())
            return false;
        if (!manual && repeat_ == Repeat::One)
            return true;
        if (direction < 0) {
            if (cursor_ == 0) {
                if (!manual && repeat_ == Repeat::Off)
                    return false;
                cursor_ = order_.size() - 1;
            } else
                --cursor_;
        } else if (cursor_ + 1 < order_.size())
            ++cursor_;
        else {
            if (!manual && repeat_ == Repeat::Off)
                return false;
            std::string last = Current();
            Reorder();
            if (shuffle_ && order_.size() > 1 && Current() == last)
                std::swap(order_[0], order_[1]);
        }
        return true;
    }

private:
    std::vector<std::string> tracks_;
    std::vector<size_t> order_;
    size_t cursor_ = 0;
    Repeat repeat_ = Repeat::Off;
    bool shuffle_ = false;
    unsigned random_ = 1;
    void Reorder() {
        order_.resize(tracks_.size());
        std::iota(order_.begin(), order_.end(), 0);
        cursor_ = 0;
        if (shuffle_)
            for (size_t i = order_.size(); i > 1; --i) {
                random_ ^= random_ << 13;
                random_ ^= random_ >> 17;
                random_ ^= random_ << 5;
                std::swap(order_[i - 1], order_[random_ % i]);
            }
    }
};
inline const char* RepeatName(Repeat repeat) {
    return repeat == Repeat::All ? "all" : (repeat == Repeat::One ? "one" : "off");
}
}  // namespace box2_music

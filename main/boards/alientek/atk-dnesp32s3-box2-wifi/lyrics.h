#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace box2_music {

// A word (English) or a single character (Chinese/Japanese) of a lyric line, with the time
// span in which it is sung.
struct LyricWord {
    std::string text;
    uint32_t start_ms = 0, end_ms = 0;
    bool space_after = false;  // Whitespace followed this word in the source.
};

struct LyricLine {
    uint32_t start_ms = 0;
    uint32_t end_ms = 0;  // Start of the next line (or an instrumental marker).
    std::vector<LyricWord> words;

    // Index of the word being sung at `ms`: -1 before the first word, otherwise the last word
    // that has started (so after the line's last word it stays on that word).
    int WordAt(uint32_t ms) const {
        int index = -1;
        for (size_t i = 0; i < words.size() && words[i].start_ms <= ms; ++i)
            index = int(i);
        return index;
    }
};

struct Lyrics {
    std::vector<LyricLine> lines;

    // Index of the last line that has started at `ms`, or -1 before the first line.
    int LineAt(uint32_t ms) const {
        auto it = std::upper_bound(lines.begin(), lines.end(), ms,
                                   [](uint32_t t, const LyricLine& l) { return t < l.start_ms; });
        return int(it - lines.begin()) - 1;
    }
};

namespace lyrics_detail {

constexpr size_t kMaxLines = 400, kMaxWordsPerLine = 64, kMaxLineBytes = 512;

// Decode one UTF-8 code point; invalid bytes decode as themselves with length 1.
inline uint32_t Decode(std::string_view s, size_t i, size_t& length) {
    unsigned char c = s[i];
    length = 1;
    if (c < 0x80)
        return c;
    size_t need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (!need || c >= 0xF8 || i + need >= s.size())
        return c;
    uint32_t cp = c & (0x3F >> need);
    for (size_t k = 1; k <= need; ++k) {
        unsigned char next = s[i + k];
        if ((next & 0xC0) != 0x80)
            return c;
        cp = cp << 6 | (next & 0x3F);
    }
    length = need + 1;
    return cp;
}

// Characters that are sung one per syllable and learned one by one.
inline bool IsIdeograph(uint32_t cp) {
    return (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
           (cp >= 0x20000 && cp <= 0x2FFFF);
}
// CJK and full-width punctuation sticks to the character before it.
inline bool IsCjkPunctuation(uint32_t cp) {
    return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF00 && cp <= 0xFFEF) ||
           (cp >= 0x2018 && cp <= 0x201F) || cp == 0x2026 || cp == 0x2014;
}
inline bool IsSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Split a lyric text into words: each ideograph is a word, other text splits at whitespace.
inline std::vector<LyricWord> Tokenize(std::string_view text) {
    std::vector<LyricWord> words;
    bool in_latin = false;
    size_t i = 0;
    while (i < text.size()) {
        if (IsSpace(text[i])) {
            if (!words.empty())
                words.back().space_after = true;
            in_latin = false;
            ++i;
            continue;
        }
        size_t length;
        uint32_t cp = Decode(text, i, length);
        auto chunk = text.substr(i, length);
        i += length;
        if (words.size() >= kMaxWordsPerLine) {  // Absurdly long line: glue the rest together.
            words.back().text.append(chunk);
            continue;
        }
        if (IsIdeograph(cp)) {
            words.push_back({std::string(chunk)});
            in_latin = false;
        } else if (IsCjkPunctuation(cp) && !words.empty() && !words.back().space_after) {
            words.back().text.append(chunk);
        } else if (in_latin && !words.empty()) {
            words.back().text.append(chunk);
        } else {
            words.push_back({std::string(chunk)});
            in_latin = !IsCjkPunctuation(cp);
        }
    }
    return words;
}

// Natural singing time of a word, used to spread a line over its words.
inline float NaturalSeconds(const LyricWord& word) {
    size_t cp_count = 0;
    bool ideograph = false;
    for (size_t i = 0; i < word.text.size();) {
        size_t length;
        uint32_t cp = Decode(word.text, i, length);
        i += length;
        ++cp_count;
        ideograph = ideograph || IsIdeograph(cp);
    }
    return ideograph ? 0.40f : 0.20f + 0.06f * float(cp_count);
}

// Parse "mm:ss", "mm:ss.xx", "mm:ss.xxx" or "mm:ss:xx"; returns false for metadata tags.
inline bool ParseTimestamp(std::string_view tag, int64_t& ms) {
    size_t colon = tag.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 3)
        return false;
    int64_t minutes = 0, seconds = 0, fraction = 0;
    for (size_t i = 0; i < colon; ++i) {
        if (tag[i] < '0' || tag[i] > '9')
            return false;
        minutes = minutes * 10 + (tag[i] - '0');
    }
    size_t i = colon + 1, digits = 0;
    for (; i < tag.size() && tag[i] >= '0' && tag[i] <= '9' && digits < 2; ++i, ++digits)
        seconds = seconds * 10 + (tag[i] - '0');
    if (!digits || seconds > 59)
        return false;
    if (i < tag.size()) {
        if (tag[i] != '.' && tag[i] != ':')
            return false;
        ++i;
        size_t frac_digits = 0;
        for (; i < tag.size(); ++i, ++frac_digits) {
            if (tag[i] < '0' || tag[i] > '9')
                return false;
            if (frac_digits < 3)
                fraction = fraction * 10 + (tag[i] - '0');
        }
        if (!frac_digits)
            return false;
        for (size_t d = frac_digits; d < 3; ++d)
            fraction *= 10;
    }
    ms = (minutes * 60 + seconds) * 1000 + fraction;
    return true;
}

struct RawLine {
    uint32_t start_ms;
    std::string text;
    size_t order;
};

}  // namespace lyrics_detail

// Parse an LRC file ("[mm:ss.xx]text", several timestamps per line allowed, optional
// "[offset:ms]" and enhanced "<mm:ss.xx>word" timing). Returns nothing when there are no
// timed lines. Words without explicit timing are timed by spreading the line over them.
inline std::optional<Lyrics> ParseLrc(std::string_view data) {
    using namespace lyrics_detail;
    if (data.size() >= 3 && data.substr(0, 3) == "\xEF\xBB\xBF")
        data.remove_prefix(3);
    std::vector<RawLine> raw;
    int64_t offset_ms = 0;
    size_t pos = 0;
    while (pos < data.size() && raw.size() < kMaxLines * 2) {
        size_t eol = data.find('\n', pos);
        std::string_view line = data.substr(pos, eol == std::string_view::npos ? eol : eol - pos);
        pos = eol == std::string_view::npos ? data.size() : eol + 1;
        std::vector<int64_t> stamps;
        size_t i = 0;
        while (i < line.size() && line[i] == '[') {
            size_t close = line.find(']', i);
            if (close == std::string_view::npos)
                break;
            auto tag = line.substr(i + 1, close - i - 1);
            int64_t ms;
            if (ParseTimestamp(tag, ms)) {
                stamps.push_back(ms);
            } else if (tag.substr(0, 7) == "offset:") {
                offset_ms = std::strtoll(std::string(tag.substr(7)).c_str(), nullptr, 10);
            }
            i = close + 1;
        }
        if (stamps.empty())
            continue;
        std::string text(line.substr(i));
        while (!text.empty() && IsSpace(text.back()))
            text.pop_back();
        if (text.size() > kMaxLineBytes)
            text.resize(kMaxLineBytes);
        for (int64_t ms : stamps)
            raw.push_back({uint32_t(std::max<int64_t>(0, ms - offset_ms)), text, raw.size()});
    }
    std::stable_sort(raw.begin(), raw.end(), [](const RawLine& a, const RawLine& b) {
        return a.start_ms < b.start_ms;
    });

    Lyrics lyrics;
    for (size_t r = 0; r < raw.size() && lyrics.lines.size() < kMaxLines; ++r) {
        if (!lyrics.lines.empty() && lyrics.lines.back().end_ms == 0)
            lyrics.lines.back().end_ms = raw[r].start_ms;  // Previous line ends here.
        // Enhanced word timing: "<mm:ss.xx>word" pieces.
        std::vector<LyricWord> words;
        bool enhanced = false;
        std::string_view text = raw[r].text;
        if (text.find('<') != std::string_view::npos) {
            size_t i = 0;
            int64_t pending = -1;
            std::string current;
            auto flush = [&]() {
                if (pending >= 0 && !current.empty()) {
                    for (auto& w : Tokenize(current)) {
                        w.start_ms = uint32_t(std::max<int64_t>(0, pending - offset_ms));
                        words.push_back(std::move(w));
                    }
                }
                current.clear();
            };
            while (i < text.size()) {
                size_t close;
                int64_t ms;
                if (text[i] == '<' && (close = text.find('>', i)) != std::string_view::npos &&
                    ParseTimestamp(text.substr(i + 1, close - i - 1), ms)) {
                    flush();
                    pending = ms;
                    enhanced = true;
                    i = close + 1;
                } else {
                    current.push_back(text[i++]);
                }
            }
            flush();
        }
        if (!enhanced)
            words = Tokenize(text);
        if (words.empty()) {
            if (!lyrics.lines.empty() && lyrics.lines.back().end_ms == 0)
                lyrics.lines.back().end_ms = raw[r].start_ms;
            continue;  // Instrumental marker: only ends the previous line.
        }
        LyricLine line;
        line.start_ms = raw[r].start_ms;
        line.words = std::move(words);
        if (enhanced) {
            for (size_t w = 0; w < line.words.size(); ++w)
                line.words[w].start_ms = std::max(line.words[w].start_ms, line.start_ms);
        }
        lyrics.lines.push_back(std::move(line));
    }
    if (lyrics.lines.empty())
        return std::nullopt;

    // Fill in line ends and the timing of words without explicit stamps.
    for (size_t l = 0; l < lyrics.lines.size(); ++l) {
        LyricLine& line = lyrics.lines[l];
        uint32_t next = l + 1 < lyrics.lines.size() ? lyrics.lines[l + 1].start_ms : 0;
        if (line.end_ms == 0 || line.end_ms < line.start_ms)
            line.end_ms = next ? next : line.start_ms + 5000;
        bool timed = line.words.size() > 1 && line.words[1].start_ms > 0;
        const uint32_t span = line.end_ms - line.start_ms;
        if (timed) {
            for (size_t w = 0; w < line.words.size(); ++w)
                line.words[w].end_ms =
                    w + 1 < line.words.size() ? line.words[w + 1].start_ms : line.end_ms;
            continue;
        }
        float total = 0;
        for (const auto& w : line.words)
            total += NaturalSeconds(w);
        // A line is sung for at most 1.6x its natural length: long gaps are instrumental.
        float sing = std::min(float(span) / 1000.0f, total * 1.6f);
        float cursor = 0;
        for (auto& w : line.words) {
            float share = NaturalSeconds(w) / total * sing;
            w.start_ms = line.start_ms + uint32_t(cursor * 1000.0f);
            cursor += share;
            w.end_ms = line.start_ms + uint32_t(cursor * 1000.0f);
        }
    }
    return lyrics;
}

}  // namespace box2_music

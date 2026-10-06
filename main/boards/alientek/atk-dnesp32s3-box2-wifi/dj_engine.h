#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace box2_music {

// Real-time DJ processing for the mono PCM stream: one selectable effect, four
// synthesised sampler pads mixed over the song, and analysis of the final output
// (12-band spectrum, waveform envelope, beat/BPM) for the DJ screen.
//
// Process() runs on the MP3 task. Controls are atomics and GetSnapshot() takes a
// short lock, so the UI task never blocks the audio path for long.
class DjEngine {
public:
    static constexpr int kBands = 12;
    static constexpr int kWave = 48;
    enum Fx { kFxOff, kFxLowpass, kFxHighpass, kFxEcho, kFxCrush, kFxGate, kFxCount };
    enum Pad { kPadKick, kPadSnare, kPadHat, kPadHorn, kPadCount };

    struct Snapshot {
        uint8_t bands[kBands] = {};
        uint8_t wave[kWave] = {};  // Oldest first, 0..100.
        uint32_t frames = 0;       // Increments with every processed frame.
        uint32_t beats = 0;        // Increments on every detected beat.
        int bpm = 0;               // 0 until enough beats were seen.
    };

    static const char* FxName(int fx) {
        static const char* const names[] = {"OFF", "LPF", "HPF", "ECHO", "CRUSH", "GATE"};
        return names[std::clamp(fx, 0, int(kFxCount) - 1)];
    }
    static const char* PadName(int pad) {
        static const char* const names[] = {"KICK", "SNARE", "HAT", "HORN"};
        return names[std::clamp(pad, 0, int(kPadCount) - 1)];
    }

    ~DjEngine() { FreeEcho(); }

    int GetFx() const { return fx_.load(); }
    void SetFx(int fx) { fx_.store(std::clamp(fx, 0, int(kFxCount) - 1)); }
    int NextFx() {
        int next = (fx_.load() + 1) % kFxCount;
        fx_.store(next);
        return next;
    }
    void TriggerPad(int pad) {
        if (pad >= 0 && pad < kPadCount)
            pending_pads_.fetch_or(1u << pad);
    }

    // Forget analysis history (call when a new track starts).
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = Snapshot{};
        wave_ring_.fill(0);
        wave_pos_ = 0;
        beat_average_ = 0;
        last_beat_ms_ = -1000;
        interval_count_ = 0;
    }

    Snapshot GetSnapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    // Apply the effect and sampler to `pcm` in place, then analyse the result.
    void Process(int16_t* pcm, size_t count, int rate) {
        if (!count || rate < 8000)
            return;
        const float fs = float(rate);
        const int fx = fx_.load();
        if (fx != active_fx_) {
            active_fx_ = fx;
            lp_[0] = lp_[1] = 0;
            gate_gain_ = 1;
            hold_ = 0;
            hold_count_ = 0;
            if (fx == kFxEcho)
                AllocateEcho();
        }
        StartPendingPads(fs);
        ApplyEffectAndPads(pcm, count, fs, fx);
        Analyse(pcm, count, rate);
    }

private:
    struct Voice {
        bool active = false;
        uint32_t n = 0;
        float phase = 0, phase2 = 0, env = 0, env2 = 0, freq = 0, prev = 0;
    };
    static constexpr float kTwoPi = 6.2831853f;
    static constexpr int kMaxEchoSamples = 8000;  // 280 ms at up to 28.5 kHz.

    std::atomic<int> fx_{kFxOff};
    std::atomic<uint32_t> pending_pads_{0};
    std::mutex mutex_;
    Snapshot snapshot_;

    // Effect state (MP3 task only).
    int active_fx_ = kFxOff;
    float lp_[2] = {0, 0};
    float gate_gain_ = 1;
    uint32_t gate_pos_ = 0;
    float hold_ = 0;
    int hold_count_ = 0;
    int16_t* echo_ = nullptr;
    int echo_len_ = 0, echo_pos_ = 0;

    // Sampler state.
    std::array<Voice, kPadCount> voices_;
    uint32_t noise_ = 0x12345678;

    // Analysis state.
    std::array<uint8_t, kWave> wave_ring_ = {};
    int wave_pos_ = 0;
    uint64_t samples_total_ = 0;
    float beat_average_ = 0;
    int64_t last_beat_ms_ = -1000;
    std::array<int, 8> intervals_ = {};
    int interval_count_ = 0;

    void AllocateEcho() {
        if (echo_)
            return;
#ifdef ESP_PLATFORM
        echo_ = static_cast<int16_t*>(
            heap_caps_calloc(kMaxEchoSamples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
        echo_ = static_cast<int16_t*>(std::calloc(kMaxEchoSamples, sizeof(int16_t)));
#endif
        echo_pos_ = 0;
    }
    void FreeEcho() {
        if (!echo_)
            return;
#ifdef ESP_PLATFORM
        heap_caps_free(echo_);
#else
        std::free(echo_);
#endif
        echo_ = nullptr;
    }

    float Noise() {
        noise_ ^= noise_ << 13;
        noise_ ^= noise_ >> 17;
        noise_ ^= noise_ << 5;
        return float(int32_t(noise_)) * (1.0f / 2147483648.0f);
    }

    void StartPendingPads(float) {
        uint32_t mask = pending_pads_.exchange(0);
        for (int pad = 0; pad < kPadCount; ++pad) {
            if (!(mask & (1u << pad)))
                continue;
            Voice& v = voices_[pad];
            v = Voice{};
            v.active = true;
            v.env = v.env2 = 1;
            v.freq = pad == kPadKick ? 120.0f : 0.0f;  // Kick: pitch drop above 45 Hz.
        }
    }

    // One sample of a pad voice in -1..1; deactivates it when finished.
    float VoiceSample(int pad, Voice& v, float fs, float k_fast, float k_slow) {
        float out = 0;
        const float t = float(v.n) / fs;
        switch (pad) {
        case kPadKick:
            out = std::sin(v.phase) * v.env;
            v.phase += kTwoPi * (45.0f + v.freq) / fs;
            v.freq *= k_fast;  // Pitch sweep.
            v.env *= k_slow;
            if (t > 0.4f)
                v.active = false;
            break;
        case kPadSnare:
            out = Noise() * v.env * 0.55f + std::sin(v.phase) * v.env2 * 0.5f;
            v.phase += kTwoPi * 190.0f / fs;
            v.env *= k_fast;
            v.env2 *= k_slow;
            if (t > 0.3f)
                v.active = false;
            break;
        case kPadHat: {
            float n = Noise();
            out = (n - v.prev) * 0.5f * v.env;  // Differentiated noise: bright.
            v.prev = n;
            v.env *= k_fast;
            if (t > 0.12f)
                v.active = false;
            break;
        }
        case kPadHorn: {
            float gain = t < 0.01f ? t / 0.01f : v.env;
            if (t > 0.55f)
                v.env *= k_fast;
            out = ((2 * v.phase - 1) + (2 * v.phase2 - 1)) * 0.25f * gain;
            v.phase += 440.0f / fs;
            v.phase2 += 554.0f / fs;
            v.phase -= std::floor(v.phase);
            v.phase2 -= std::floor(v.phase2);
            if (t > 0.8f)
                v.active = false;
            break;
        }
        }
        ++v.n;
        return out;
    }

    void ApplyEffectAndPads(int16_t* pcm, size_t count, float fs, int fx) {
        // Per-sample decay factors, computed once per frame.
        const float kick_fast = std::exp(-30.0f / fs), kick_slow = std::exp(-9.0f / fs);
        const float snare_fast = std::exp(-16.0f / fs), snare_slow = std::exp(-28.0f / fs);
        const float hat_fast = std::exp(-55.0f / fs);
        const float horn_fast = std::exp(-12.0f / fs);
        const float lp_a = 1.0f - std::exp(-kTwoPi * 600.0f / fs);
        const float hp_a = 1.0f - std::exp(-kTwoPi * 1200.0f / fs);
        const int echo_len = std::min(kMaxEchoSamples, int(fs * 0.28f));
        const uint32_t gate_period = uint32_t(fs / 8.0f);
        const float gate_step = 1.0f / (0.004f * fs);
        for (size_t i = 0; i < count; ++i) {
            float x = pcm[i], y = x;
            switch (fx) {
            case kFxLowpass:
                lp_[0] += lp_a * (x - lp_[0]);
                lp_[1] += lp_a * (lp_[0] - lp_[1]);
                y = lp_[1];
                break;
            case kFxHighpass:
                lp_[0] += hp_a * (x - lp_[0]);
                y = x - lp_[0];
                break;
            case kFxEcho:
                if (echo_) {
                    if (echo_len_ != echo_len) {  // Sample rate changed: restart the line.
                        echo_len_ = echo_len;
                        std::memset(echo_, 0, kMaxEchoSamples * sizeof(int16_t));
                        echo_pos_ = 0;
                    }
                    float d = echo_[echo_pos_];
                    y = x + 0.5f * d;
                    echo_[echo_pos_] = int16_t(std::clamp(x + 0.45f * d, -32768.0f, 32767.0f));
                    if (++echo_pos_ >= echo_len_)
                        echo_pos_ = 0;
                }
                break;
            case kFxCrush:
                if (hold_count_ == 0)
                    hold_ = float(int(x) & 0xF800);  // 5 bits, held for 4 samples.
                hold_count_ = (hold_count_ + 1) % 4;
                y = hold_;
                break;
            case kFxGate: {
                float target = (gate_pos_++ % gate_period) < gate_period / 2 ? 1.0f : 0.0f;
                gate_gain_ += std::clamp(target - gate_gain_, -gate_step, gate_step);
                y = x * gate_gain_;
                break;
            }
            default:
                break;
            }
            for (int pad = 0; pad < kPadCount; ++pad) {
                Voice& v = voices_[pad];
                if (!v.active)
                    continue;
                float fast = pad == kPadKick    ? kick_fast
                             : pad == kPadSnare ? snare_fast
                             : pad == kPadHat   ? hat_fast
                                                : horn_fast;
                float slow = pad == kPadKick ? kick_slow : snare_slow;
                y += VoiceSample(pad, v, fs, fast, slow) * 24000.0f;
            }
            pcm[i] = int16_t(std::clamp(y, -32768.0f, 32767.0f));
        }
    }

    void Analyse(const int16_t* pcm, size_t count, int rate) {
        static constexpr float kFreqs[kBands] = {60,   120,  200,  350,  550,  850,
                                                 1300, 2000, 3000, 4500, 7000, 10000};
        const int n = int(std::min<size_t>(count, 768));
        int peak = 0;
        for (size_t i = 0; i < count; ++i)
            peak = std::max(peak, std::abs(int(pcm[i])));
        uint8_t bands[kBands] = {};
        for (int b = 0; b < kBands; ++b) {
            if (kFreqs[b] >= rate * 0.45f)
                continue;
            const float coeff = 2.0f * std::cos(kTwoPi * kFreqs[b] / float(rate));
            float s1 = 0, s2 = 0;
            for (int i = 0; i < n; ++i) {
                float s = pcm[i] + coeff * s1 - s2;
                s2 = s1;
                s1 = s;
            }
            float power = std::max(0.0f, s1 * s1 + s2 * s2 - coeff * s1 * s2);
            float amplitude = 2.0f * std::sqrt(power) / float(n);
            float db = 20.0f * std::log10(amplitude / 32768.0f + 1e-9f);
            bands[b] = uint8_t(std::clamp((db + 75.0f) * (100.0f / 55.0f), 0.0f, 100.0f));
        }
        samples_total_ += count;
        const int64_t now_ms = int64_t(samples_total_ * 1000 / uint64_t(rate));

        // Beat: the low bands jump above their recent average.
        float low = std::max({bands[0], bands[1], bands[2]});
        bool beat = low > beat_average_ + 12.0f && low > 35.0f && now_ms - last_beat_ms_ > 280;
        beat_average_ = beat_average_ * 0.94f + low * 0.06f;

        std::lock_guard<std::mutex> lock(mutex_);
        if (beat) {
            int64_t interval = now_ms - last_beat_ms_;
            last_beat_ms_ = now_ms;
            ++snapshot_.beats;
            if (interval < 1500) {
                intervals_[interval_count_ % intervals_.size()] = int(interval);
                ++interval_count_;
            }
            if (interval_count_ >= 4) {
                std::array<int, 8> sorted = intervals_;
                int used = std::min<int>(interval_count_, sorted.size());
                std::sort(sorted.begin(), sorted.begin() + used);
                float bpm = 60000.0f / float(sorted[used / 2]);
                while (bpm < 75.0f)
                    bpm *= 2;
                while (bpm > 180.0f)
                    bpm /= 2;
                snapshot_.bpm = int(bpm + 0.5f);
            }
        }
        // sqrt companding makes quiet passages visible.
        wave_ring_[wave_pos_] = uint8_t(std::sqrt(float(peak) / 32768.0f) * 100.0f);
        wave_pos_ = (wave_pos_ + 1) % kWave;
        for (int i = 0; i < kWave; ++i)
            snapshot_.wave[i] = wave_ring_[(wave_pos_ + i) % kWave];
        std::memcpy(snapshot_.bands, bands, sizeof(bands));
        ++snapshot_.frames;
    }
};

}  // namespace box2_music

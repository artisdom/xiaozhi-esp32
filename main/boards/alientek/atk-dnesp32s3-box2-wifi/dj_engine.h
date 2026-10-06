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
    enum Fx {
        kFxOff,
        kFxLowpass,
        kFxHighpass,
        kFxEcho,
        kFxReverb,
        kFxGate,
        kFxFlanger,
        kFxWobble,
        kFxRobot,
        kFxCount
    };
    enum Pad {
        kPadKick,
        kPadSnare,
        kPadHat,
        kPadClap,
        kPadHorn,
        kPadLaser,
        kPadBass,
        kPadSiren,
        kPadCount
    };

    struct Snapshot {
        uint8_t bands[kBands] = {};
        uint8_t wave[kWave] = {};  // Oldest first, 0..100.
        uint32_t frames = 0;       // Increments with every processed frame.
        uint32_t beats = 0;        // Increments on every detected beat.
        int bpm = 0;               // 0 until enough beats were seen.
    };

    static const char* FxName(int fx) {
        static const char* const names[] = {"OFF", "LPF", "HPF", "ECHO", "VERB", "GATE", "FLNG", "WOB", "ROBO"};
        return names[std::clamp(fx, 0, int(kFxCount) - 1)];
    }
    static const char* PadName(int pad) {
        static const char* const names[] = {"KICK", "SNARE", "HAT", "CLAP", "HORN", "LASER", "BASS", "SIREN"};
        return names[std::clamp(pad, 0, int(kPadCount) - 1)];
    }

    ~DjEngine() { FreeFxMemory(); }

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
            svf_low_ = svf_band_ = 0;
            flanger_.fill(0);
            flanger_pos_ = 0;
            lfo_phase_ = carrier_phase_ = 0;
            echo_len_ = 0;       // Force the shared delay memory to be cleared for echo...
            reverb_len_[0] = 0;  // ... and re-laid out for reverb.
            if (fx == kFxEcho || fx == kFxReverb)
                AllocateFxMemory();
            else
                FreeFxMemory();
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
    static constexpr int kMaxFxSamples = 8000;  // Echo: 280 ms; reverb: about 6000 at 48 kHz.
    static constexpr int kFlangerSamples = 512;

    std::atomic<int> fx_{kFxOff};
    std::atomic<uint32_t> pending_pads_{0};
    std::mutex mutex_;
    Snapshot snapshot_;

    // Effect state (MP3 task only).
    int active_fx_ = kFxOff;
    float lp_[2] = {0, 0};
    float gate_gain_ = 1;
    uint32_t gate_pos_ = 0;
    float svf_low_ = 0, svf_band_ = 0;  // Wobble filter.
    float lfo_phase_ = 0, carrier_phase_ = 0;
    std::array<float, kFlangerSamples> flanger_ = {};
    int flanger_pos_ = 0;
    // One PSRAM block shared by echo and reverb (only one effect is active at a time).
    int16_t* fx_mem_ = nullptr;
    int echo_len_ = 0, echo_pos_ = 0;
    // Reverb layout inside fx_mem_: four combs then two all-passes.
    static constexpr int kCombs = 4, kAllpasses = 2;
    int reverb_len_[kCombs + kAllpasses] = {};
    int reverb_off_[kCombs + kAllpasses] = {};
    int reverb_pos_[kCombs + kAllpasses] = {};
    float comb_damp_[kCombs] = {};

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

    void AllocateFxMemory() {
        if (fx_mem_)
            return;
#ifdef ESP_PLATFORM
        fx_mem_ = static_cast<int16_t*>(
            heap_caps_calloc(kMaxFxSamples, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
        fx_mem_ = static_cast<int16_t*>(std::calloc(kMaxFxSamples, sizeof(int16_t)));
#endif
        echo_pos_ = 0;
        echo_len_ = 0;
    }
    void FreeFxMemory() {
        if (!fx_mem_)
            return;
#ifdef ESP_PLATFORM
        heap_caps_free(fx_mem_);
#else
        std::free(fx_mem_);
#endif
        fx_mem_ = nullptr;
    }

    // Lay out the reverb delay lines for this sample rate and clear them.
    void SetupReverb(float fs) {
        static constexpr int kBase[kCombs + kAllpasses] = {1116, 1188, 1277, 1356, 556, 441};
        int offset = 0;
        for (int i = 0; i < kCombs + kAllpasses; ++i) {
            reverb_len_[i] = std::max(8, int(kBase[i] * fs / 44100.0f));
            reverb_off_[i] = offset;
            reverb_pos_[i] = 0;
            offset += reverb_len_[i];
        }
        if (offset > kMaxFxSamples) {  // Absurdly high rate: shrink to fit.
            for (int i = 0; i < kCombs + kAllpasses; ++i)
                reverb_len_[i] = std::max(8, reverb_len_[i] * kMaxFxSamples / offset);
            offset = 0;
            for (int i = 0; i < kCombs + kAllpasses; ++i) {
                reverb_off_[i] = offset;
                offset += reverb_len_[i];
            }
        }
        std::memset(fx_mem_, 0, kMaxFxSamples * sizeof(int16_t));
        for (float& d : comb_damp_)
            d = 0;
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
            // Initial pitch offsets of the sweeping pads.
            v.freq = pad == kPadKick ? 120.0f : pad == kPadLaser ? 1800.0f : pad == kPadBass ? 50.0f : 0.0f;
        }
    }

    // Decay rates (1/s) per pad: {fast, slow}; meaning depends on the pad, see VoiceSample().
    static constexpr float kRates[kPadCount][2] = {{30, 9}, {16, 28}, {55, 0}, {25, 0},
                                                   {12, 0}, {10, 5},  {20, 5}, {0, 3}};

    // One sample of a pad voice in about -1..1; deactivates it when finished.
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
        case kPadClap:
            // Three quick noise bursts followed by a short tail.
            if (v.n == 0 || v.n == uint32_t(0.012f * fs) || v.n == uint32_t(0.024f * fs))
                v.env = 1;
            out = Noise() * v.env * 0.7f;
            v.env *= k_fast;
            if (t > 0.22f)
                v.active = false;
            break;
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
        case kPadLaser:
            // Square wave sweeping down from about 2 kHz.
            out = (std::sin(v.phase) >= 0 ? 0.3f : -0.3f) * v.env;
            v.phase += kTwoPi * (200.0f + v.freq) / fs;
            v.freq *= k_fast;
            v.env *= k_slow;
            if (t > 0.35f)
                v.active = false;
            break;
        case kPadBass:
            // 808-style sub drop with a little second harmonic.
            out = (std::sin(v.phase) + 0.3f * std::sin(2 * v.phase)) * 0.8f * v.env;
            v.phase += kTwoPi * (55.0f + v.freq) / fs;
            v.freq *= k_fast;
            v.env *= k_slow;
            if (t > 0.7f)
                v.active = false;
            break;
        case kPadSiren: {
            float gain = t < 0.02f ? t / 0.02f : v.env;
            if (t > 0.5f)
                v.env *= k_slow;
            out = std::sin(v.phase) * 0.5f * gain;
            v.phase += kTwoPi * (850.0f + 350.0f * std::sin(kTwoPi * 3.0f * t)) / fs;
            if (t > 0.8f)
                v.active = false;
            break;
        }
        }
        if (v.phase > 1000.0f * kTwoPi)  // Keep float phase precise.
            v.phase = std::fmod(v.phase, kTwoPi);
        ++v.n;
        return out;
    }

    void ApplyEffectAndPads(int16_t* pcm, size_t count, float fs, int fx) {
        // Per-sample decay factors, computed once per frame.
        float k_fast[kPadCount], k_slow[kPadCount];
        for (int pad = 0; pad < kPadCount; ++pad) {
            k_fast[pad] = std::exp(-kRates[pad][0] / fs);
            k_slow[pad] = std::exp(-kRates[pad][1] / fs);
        }
        const float lp_a = 1.0f - std::exp(-kTwoPi * 600.0f / fs);
        const float hp_a = 1.0f - std::exp(-kTwoPi * 1200.0f / fs);
        const int echo_len = std::min(kMaxFxSamples, int(fs * 0.28f));
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
                if (fx_mem_) {
                    if (echo_len_ != echo_len) {  // First use or sample rate changed.
                        echo_len_ = echo_len;
                        std::memset(fx_mem_, 0, kMaxFxSamples * sizeof(int16_t));
                        echo_pos_ = 0;
                    }
                    float d = fx_mem_[echo_pos_];
                    y = x + 0.5f * d;
                    fx_mem_[echo_pos_] = int16_t(std::clamp(x + 0.45f * d, -32768.0f, 32767.0f));
                    if (++echo_pos_ >= echo_len_)
                        echo_pos_ = 0;
                }
                break;
            case kFxReverb:
                if (fx_mem_) {
                    if (!reverb_len_[0])
                        SetupReverb(fs);
                    // Schroeder reverb: four damped parallel combs into two all-passes.
                    const float input = x * 0.25f;
                    float wet = 0;
                    for (int c = 0; c < kCombs; ++c) {
                        int16_t* line = fx_mem_ + reverb_off_[c];
                        float out = line[reverb_pos_[c]];
                        comb_damp_[c] = out * 0.7f + comb_damp_[c] * 0.3f;
                        line[reverb_pos_[c]] =
                            int16_t(std::clamp(input + comb_damp_[c] * 0.84f, -32768.0f, 32767.0f));
                        if (++reverb_pos_[c] >= reverb_len_[c])
                            reverb_pos_[c] = 0;
                        wet += out;
                    }
                    for (int a = kCombs; a < kCombs + kAllpasses; ++a) {
                        int16_t* line = fx_mem_ + reverb_off_[a];
                        float delayed = line[reverb_pos_[a]];
                        line[reverb_pos_[a]] =
                            int16_t(std::clamp(wet + delayed * 0.5f, -32768.0f, 32767.0f));
                        wet = delayed - wet;
                        if (++reverb_pos_[a] >= reverb_len_[a])
                            reverb_pos_[a] = 0;
                    }
                    y = x * 0.75f + wet * 0.7f;
                }
                break;
            case kFxFlanger: {
                // 1-5 ms delay swept by a 0.3 Hz LFO, with feedback.
                lfo_phase_ += kTwoPi * 0.3f / fs;
                if (lfo_phase_ > kTwoPi)
                    lfo_phase_ -= kTwoPi;
                float delay = std::min(fs * (0.003f + 0.002f * std::sin(lfo_phase_)),
                                       float(kFlangerSamples - 2));
                float read = float(flanger_pos_) - delay;
                if (read < 0)
                    read += kFlangerSamples;
                int i0 = int(read);
                float frac = read - float(i0);
                float d = flanger_[i0 % kFlangerSamples] * (1 - frac) +
                          flanger_[(i0 + 1) % kFlangerSamples] * frac;
                flanger_[flanger_pos_] = std::clamp(x + 0.5f * d, -32768.0f, 32767.0f);
                flanger_pos_ = (flanger_pos_ + 1) % kFlangerSamples;
                y = x * 0.7f + d * 0.8f;
                break;
            }
            case kFxWobble: {
                // Resonant low-pass (state-variable) swept between 150 Hz and 2.5 kHz at 1.5 Hz.
                lfo_phase_ += kTwoPi * 1.5f / fs;
                if (lfo_phase_ > kTwoPi)
                    lfo_phase_ -= kTwoPi;
                float cutoff = 150.0f + 2350.0f * (0.5f + 0.5f * std::sin(lfo_phase_));
                float f = std::min(2.0f * std::sin(3.14159265f * cutoff / fs), 0.9f);
                svf_low_ += f * svf_band_;
                float high = x - svf_low_ - 0.35f * svf_band_;
                svf_band_ += f * high;
                svf_low_ = std::clamp(svf_low_, -60000.0f, 60000.0f);
                svf_band_ = std::clamp(svf_band_, -60000.0f, 60000.0f);
                y = svf_low_;
                break;
            }
            case kFxRobot:
                // Ring modulation with a 220 Hz carrier keeps a little of the dry signal.
                carrier_phase_ += kTwoPi * 220.0f / fs;
                if (carrier_phase_ > kTwoPi)
                    carrier_phase_ -= kTwoPi;
                y = x * (0.25f + 0.9f * std::sin(carrier_phase_));
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
                y += VoiceSample(pad, v, fs, k_fast[pad], k_slow[pad]) * 24000.0f;
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

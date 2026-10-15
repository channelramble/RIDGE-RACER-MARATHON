// Core audio -> SDL_AudioStream (S16 stereo at the core's rate; SDL resamples to the device).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

namespace rrm::lr {

class Audio {
public:
    bool open(double sampleRate);  // false: no device, the host keeps running silently
    void close();
    bool ok() const { return stream_ != nullptr; }

    void setSampleRate(double sampleRate);
    double sampleRate() const { return rate_; }

    // Collects samples during retro_run; submit() hands them to SDL (or drops them when muted).
    void append(const int16_t* stereo, size_t frames);
    void submit(bool muted);

    // Playback ratio: >1 consumes faster (higher pitch). Clamped to SDL's range.
    void setRatio(double ratio);
    double ratio() const { return ratio_; }
    // Wall-clock seconds of audio still queued (input data / (rate * ratio)).
    double queuedSeconds() const;
    void clear();
    void prefill(double seconds);  // silence, so playback starts without an underrun
    void setPaused(bool paused);

private:
    SDL_AudioStream* stream_ = nullptr;
    double rate_ = 0;
    double ratio_ = 1.0;
    std::vector<int16_t> pending_;
    std::vector<int16_t> silence_;
};

}  // namespace rrm::lr

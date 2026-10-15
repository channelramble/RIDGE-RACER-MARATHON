#include "libretro/Audio.h"

#include <algorithm>
#include <cmath>

namespace rrm::lr {

namespace {

SDL_AudioSpec specFor(double rate) {
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = std::max(1, static_cast<int>(std::lround(rate)));
    return spec;
}

}  // namespace

bool Audio::open(double sampleRate) {
    close();
    if (sampleRate <= 0) sampleRate = 44100;
    rate_ = sampleRate;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "[libretro] audio init failed: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = specFor(sampleRate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "[libretro] no audio device: %s", SDL_GetError());
        return false;
    }
    ratio_ = 1.0;
    pending_.reserve(8192);
    SDL_AudioSpec device{};
    int deviceFrames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &deviceFrames)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[libretro] audio: core %.0f Hz -> device %d Hz, %d frames/buffer",
                    sampleRate, device.freq, deviceFrames);
    }
    SDL_ResumeAudioStreamDevice(stream_);  // device streams open paused
    return true;
}

void Audio::close() {
    if (stream_) SDL_DestroyAudioStream(stream_);  // also closes the device
    stream_ = nullptr;
    pending_.clear();
    ratio_ = 1.0;
}

void Audio::setSampleRate(double sampleRate) {
    if (sampleRate <= 0 || sampleRate == rate_) return;
    rate_ = sampleRate;
    if (!stream_) return;
    SDL_AudioSpec spec = specFor(sampleRate);
    SDL_ClearAudioStream(stream_);
    SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
}

void Audio::append(const int16_t* stereo, size_t frames) {
    if (!stereo || !frames) return;
    pending_.insert(pending_.end(), stereo, stereo + frames * 2);
}

void Audio::submit(bool muted) {
    if (stream_ && !muted && !pending_.empty()) {
        SDL_PutAudioStreamData(stream_, pending_.data(), static_cast<int>(pending_.size() * sizeof(int16_t)));
    }
    pending_.clear();
}

void Audio::setRatio(double ratio) {
    ratio = std::clamp(ratio, 0.05, 50.0);
    if (!stream_ || std::fabs(ratio - ratio_) < 1e-6) return;
    if (SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio))) ratio_ = ratio;
}

double Audio::queuedSeconds() const {
    if (!stream_ || rate_ <= 0) return 0;
    int bytes = SDL_GetAudioStreamQueued(stream_);
    if (bytes <= 0) return 0;
    return (bytes / 4.0) / (rate_ * ratio_);
}

void Audio::clear() {
    if (stream_) SDL_ClearAudioStream(stream_);
    pending_.clear();
}

void Audio::prefill(double seconds) {
    if (!stream_ || seconds <= 0) return;
    size_t frames = static_cast<size_t>(seconds * rate_ * ratio_);
    silence_.assign(frames * 2, 0);
    SDL_PutAudioStreamData(stream_, silence_.data(), static_cast<int>(silence_.size() * sizeof(int16_t)));
}

void Audio::setPaused(bool paused) {
    if (!stream_) return;
    if (paused) SDL_PauseAudioStreamDevice(stream_);
    else SDL_ResumeAudioStreamDevice(stream_);
}

}  // namespace rrm::lr

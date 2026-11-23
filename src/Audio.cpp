#include "Audio.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace storms {
namespace {
constexpr float Tau = std::numbers::pi_v<float> * 2.f;

float whiteNoise(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.f / 8388608.f) - 1.f;
}
}

bool Audio::open() {
    if (stream && AAudioStream_getState(stream) != AAUDIO_STREAM_STATE_DISCONNECTED) return true;
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
        stream = nullptr;
    }
    AAudioStreamBuilder* builder{};
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(builder, render, this);
    const auto status = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (status != AAUDIO_OK || !stream) return false;
    channels = AAudioStream_getChannelCount(stream);
    rate = AAudioStream_getSampleRate(stream);
    const bool ready = (channels == 1 || channels == 2) && rate >= 8000
        && AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_FLOAT
        && AAudioStream_requestStart(stream) == AAUDIO_OK;
    if (!ready) {
        AAudioStream_close(stream);
        stream = nullptr;
    }
    return ready;
}

void Audio::setGains(float a, float b, float c, bool covered, double time) {
    softGain.store(a, std::memory_order_relaxed);
    strongGain.store(b, std::memory_order_relaxed);
    breezeGain.store(c, std::memory_order_relaxed);
    enclosure.store(covered ? 1.f : 0.f, std::memory_order_relaxed);
    const bool audible = a + b + c > 0.001f;
    if (audible) {
        quietSince = time;
        if (time >= retryAt && !open()) retryAt = time + 5.;
    } else if (stream && time - quietSince > 4.) {
        close();
    }
}

void Audio::update(const Environment& env, float volume, float rain, bool covered, bool windSounds, double time) {
    if (nextBreeze == 0.) nextBreeze = time + random.between(1, 2.5f);
    if (time >= nextBreeze) {
        nextBreeze = time + random.between(9, 24);
        if (env.wind > 0.3f && rain < 0.20f && windSounds && !covered) breezeStart = time;
    }
    const float age = float(time - breezeStart);
    const float breeze = age < 3.2f && rain < 0.20f && windSounds && !covered
        ? std::sin(age / 3.2f * std::numbers::pi_v<float>) * 0.47f * env.wind : 0.f;
    const float level = volume * (covered ? 0.2f : 1.f);
    setGains(env.soft * level, env.strong * level, breeze * volume, covered, time);
}

void Audio::quiet(double time) {
    nextBreeze = 0.;
    breezeStart = -20.;
    setGains(0.f, 0.f, 0.f, false, time);
}

void Audio::close() {
    softGain = strongGain = breezeGain = 0.f;
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
        stream = nullptr;
    }
    soft = strong = breeze = damping = low = band = high = side = phase = 0.f;
    retryAt = 0.;
    nextBreeze = 0.;
    breezeStart = -20.;
}

aaudio_data_callback_result_t Audio::render(AAudioStream*, void* user, void* output, int32_t count) {
    auto& self = *static_cast<Audio*>(user);
    auto* pcm = static_cast<float*>(output);
    const float a = self.softGain.load(std::memory_order_relaxed);
    const float b = self.strongGain.load(std::memory_order_relaxed);
    const float c = self.breezeGain.load(std::memory_order_relaxed);
    const float enclosure = self.enclosure.load(std::memory_order_relaxed);
    const float rate = float(self.rate);
    const float slew = 1.f - std::exp(-5.f / rate);
    const float bass = 1.f - std::exp(-Tau * 75.f / rate);
    const float mid = 1.f - std::exp(-Tau * 330.f / rate);
    const float air = 1.f - std::exp(-Tau * 1600.f / rate);
    for (int frame = 0; frame < count; ++frame) {
        self.soft += slew * (a - self.soft);
        self.strong += slew * (b - self.strong);
        self.breeze += slew * (c - self.breeze);
        self.damping += slew * (enclosure - self.damping);
        self.phase += Tau * (0.11f + self.strong * 0.07f) / rate;
        if (self.phase > Tau) self.phase -= Tau;
        const float gust = 0.62f + 0.23f * std::sin(self.phase) + 0.14f * std::sin(3.f * self.phase + 0.8f);
        const float wind = whiteNoise(self.noise);
        self.low += bass * (wind - self.low);
        self.band += mid * (wind - self.band);
        self.high += air * (wind - self.high);
        self.side += mid * (whiteNoise(self.noise) - self.side);
        const float muffled = 1.f - self.damping * 0.82f;
        const float center = gust * (self.soft * (0.37f * self.band + 0.11f * self.high * muffled)
            + self.strong * (0.69f * self.low + 0.51f * self.band + 0.21f * self.high * muffled)
            + self.breeze * (0.29f * self.band + 0.17f * self.high));
        const float stereo = 0.16f * gust * self.side * (self.soft + self.strong + self.breeze);
        const int index = frame * self.channels;
        pcm[index] = std::clamp(center - stereo, -1.f, 1.f);
        if (self.channels == 2) pcm[index + 1] = std::clamp(center + stereo, -1.f, 1.f);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

}

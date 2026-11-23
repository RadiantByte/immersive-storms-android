#pragma once

#include "Particles.hpp"

#include <aaudio/AAudio.h>
#include <atomic>

namespace storms {

class Audio {
public:
    void update(const Environment& env, float volume, float rain, bool covered, bool windSounds, double time);
    void quiet(double time);
    void close();
private:
    static aaudio_data_callback_result_t render(AAudioStream*, void*, void*, int32_t);
    bool open();
    void setGains(float soft, float strong, float breeze, bool covered, double time);

    AAudioStream* stream{};
    int channels{}, rate{};
    double retryAt{}, quietSince{}, breezeStart{-20}, nextBreeze{};
    Random random;
    std::atomic<float> softGain{}, strongGain{}, breezeGain{}, enclosure{};
    float soft{}, strong{}, breeze{}, damping{}, low{}, band{}, high{}, side{};
    std::uint32_t noise{0xca73c217};
    float phase{};
};

}

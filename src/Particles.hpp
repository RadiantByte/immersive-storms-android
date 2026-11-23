#pragma once

#include "Weather.hpp"

#include <array>

namespace storms {

enum class ParticleKind : std::uint8_t { Wind, Dust, Rain, Splash };

struct Particle {
    Vec3 position;
    Vec3 velocity;
    Color color;
    float age{}, lifetime{}, width{}, ground{};
    ParticleKind kind{};
    std::uint8_t sprite{};
};

struct Column {
    float surface{};
    Biome biome;
};

struct Sampler {
    void* context{};
    std::optional<Column>(*sample)(void*, Vec3){};
    std::optional<Column> operator()(Vec3 at) const { return sample(context, at); }
};

class Random {
public:
    float unit();
    float between(float a, float b);
private:
    std::uint32_t state{0x59ba83ef};
};

class Particles {
public:
    static constexpr unsigned Capacity = 760;
    void clear();
    void tick(Vec3 camera, Environment env, float rain, float thunder,
              const Settings& settings, std::span<const Rule> rules, const Sampler& sampler);
    std::span<const Particle> view() const { return entries; }
private:
    void advance();
    void add(Vec3 position, Vec3 velocity, Color color, float life, float width, float ground, ParticleKind kind);
    std::vector<Particle> entries;
    Random random;
};

struct Vertex {
    Vec3 position;
    Color color;
    float alpha{};
};

void buildMesh(std::vector<Vertex>& mesh, std::span<const Particle> particles, Vec3 camera,
               Vec3 right, Vec3 up, float frameFraction, float weatherLight);

}

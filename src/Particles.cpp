#include "Particles.hpp"

#include <algorithm>
#include <cmath>

namespace storms {
namespace {

float distanceSquared(Vec3 delta) { return delta.x * delta.x + delta.y * delta.y + delta.z * delta.z; }

Color lit(Color color, float scale) { return {color.r * scale, color.g * scale, color.b * scale}; }

void quad(std::vector<Vertex>& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Color color, float alpha) {
    mesh.insert(mesh.end(), {{a, color, alpha}, {b, color, alpha}, {c, color, alpha},
                             {a, color, alpha}, {c, color, alpha}, {d, color, alpha}});
}

}

float Random::unit() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.0f / 16777216.f);
}

float Random::between(float a, float b) { return a + (b - a) * unit(); }

void Particles::clear() { entries.clear(); }

void Particles::add(Vec3 position, Vec3 velocity, Color color, float life, float width, float ground, ParticleKind kind) {
    if (entries.size() >= Capacity) return;
    const auto sprite = static_cast<std::uint8_t>(random.unit() * 3);
    entries.push_back({position, velocity, color, 0.f, life, width, ground, kind, sprite});
}

void Particles::advance() {
    for (auto& p : entries) {
        p.age += 1.f;
        if (p.age >= p.lifetime) continue;
        p.position = p.position + p.velocity;
        if (p.kind == ParticleKind::Dust || p.kind == ParticleKind::Wind) {
            p.velocity = {p.velocity.x * 0.96f, p.velocity.y - 0.004f, p.velocity.z * 0.96f};
            if (p.position.y < p.ground) {
                p.position.y = p.ground;
                p.velocity.y = 0.f;
            }
        } else if (p.kind == ParticleKind::Rain && p.position.y < p.ground) {
            p.kind = ParticleKind::Splash;
            p.position.y = p.ground + 0.03f;
            p.velocity = {};
            p.age = 0.f;
            p.lifetime = random.between(4.f, 8.f);
            p.width *= 7.0f;
        }
    }
    std::erase_if(entries, [](const Particle& p) { return p.age >= p.lifetime; });
}

void Particles::tick(Vec3 camera, Environment env, float rain, float thunder,
                     const Settings& config, std::span<const Rule> rules, const Sampler& sampler) {
    if (entries.capacity() < Capacity) entries.reserve(Capacity);
    if (entries.size() > 0) advance();

    if (config.windParticles) {
        for (int i = 0; i < 3; ++i) {
            if (random.unit() >= 0.2f * config.windChance) continue;
            const auto at = camera + Vec3{random.between(-20, 20), 0, random.between(-20, 20)};
            const auto column = sampler(at);
            if (!column || std::abs(column->surface - camera.y) > 21.f) continue;
            const auto biomeWeather = evaluate(column->biome, rain, thunder, config, rules);
            if (random.unit() >= biomeWeather.wind) continue;
            const float speed = random.between(-0.62f, -0.23f);
            add({at.x, column->surface + random.between(0.15f, 0.35f), at.z}, {speed, 0, speed},
                lit(biomeWeather.windColor, random.between(0.6f, 1.f)), random.between(12, 27),
                config.windSize * random.between(0.045f, 0.107f), column->surface + 0.02f, ParticleKind::Wind);
        }
    }

    if (config.dust && (rain > 0.20f || env.dust > 0.10f)) {
        const float radius = float(config.dustRadius);
        const float expected = 4.f * radius * radius / 60.f * config.dustDensity * std::max(rain, env.dust);
        const int count = static_cast<int>(std::min(expected + random.unit(), 46.f));
        for (int i = 0; i < count; ++i) {
            const auto at = camera + Vec3{random.between(-radius * 0.5f, radius * 1.5f),
                random.between(-radius * 0.5f, radius * 0.5f), random.between(-radius, radius)};
            const auto column = sampler(at);
            if (!column || (column->surface > camera.y + radius * 0.6f + 4.f)) continue;
            const auto biomeWeather = evaluate(column->biome, rain, thunder, config, rules);
            if (random.unit() >= biomeWeather.dust) continue;
            add({at.x, std::max(at.y, column->surface + 0.02f) + random.unit(), at.z},
                {-1.f, random.between(-0.08f, 0.05f), random.between(-0.08f, 0.08f)},
                lit(Sandy, random.between(0.60f, 1.f)), random.between(11, 24),
                config.dustSize * random.between(0.08f, 0.23f), column->surface + 0.02f, ParticleKind::Dust);
        }
    }

    if (config.blackRain && env.blackRain > 0.04f) {
        const int count = static_cast<int>(std::min(37.f, 4.f + 33.f * std::max(rain, env.blackRain)));
        for (int i = 0; i < count; ++i) {
            const auto at = camera + Vec3{random.between(-12, 12), random.between(7, 15), random.between(-12, 12)};
            const auto column = sampler(at);
            if (!column || column->surface > at.y || column->surface < camera.y - 20.f) continue;
            const auto biomeWeather = evaluate(column->biome, rain, thunder, config, rules);
            if (random.unit() >= biomeWeather.blackRain) continue;
            add(at, {-0.055f, random.between(-1.4f, -0.85f), -0.025f}, BlackRain, 37.f,
                random.between(0.022f, 0.042f), column->surface, ParticleKind::Rain);
        }
    }
}

void buildMesh(std::vector<Vertex>& mesh, std::span<const Particle> particles, Vec3 camera,
               Vec3 right, Vec3 up, float frameFraction, float weatherLight) {
    mesh.clear();
    for (const auto& p : particles) {
        Vec3 center = p.position + p.velocity * frameFraction - camera;
        const float range = distanceSquared(center);
        if (range < 0.12f || range > 19000.f) continue;

        const float phase = (p.age + frameFraction) / p.lifetime;
        const Color tint = lit(p.color, weatherLight);
        const float alpha = std::clamp(std::min(phase * 6.f, (1.f - phase) * 4.f), 0.f, 1.f);
        if (p.kind == ParticleKind::Splash) {
            const auto w = right * (p.width * (0.5f + phase));
            const auto h = up * (p.width * (0.32f + phase * 0.35f));
            const Vec3 rise{0, phase * 0.16f, 0};
            quad(mesh, center - w, center + rise - h, center + w, center + rise + h, tint, alpha * 0.80f);
        } else if (p.kind == ParticleKind::Rain) {
            const auto w = right * p.width;
            const auto fall = p.velocity * 0.80f;
            quad(mesh, center - w, center + fall - w, center + fall + w, center + w, tint, alpha * 0.77f);
        } else {
            constexpr std::array<int, 4> SpriteX{4, 3, 5, 4};
            constexpr std::array<int, 4> SpriteY{4, 3, 4, 6};
            constexpr std::array<int, 3> PixelCount{1, 2, 4};
            const auto w = right * (p.width * 0.4f);
            const auto h = up * (p.width * 0.4f);
            for (int i = 0; i < PixelCount[p.sprite]; ++i) {
                const auto pixel = center + w * float(2 * SpriteX[i] - 8) + h * float(8 - 2 * SpriteY[i]);
                quad(mesh, pixel - w - h, pixel + w - h, pixel + w + h, pixel - w + h, tint, alpha * 0.82f);
            }
        }
    }
}

}

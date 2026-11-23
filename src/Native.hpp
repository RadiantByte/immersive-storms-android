#pragma once

#include "Particles.hpp"

#include <cstddef>
#include <memory>

namespace storms::native {

enum class Build { Missing, Unsupported, Supported };
Build inspect();
inline std::uintptr_t base{};

namespace rva {
inline constexpr std::uintptr_t ClientUpdate = 0x9B7B418;
inline constexpr std::uintptr_t LocalPlayer = 0x9B7F7B0;
inline constexpr std::uintptr_t PlayerTick = 0xAF5EB10;
inline constexpr std::uintptr_t ActorRemoved = 0xFD9969C;
inline constexpr std::uintptr_t DimensionId = 0xFD8344C;
inline constexpr std::uintptr_t RenderLevel = 0xB7A3C80;
inline constexpr std::uintptr_t SetupFog = 0xB7BFDBC;
inline constexpr std::uintptr_t SkyColor = 0xB84A788;
inline constexpr std::uintptr_t ExtractWeather = 0xB7665A8;
inline constexpr std::uintptr_t SpawnWeather = 0xB7A8B88;
inline constexpr std::uintptr_t ClientParticle = 0xFEF9A28;
inline constexpr std::uintptr_t GetBiome = 0x1037B980;
inline constexpr std::uintptr_t BiomeTemperature = 0x10AA07A8;
inline constexpr std::uintptr_t RainLevel = 0x10AA12AC;
inline constexpr std::uintptr_t LightningLevel = 0x10AA1284;
inline constexpr std::uintptr_t Surface = 0x10AA14D4;
inline constexpr std::uintptr_t TimeOfDay = 0x10B3F3E8;
inline constexpr std::uintptr_t TessBegin = 0xAC2AAB8;
inline constexpr std::uintptr_t TessColor = 0xAC2B06C;
inline constexpr std::uintptr_t TessVertex = 0xAC2B308;
inline constexpr std::uintptr_t RenderMesh = 0xB76A3CC;
inline constexpr std::uintptr_t MaterialResolved = 0x117A93F8;
inline constexpr std::uintptr_t MaterialName = 0x117A9438;
}

namespace offset {
inline constexpr std::size_t ActorLevel = 0x1F0;
inline constexpr std::size_t ActorDimension = 0x1E0;
inline constexpr std::size_t ActorState = 0x228;
inline constexpr std::size_t PlayerClient = 0xCA8;
inline constexpr std::size_t ClientRenderer = 0x1B8;
inline constexpr std::size_t PlayerRenderer = 0x430;
inline constexpr std::size_t RendererRegion = 0x958;
inline constexpr std::size_t RendererDimension = 0x968;
inline constexpr std::size_t RendererWeather = 0x930;
inline constexpr std::size_t WeatherOwner = 0x38;
inline constexpr std::size_t RendererCamera = 0x614;
inline constexpr std::size_t RendererMedium = 0x949;
inline constexpr std::size_t RendererView = 0xEB0;
inline constexpr std::size_t ParticleMaterial = 0x1048;
inline constexpr std::size_t FogColor = 0x41C;
inline constexpr std::size_t FogStart = 0x42C;
inline constexpr std::size_t FogEnd = 0x430;
inline constexpr std::size_t DimensionWeather = 0x1B8;
inline constexpr std::size_t BiomeName = 0x198;
inline constexpr std::size_t BiomePrecipitation = 0x40;
}

template<class T>
T addr(std::uintptr_t rva) { return reinterpret_cast<T>(base + rva); }

template<class T>
T& field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

template<class Fn>
Fn slot(void* object, std::size_t byteOffset) {
    return reinterpret_cast<Fn>((*static_cast<void***>(object))[byteOffset / sizeof(void*)]);
}

struct World {
    void* client{};
    void* level{};
    void* dimension{};
    void* region{};
    void* playerRenderer{};
    void* weatherRenderer{};
    Vec3 camera;
    float rain{}, thunder{}, light{};
    bool medium{};
};

void* localPlayer(void* client);
std::optional<World> world(void* player);
Biome biome(void* region, Vec3 at);
std::optional<Column> column(void* region, Vec3 at);
bool positionValid(Vec3 pos);
bool viewVectors(void* renderer, Vec3& right, Vec3& up);
const void* particleMaterial(void* renderer);
void draw(void* context, void* renderer, std::span<const Vertex> mesh);

struct Rgba { float r, g, b, a; };

struct WeatherDrawData {
    alignas(8) std::byte beforeRain[0x2A14];
    float rain;
    std::byte other[0x30];
};
static_assert(offsetof(WeatherDrawData, rain) == 0x2A14 && sizeof(WeatherDrawData) == 0x2A48);

}

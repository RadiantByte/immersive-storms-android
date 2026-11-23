#include "Native.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <link.h>

namespace storms::native {
namespace {

constexpr std::uint8_t BuildId[]{
    0xe9, 0x6d, 0xdd, 0x68, 0x54, 0x18, 0x14, 0x92, 0x3c, 0x35,
    0x46, 0x0f, 0x8b, 0x99, 0xf2, 0x7a, 0x66, 0xc5, 0x12, 0x32,
};
struct BlockPos { int x, y, z; };
static_assert(sizeof(BlockPos) == 12 && sizeof(std::string) == 24);

bool noteMatches(const std::byte* notes, std::size_t size) {
    std::size_t start{};
    while (size - start >= sizeof(ElfW(Nhdr))) {
        ElfW(Nhdr) note{};
        std::memcpy(&note, notes + start, sizeof(note));
        start += sizeof(note);
        const std::size_t names = (std::size_t(note.n_namesz) + 3u) & ~std::size_t(3u);
        const std::size_t desc = (std::size_t(note.n_descsz) + 3u) & ~std::size_t(3u);
        if (names > size - start || desc > size - start - names) return false;
        if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && note.n_descsz == sizeof(BuildId)
            && std::memcmp(notes + start, "GNU", 4) == 0
            && std::memcmp(notes + start + names, BuildId, sizeof(BuildId)) == 0) return true;
        start += names + desc;
    }
    return false;
}

int checkImage(dl_phdr_info* info, std::size_t, void* result) {
    const std::string_view name = info->dlpi_name ? info->dlpi_name : "";
    if (name.substr(name.find_last_of('/') + 1) != "libminecraftpe.so") return 0;
    auto& build = *static_cast<Build*>(result);
    build = Build::Unsupported;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& header = info->dlpi_phdr[i];
        if (header.p_type != PT_NOTE) continue;
        const auto* notes = reinterpret_cast<const std::byte*>(info->dlpi_addr + header.p_vaddr);
        if (!noteMatches(notes, header.p_filesz)) continue;
        base = info->dlpi_addr;
        build = Build::Supported;
        return 1;
    }
    return 1;
}

BlockPos blocks(Vec3 pos, void* region) {
    const auto bottom = field<std::int16_t>(region, 0x42);
    const auto top = field<std::int16_t>(region, 0x40);
    const int y = static_cast<int>(std::floor(pos.y));
    return {static_cast<int>(std::floor(pos.x)), bottom < top ? std::clamp(y, int(bottom), int(top) - 1) : y,
            static_cast<int>(std::floor(pos.z))};
}

float strength(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
}

Biome readBiome(void* region, const BlockPos& position) {
    auto* info = addr<void*(*)(void*, const BlockPos&)>(rva::GetBiome)(region, position);
    if (!info) return {};
    const auto& name = field<std::string>(info, offset::BiomeName);
    if (name.empty() || name.size() > 192) return {};
    const auto precip = field<bool>(info, offset::BiomePrecipitation);
    const bool snow = precip && addr<float(*)(void*, void*, const BlockPos&)>(rva::BiomeTemperature)(info, region, position) <= 0.15f;
    return {name, true, precip, snow};
}

float norm(Vec3 v) { return std::hypot(v.x, v.y, v.z); }

constexpr std::uint64_t materialHash(std::string_view value) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto character : value) hash = hash * 0x100000001b3ULL ^ std::uint8_t(character);
    return hash;
}

struct WorldDrawParameters {
    alignas(8) std::byte storage[0x18]{};
    std::int32_t variant{};
};
static_assert(offsetof(WorldDrawParameters, variant) == 0x18);

}

Build inspect() {
    Build result = Build::Missing;
    dl_iterate_phdr(checkImage, &result);
    return result;
}

void* localPlayer(void* client) {
    return client ? addr<void*(*)(void*)>(rva::LocalPlayer)(client) : nullptr;
}

bool positionValid(Vec3 value) {
    return std::abs(value.x) < 31'000'000.f && std::abs(value.y) < 100'000.f && std::abs(value.z) < 31'000'000.f;
}

std::optional<World> world(void* player) {
    if (!player || addr<bool(*)(void*)>(rva::ActorRemoved)(player)) return {};
    auto* client = field<void*>(player, offset::PlayerClient);
    auto* level = field<void*>(player, offset::ActorLevel);
    auto* dimension = field<void*>(player, offset::ActorDimension);
    auto* state = field<void*>(player, offset::ActorState);
    if (!client || !level || !dimension || !state || localPlayer(client) != player) return {};
    if (addr<int(*)(void*)>(rva::DimensionId)(player) != 0) return {};

    auto* levelRenderer = field<void*>(client, offset::ClientRenderer);
    auto* renderer = levelRenderer ? field<void*>(levelRenderer, offset::PlayerRenderer) : nullptr;
    if (!renderer || field<void*>(renderer, offset::RendererDimension) != dimension) return {};
    auto* region = field<void*>(renderer, offset::RendererRegion);
    auto* precip = field<void*>(dimension, offset::DimensionWeather);
    auto* weather = field<void*>(renderer, offset::RendererWeather);
    const auto camera = field<Vec3>(renderer, offset::RendererCamera);
    if (!region || !precip || !weather || !positionValid(camera)) return {};
    const auto playerPosition = field<Vec3>(state, 0);
    if (!positionValid(playerPosition) || norm(camera - playerPosition) > 128.f) return {};
    const float rain = strength(addr<float(*)(void*, float)>(rva::RainLevel)(precip, 1.f));
    const float lightning = strength(addr<float(*)(void*, float)>(rva::LightningLevel)(precip, 1.f));
    const float time = addr<float(*)(void*, float)>(rva::TimeOfDay)(dimension, 1.f);
    const float light = std::isfinite(time) ? std::clamp(0.67f + 0.39f * std::cos(time * 6.2831853f), 0.30f, 1.f) : 1.f;
    return World{client, level, dimension, region, renderer, weather, camera, rain, rain * lightning, light,
                 field<bool>(renderer, offset::RendererMedium)};
}

Biome biome(void* region, Vec3 at) {
    if (!region || !positionValid(at)) return {};
    const BlockPos pos = blocks(at, region);
    auto* chunk = slot<void*(*)(void*, const BlockPos&)>(region, 0x138)(region, pos);
    return chunk ? readBiome(region, pos) : Biome{};
}

std::optional<Column> column(void* region, Vec3 at) {
    if (!region || !positionValid(at)) return {};
    const BlockPos pos = blocks(at, region);
    auto* chunk = slot<void*(*)(void*, const BlockPos&)>(region, 0x138)(region, pos);
    if (!chunk) return {};
    const auto height = addr<BlockPos(*)(void*, const BlockPos&)>(rva::Surface)(chunk, pos);
    if (height.x != pos.x || height.z != pos.z || height.y < field<std::int16_t>(region, 0x42)
        || height.y > field<std::int16_t>(region, 0x40)) return {};
    BlockPos sample = pos;
    sample.y = std::min(std::max(pos.y, height.y), int(field<std::int16_t>(region, 0x40)) - 1);
    return Column{float(height.y), readBiome(region, sample)};
}

bool viewVectors(void* renderer, Vec3& right, Vec3& up) {
    const auto& view = field<std::array<float, 16>>(renderer, offset::RendererView);
    right = {view[0], view[4], view[8]};
    up = {view[1], view[5], view[9]};
    const float r = norm(right), u = norm(up);
    return r > 0.8f && r < 1.2f && u > 0.8f && u < 1.2f;
}

const void* particleMaterial(void* renderer) {
    if (!renderer) return nullptr;
    const auto* material = static_cast<std::byte*>(renderer) + offset::ParticleMaterial;
    if (!addr<bool(*)(const void*)>(rva::MaterialResolved)(material)) return nullptr;
    const auto* hash = addr<const std::uint64_t*(*)(const void*)>(rva::MaterialName)(material);
    return hash && *hash == materialHash("selection_overlay_double_sided") ? material : nullptr;
}

void draw(void* context, void* renderer, std::span<const Vertex> mesh) {
    if (!context || mesh.empty()) return;
    void* tess = field<void*>(context, 0xB0);
    auto* tint = field<Rgba*>(context, 0x30);
    if (!tess || !tint || field<bool>(tess, 0x298) || field<bool>(tess, 0x255)) return;
    const auto* material = particleMaterial(renderer);
    if (!material) return;

    const Rgba previous = *tint;
    *tint = {1.f, 1.f, 1.f, 1.f};
    field<std::uint8_t>(tint, 0x10) = 1;
    addr<void(*)(void*, void*, int, int, bool)>(rva::TessBegin)(tess, nullptr, 2, int(mesh.size()), false);
    const auto color = addr<void(*)(void*, float, float, float, float)>(rva::TessColor);
    const auto vertex = addr<void(*)(void*, float, float, float)>(rva::TessVertex);
    for (const auto& v : mesh) {
        color(tess, v.color.r, v.color.g, v.color.b, v.alpha);
        vertex(tess, v.position.x, v.position.y, v.position.z);
    }
    const WorldDrawParameters parameters;
    addr<void(*)(void*, void*, const void*, const WorldDrawParameters&)>(rva::RenderMesh)(context, tess, material, parameters);
    *tint = previous;
    field<std::uint8_t>(tint, 0x10) = 1;
}

}

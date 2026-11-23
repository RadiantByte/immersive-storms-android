#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace storms {

struct Vec3 {
    float x{}, y{}, z{};
    Vec3 operator+(Vec3 v) const { return {x + v.x, y + v.y, z + v.z}; }
    Vec3 operator-(Vec3 v) const { return {x - v.x, y - v.y, z - v.z}; }
    Vec3 operator*(float n) const { return {x * n, y * n, z * n}; }
};

struct Color {
    float r{}, g{}, b{};
    constexpr bool operator==(const Color&) const = default;
};

constexpr Color rgb(std::uint32_t color) {
    return {float(color >> 16 & 255) / 255.f, float(color >> 8 & 255) / 255.f, float(color & 255) / 255.f};
}

inline constexpr Color White{1, 1, 1};
inline constexpr Color Sandy = rgb(0xD9AA84);
inline constexpr Color Rocky = rgb(0x888888);
inline constexpr Color BlackRain = rgb(0x202020);

enum class Archetype : std::uint8_t { Normal, Sandstorm, Blizzard, Dense };
enum class Wind : std::uint8_t { None, Soft, Strong };
enum class Condition : std::uint8_t { Clear, Rain, Thunder };

struct Effects {
    float distance{1.f};
    Color fog{White}, sky{White};
    Archetype type{Archetype::Normal};
    Wind sound{Wind::None};
    bool windy{}, dust{}, blackRain{};
};

using Profile = std::array<Effects, 3>;

struct Rule {
    std::string biome;
    Condition weather{};
    std::optional<float> distance;
    std::optional<Color> fog, sky;
    std::optional<Archetype> type;
    std::optional<Wind> sound;
    std::optional<bool> windy, dust, blackRain;
};

struct Settings {
    bool enabled{true};
    bool fog{true}, sandFog{true}, blizzardFog{true}, denseFog{true}, blackRain{true};
    bool windParticles{true}, windSounds{true}, stormSounds{true}, dust{true};
    float fogDistance{1.f}, windChance{1.f}, windSize{1.f}, dustDensity{1.f}, dustSize{1.f}, volume{1.f};
    int dustRadius{20};
};

struct Environment {
    float distance{1.f};
    Color fog{White}, sky{White};
    float wind{}, dust{}, blackRain{}, soft{}, strong{};
    Color windColor{Rocky};
};

struct Biome {
    std::string_view name;
    bool known{};
    bool precipitation{};
    bool snow{};
};

Profile profile(std::string_view biome, std::span<const Rule> rules);
Environment evaluate(Biome biome, float rain, float thunder, const Settings& settings, std::span<const Rule> rules);
Environment mix(Environment a, const Environment& b, float fraction);
std::optional<std::vector<Rule>> loadRules(const std::filesystem::path& path);
std::string_view trim(std::string_view text);
std::optional<float> number(std::string_view text);
std::optional<bool> boolean(std::string_view text);

enum class Control { Toggle, Slider, Integer };
struct Setting {
    std::string_view key, title, category, description;
    Control control;
    bool Settings::*flag{};
    float Settings::*real{};
    int Settings::*integer{};
    float min{}, max{}, step{};
};

std::span<const Setting> settingList();
bool set(Settings& settings, const Setting& option, std::string_view value);
std::string value(const Settings& settings, const Setting& option);
Settings loadSettings(const std::filesystem::path& path);
bool saveSettings(const std::filesystem::path& path, const Settings& settings);

}

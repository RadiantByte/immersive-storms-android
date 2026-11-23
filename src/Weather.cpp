#include "Weather.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace storms {
namespace {

enum Family : unsigned {
    Dry = 1, Red = 2, Pink = 4, Icy = 8, Dense = 16, Pale = 32, Windy = 64,
};

using namespace std::string_view_literals;

constexpr std::string_view DryBiomes[]{
    "desert"sv, "desert_hills"sv, "desert_mutated"sv, "savanna"sv, "savanna_plateau"sv,
    "savanna_mutated"sv, "savanna_plateau_mutated"sv, "shattered_savanna"sv,
    "shattered_savanna_plateau"sv, "windswept_savanna"sv,
};
constexpr std::string_view RedBiomes[]{
    "badlands"sv, "wooded_badlands"sv, "eroded_badlands"sv, "mesa"sv, "mesa_bryce"sv,
    "mesa_plateau_stone"sv, "mesa_plateau"sv, "mesa_plateau_stone_mutated"sv, "mesa_plateau_mutated"sv,
};
constexpr std::string_view Mountains[]{
    "extreme_hills"sv, "extreme_hills_edge"sv, "extreme_hills_plus_trees"sv, "extreme_hills_mutated"sv,
    "extreme_hills_plus_trees_mutated"sv, "windswept_hills"sv, "windswept_forest"sv,
    "windswept_gravelly_hills"sv, "jagged_peaks"sv, "frozen_peaks"sv, "stony_peaks"sv,
    "snowy_slopes"sv, "grove"sv, "meadow"sv, "cherry_grove"sv,
};
constexpr std::string_view SnowBiomes[]{
    "snowy_plains"sv, "ice_plains"sv, "ice_plains_spikes"sv, "ice_spikes"sv, "ice_mountains"sv,
    "snowy_taiga"sv, "cold_taiga"sv, "cold_taiga_hills"sv, "cold_taiga_mutated"sv,
    "snowy_beach"sv, "cold_beach"sv, "grove"sv, "snowy_slopes"sv, "jagged_peaks"sv,
    "frozen_peaks"sv, "frozen_river"sv, "frozen_ocean"sv, "deep_frozen_ocean"sv, "legacy_frozen_ocean"sv,
};
constexpr std::string_view FogBiomes[]{
    "swampland"sv, "swampland_mutated"sv, "swamp"sv, "mangrove_swamp"sv, "dark_forest"sv,
    "roofed_forest"sv, "roofed_forest_mutated"sv, "deep_dark"sv,
};

std::string_view canonical(std::string_view name) {
    if (name.starts_with("minecraft:"sv)) return name.substr(10);
    return name;
}

unsigned families(std::string_view name) {
    const auto id = canonical(name);
    unsigned group = 0;
    if (std::ranges::find(DryBiomes, id) != std::end(DryBiomes)) group |= Dry | Windy;
    if (std::ranges::find(RedBiomes, id) != std::end(RedBiomes)) group |= Dry | Red | Windy;
    if (id == "natures_spirit:drylands" || id == "natures_spirit:wooded_drylands") group |= Dry | Pink | Windy;
    if (std::ranges::find(Mountains, id) != std::end(Mountains)) group |= Windy;
    if (std::ranges::find(SnowBiomes, id) != std::end(SnowBiomes)) group |= Icy;
    if (std::ranges::find(FogBiomes, id) != std::end(FogBiomes)) group |= Dense;
    if (id == "pale_garden") group |= Pale | Dense;
    return group;
}

bool matches(std::string_view selector, std::string_view id, unsigned group) {
    constexpr std::pair<std::string_view, unsigned> tags[]{
        {"#sandstorms", Dry}, {"#badlands", Red}, {"#blizzards", Icy},
        {"#dense_fog", Dense}, {"#black_rain", Pale}, {"#windy", Windy},
    };
    if (selector == "*") return true;
    if (!selector.starts_with('#')) return canonical(selector) == canonical(id);
    const auto tag = std::ranges::find(tags, selector, &std::pair<std::string_view, unsigned>::first);
    return tag != std::end(tags) && (group & tag->second);
}

Color mixColor(Color a, Color b, float t) {
    return {std::lerp(a.r, b.r, t), std::lerp(a.g, b.g, t), std::lerp(a.b, b.b, t)};
}

std::optional<Color> hexColor(std::string_view text) {
    if (text.starts_with('#')) text.remove_prefix(1);
    if (text.size() != 6) return {};
    std::uint32_t result{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, result, 16);
    return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional{rgb(result)} : std::nullopt;
}

bool readRule(Rule& rule, std::string_view key, std::string_view text) {
    if (key == "fog_distance") {
        const auto v = number(text);
        if (!v || *v < 0.01f || *v > 1.f) return false;
        rule.distance = *v;
    } else if (key == "fog_color" || key == "sky_color") {
        const auto v = hexColor(text);
        if (!v) return false;
        (key == "fog_color" ? rule.fog : rule.sky) = *v;
    } else if (key == "archetype") {
        constexpr std::array names{"normal"sv, "sandstorm"sv, "blizzard"sv, "spooky"sv};
        const auto it = std::ranges::find(names, text);
        if (it == names.end()) return false;
        rule.type = static_cast<Archetype>(it - names.begin());
    } else if (key == "storm_sound") {
        constexpr std::array names{"none"sv, "soft"sv, "strong"sv};
        const auto it = std::ranges::find(names, text);
        if (it == names.end()) return false;
        rule.sound = static_cast<Wind>(it - names.begin());
    } else {
        const auto v = boolean(text);
        if (!v) return false;
        if (key == "ambient_wind") rule.windy = *v;
        else if (key == "sandstorm_dust") rule.dust = *v;
        else if (key == "black_rain") rule.blackRain = *v;
        else return false;
    }
    return true;
}

constexpr Setting toggle(std::string_view key, std::string_view title, std::string_view group,
                         std::string_view description, bool Settings::*flag) {
    return {key, title, group, description, Control::Toggle, flag};
}

constexpr Setting slider(std::string_view key, std::string_view title, std::string_view group,
                         std::string_view description, float Settings::*real, float min, float max, float step) {
    return {key, title, group, description, Control::Slider, nullptr, real, nullptr, min, max, step};
}

constexpr Setting Options[]{
    toggle("fog", "Storm fog and sky", "fog", "Enable fog distance changes and storm colors.", &Settings::fog),
    toggle("sand_fog", "Sandstorm fog", "fog", "Orange fog in deserts and red fog in badlands.", &Settings::sandFog),
    toggle("blizzard_fog", "Blizzard fog", "fog", "Reduce visibility during thunder in cold biomes.", &Settings::blizzardFog),
    toggle("dense_fog", "Dense fog", "fog", "Thicken weather fog in swamps, dark forests, Pale Gardens and the Deep Dark.", &Settings::denseFog),
    slider("fog_distance", "Fog distance multiplier", "fog", "If you want more visibility, increase this multiplier.", &Settings::fogDistance, 0.1f, 10.f, 0.1f),
    toggle("black_rain", "Black rain", "particles", "Dark rain and splashes in the Pale Garden.", &Settings::blackRain),
    toggle("wind_particles", "Ambient wind particles", "particles", "Show drifting grains in windy biomes, including during clear weather.", &Settings::windParticles),
    slider("wind_chance", "Ambient wind frequency", "particles", "Adjust how often ambient grains appear.", &Settings::windChance, 0.1f, 10.f, 0.1f),
    slider("wind_size", "Ambient wind particle size", "particles", "Adjust the size of ambient wind grains.", &Settings::windSize, 0.01f, 2.f, 0.05f),
    toggle("dust", "Sandstorm particles", "particles", "Show windblown dust during rain and thunder in dry biomes.", &Settings::dust),
    {"dust_radius", "Sandstorm particle distance", "particles", "The number of blocks around the camera to sample for dust.",
        Control::Integer, nullptr, nullptr, &Settings::dustRadius, 1, 100, 1},
    slider("dust_density", "Sandstorm particle density", "particles", "Adjust how many grains appear during a sandstorm.", &Settings::dustDensity, 0.1f, 10.f, 0.1f),
    slider("dust_size", "Sandstorm dust size", "particles", "Adjust the size of sandstorm grains.", &Settings::dustSize, 0.01f, 2.f, 0.05f),
    toggle("wind_sounds", "Clear weather wind sounds", "audio", "Play occasional breezes in windy biomes during clear weather.", &Settings::windSounds),
    toggle("storm_sounds", "Storm wind sounds", "audio", "Play different wind gusts in light and severe storms.", &Settings::stormSounds),
    slider("volume", "Wind volume", "audio", "Adjust the volume of ambient and storm wind.", &Settings::volume, 0.f, 1.f, 0.01f),
};

}

Profile profile(std::string_view biome, std::span<const Rule> rules) {
    const auto group = families(biome);
    Effects clear;
    clear.windy = group & Windy;
    if (group & Dense) clear.type = Archetype::Dense;
    if (group & Dry) clear.type = Archetype::Sandstorm;
    if (group & Icy) clear.type = Archetype::Blizzard;
    Profile result{clear, clear, clear};
    auto& rain = result[1];
    auto& thunder = result[2];
    if (group & Dense) {
        rain.distance = 128.f / 1024.f;
        thunder.distance = 64.f / 1024.f;
        rain.blackRain = thunder.blackRain = (group & Pale) != 0;
    }
    if (group & Dry) {
        const auto tint = rgb(group & Red ? 0xF75E05 : group & Pink ? 0xE9A481 : 0xFF9C21);
        rain.fog = rain.sky = thunder.fog = thunder.sky = tint;
        rain.distance = 128.f / 1024.f;
        thunder.distance = 64.f / 1024.f;
        rain.sound = Wind::Soft;
        thunder.sound = Wind::Strong;
        rain.dust = thunder.dust = true;
    }
    if (group & Icy) {
        thunder.distance = 128.f / 1024.f;
        thunder.sound = Wind::Soft;
    }
    for (const auto& rule : rules) {
        if (!matches(rule.biome, biome, group)) continue;
        auto& output = result[static_cast<std::size_t>(rule.weather)];
        if (rule.distance) output.distance = *rule.distance;
        if (rule.fog) output.fog = *rule.fog;
        if (rule.sky) output.sky = *rule.sky;
        if (rule.type) output.type = *rule.type;
        if (rule.sound) output.sound = *rule.sound;
        if (rule.windy) output.windy = *rule.windy;
        if (rule.dust) output.dust = *rule.dust;
        if (rule.blackRain) output.blackRain = *rule.blackRain;
    }
    return result;
}

Environment evaluate(Biome biome, float rain, float thunder, const Settings& config, std::span<const Rule> rules) {
    if (!config.enabled || !biome.known) return {};
    const auto weather = profile(biome.name, rules);
    const auto layer = [&](const Effects& e) {
        const bool fog = config.fog
            && (config.sandFog || e.type != Archetype::Sandstorm)
            && (config.blizzardFog || e.type != Archetype::Blizzard)
            && (config.denseFog || e.type != Archetype::Dense);
        Environment env;
        if (fog) {
            env.distance = e.distance < 1.f ? std::clamp(e.distance * config.fogDistance, 0.01f, 1.f) : 1.f;
            env.fog = e.fog;
            env.sky = e.sky;
        }
        env.wind = e.windy ? 1.f : 0.f;
        env.windColor = e.type == Archetype::Sandstorm ? Sandy : e.type == Archetype::Blizzard ? White : Rocky;
        env.blackRain = config.blackRain && e.blackRain && biome.precipitation && !biome.snow ? 1.f : 0.f;
        env.dust = config.dust && e.dust && !biome.precipitation ? 1.f : 0.f;
        env.soft = config.stormSounds && e.sound == Wind::Soft ? 1.f : 0.f;
        env.strong = config.stormSounds && e.sound == Wind::Strong ? 1.f : 0.f;
        return env;
    };
    auto env = mix(layer(weather[0]), layer(weather[1]), std::clamp(rain, 0.f, 1.f));
    return mix(env, layer(weather[2]), std::clamp(thunder, 0.f, 1.f));
}

Environment mix(Environment a, const Environment& b, float t) {
    a.distance = std::lerp(a.distance, b.distance, t);
    a.fog = mixColor(a.fog, b.fog, t);
    a.sky = mixColor(a.sky, b.sky, t);
    a.wind = std::lerp(a.wind, b.wind, t);
    a.dust = std::lerp(a.dust, b.dust, t);
    a.blackRain = std::lerp(a.blackRain, b.blackRain, t);
    a.soft = std::lerp(a.soft, b.soft, t);
    a.strong = std::lerp(a.strong, b.strong, t);
    a.windColor = mixColor(a.windColor, b.windColor, t);
    return a;
}

std::optional<std::vector<Rule>> loadRules(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return std::nullopt;
    std::vector<Rule> rules;
    std::string line;
    while (std::getline(file, line)) {
        auto text = trim(line);
        if (text.empty() || text.starts_with(';') || text.starts_with("//")) continue;
        if (line.size() > 512 || rules.size() >= 256) return std::nullopt;
        if (text.front() == '[' && text.back() == ']') {
            text = trim(text.substr(1, text.size() - 2));
            const auto split = text.find_last_of(" \t");
            if (split == text.npos || trim(text.substr(0, split)).empty()) return std::nullopt;
            constexpr std::array conditions{"clear"sv, "rain"sv, "thunder"sv};
            const auto condition = std::ranges::find(conditions, trim(text.substr(split + 1)));
            if (condition == conditions.end()) return std::nullopt;
            auto& rule = rules.emplace_back();
            rule.biome = trim(text.substr(0, split));
            rule.weather = static_cast<Condition>(condition - conditions.begin());
        } else {
            const auto split = text.find('=');
            if (rules.empty() || split == text.npos || !readRule(rules.back(), trim(text.substr(0, split)), trim(text.substr(split + 1)))) {
                return std::nullopt;
            }
        }
    }
    return file.eof() ? std::optional{std::move(rules)} : std::nullopt;
}

std::string_view trim(std::string_view text) {
    constexpr auto space = " \t\r\n";
    const auto start = text.find_first_not_of(space);
    if (start == text.npos) return {};
    return text.substr(start, text.find_last_not_of(space) - start + 1);
}

std::optional<float> number(std::string_view text) {
    if (text.empty() || text.size() > 24) return {};
    std::string input(text);
    char* end{};
    const float parsed = std::strtof(input.c_str(), &end);
    if (end != input.data() + input.size() || !std::isfinite(parsed)) return {};
    return parsed;
}

std::optional<bool> boolean(std::string_view text) {
    if (text == "true" || text == "1") return true;
    if (text == "false" || text == "0") return false;
    return {};
}

std::span<const Setting> settingList() { return Options; }

bool set(Settings& config, const Setting& option, std::string_view text) {
    if (option.control == Control::Toggle) {
        const auto v = boolean(text);
        if (!v) return false;
        config.*option.flag = *v;
        return true;
    }
    const auto parsed = number(text);
    if (!parsed) return false;
    const float v = std::clamp(*parsed, option.min, option.max);
    if (option.integer) config.*option.integer = static_cast<int>(std::lround(v));
    else config.*option.real = v;
    return true;
}

std::string value(const Settings& config, const Setting& option) {
    if (option.flag) return config.*option.flag ? "true" : "false";
    if (option.integer) return std::to_string(config.*option.integer);
    return std::to_string(config.*option.real);
}

Settings loadSettings(const std::filesystem::path& path) {
    Settings config;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const auto split = line.find('=');
        if (split == line.npos) continue;
        const auto name = trim(std::string_view(line).substr(0, split));
        const auto text = trim(std::string_view(line).substr(split + 1));
        if (name == "enabled") {
            config.enabled = boolean(text).value_or(config.enabled);
        } else {
            const auto row = std::ranges::find(Options, name, &Setting::key);
            if (row != std::end(Options)) set(config, *row, text);
        }
    }
    return config;
}

bool saveSettings(const std::filesystem::path& path, const Settings& config) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto tmp = path;
    tmp += ".tmp";
    std::ofstream file(tmp);
    if (!file) return false;
    file << "enabled=" << (config.enabled ? "true" : "false") << '\n';
    for (const auto& row : Options) file << row.key << '=' << value(config, row) << '\n';
    file.close();
    if (!file) return false;
    std::filesystem::rename(tmp, path, error);
    if (!error) return true;
    std::filesystem::remove(tmp, error);
    return false;
}

}

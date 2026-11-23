#include "Audio.hpp"
#include "Native.hpp"

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace storms {
namespace {

using namespace native;
using Clock = std::chrono::steady_clock;
constexpr char Tag[] = "ImmersiveStorms";
constexpr char BuildLabel[] = "Immersive Storms " STORMS_VERSION;
constexpr std::string_view MenuId = "immersive_storms.weather";
using Priority = pl::memory::HookPriority;

double now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

struct NativeHooks {
    void* preloader{};
    int(*install)(void*, void*, void**, Priority){};
    bool(*uninstall)(void*, void*){};
    std::array<std::pair<void*, void*>, 12> records{};
    std::size_t size{};

    template<class T>
    T symbol(const char* name) const { return reinterpret_cast<T>(dlsym(preloader, name)); }

    bool bind() {
        preloader = dlopen("libpreloader.so", RTLD_NOW | RTLD_NOLOAD);
        if (!preloader) return false;
        install = symbol<decltype(install)>("_ZN2pl6memory4hookEPvS1_PS1_NS0_12HookPriorityE");
        uninstall = symbol<decltype(uninstall)>("_ZN2pl6memory6unhookEPvS1_");
        return install && uninstall;
    }

    template<class Fn>
    bool add(void* target, Fn replacement, Fn& original, Priority priority = Priority::Normal) {
        if (!target || size == records.size()) return false;
        auto* detour = reinterpret_cast<void*>(replacement);
        const int status = install(target, detour, reinterpret_cast<void**>(&original), priority);
        if (status == 0) {
            records[size++] = {target, detour};
            if (original) return true;
        }
        __android_log_print(ANDROID_LOG_ERROR, Tag, "Hook failed at %p (%d)", target, status);
        return false;
    }

    template<class Fn>
    bool at(std::uintptr_t rva, Fn replacement, Fn& original, Priority priority = Priority::Normal) {
        return add(addr<void*>(rva), replacement, original, priority);
    }

    bool removeFrom(std::size_t first) {
        for (std::size_t i = size; i > first; --i) {
            if (!uninstall(records[i - 1].first, records[i - 1].second)) return false;
            --size;
        }
        return true;
    }

    void close() {
        if (preloader) dlclose(preloader);
        preloader = nullptr;
    }
};

struct Frame {
    void* level{};
    void* player{};
    void* renderer{};
    void* dimension{};
    void* weatherRenderer{};
    Vec3 camera;
    Environment env;
    float rain{}, thunder{}, light{};
    double time{};
};

enum class Installation { Waiting, Ready, Unsupported, Failed };
std::atomic<Installation> installation{Installation::Waiting};
std::atomic<bool> running{false};
std::mutex installMutex;
std::mutex sceneMutex;
NativeHooks hooks;
Frame frame;
Particles particles;
Audio audio;
Settings settings;
std::vector<Rule> rules;
std::filesystem::path configPath, rulesPath;
std::filesystem::file_time_type lastRulesChange{};
double nextRefresh{};
std::string owner;
bool menuRegistered{};

using WeatherEmitter = void(*)(void*, void*);
using SpawnParticle = void*(*)(void*, unsigned, const Vec3&, const Vec3&, int, const void*, bool);
using PlayerTick = void(*)(void*);
using ClientUpdate = bool(*)(void*, bool, bool);
using SetupFog = void(*)(void*, void*, float);
using SkyColor = Rgba(*)(void*, const void*, bool, float);
using ExtractWeather = WeatherDrawData(*)(void*, void*);
using RenderLevel = void(*)(void*, void*, void*);
using Open = void*(*)(const char*, int);
using OpenExt = void*(*)(const char*, int, const android_dlextinfo*);
using RegisterMenu = bool(*)(const pl::modmenu::ModuleInfo&);
using RemoveMenu = void(*)(std::string_view);
using SetSchema = bool(*)(std::string_view, std::string_view);

PlayerTick playerTickOriginal{};
ClientUpdate updateOriginal{};
SetupFog fogOriginal{};
SkyColor skyOriginal{};
ExtractWeather extractOriginal{};
WeatherEmitter emitterOriginal{};
SpawnParticle particleOriginal{};
RenderLevel renderOriginal{};
Open openOriginal{};
OpenExt openExtOriginal{};
RemoveMenu removeMenu{};
SetSchema setSchema{};

void tryInstall();

bool active() {
    return running.load(std::memory_order_acquire)
        && installation.load(std::memory_order_acquire) == Installation::Ready;
}

void updateRules(double time) {
    if (time < nextRefresh) return;
    nextRefresh = time + 1.;
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(rulesPath, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            rules.clear();
            lastRulesChange = {};
        }
        return;
    }
    if (lastRulesChange == modified) return;
    lastRulesChange = modified;
    if (auto parsed = loadRules(rulesPath)) {
        rules = std::move(*parsed);
    } else {
        __android_log_print(ANDROID_LOG_WARN, Tag, "Could not read biomes.cfg, keeping previous biome settings");
    }
}

void clearWorld() {
    frame = {};
    particles.clear();
    audio.quiet(now());
}

void tick(void* player) {
    playerTickOriginal(player);
    if (!active()) return;
    const auto w = world(player);
    if (!w) return;

    std::scoped_lock lock(sceneMutex);
    if (!settings.enabled || !running.load(std::memory_order_relaxed)) return;
    const double time = now();
    updateRules(time);
    const bool changed = frame.level != w->level || frame.dimension != w->dimension || frame.renderer != w->playerRenderer
        || time - frame.time > 0.6 || std::hypot(w->camera.x - frame.camera.x, w->camera.z - frame.camera.z) > 40.f;
    if (changed) {
        particles.clear();
        audio.quiet(time);
    } else if (time - frame.time < 0.012) {
        return;
    }

    auto center = biome(w->region, w->camera);
    if (!center.known) {
        clearWorld();
        return;
    }
    const auto evaluateAt = [&](Biome b) { return evaluate(b, w->rain, w->thunder, settings, rules); };
    Environment env = evaluateAt(center);
    int weight = 8;
    constexpr Vec3 Around[]{{8, 0, 0}, {-8, 0, 0}, {0, 0, 8}, {0, 0, -8}};
    for (auto offset : Around) {
        auto neighbor = biome(w->region, w->camera + offset);
        if (!neighbor.known) continue;
        env = mix(env, evaluateAt(neighbor), 1.f / float(++weight));
    }
    if (!changed) env = mix(frame.env, env, 0.42f);
    if (!settings.blackRain) env.blackRain = 0.f;
    if (!settings.fog) {
        env.fog = env.sky = White;
        env.distance = 1.f;
    }
    if (!settings.stormSounds) env.soft = env.strong = 0.f;

    const auto surface = column(w->region, w->camera);
    const bool covered = !surface || surface->surface > w->camera.y + 0.75f || w->medium;
    const Sampler sample{w->region, [](void* region, Vec3 at) { return column(region, at); }};
    if (!w->medium) particles.tick(w->camera, env, w->rain, w->thunder, settings, rules, sample);
    else particles.clear();
    if (w->medium) audio.quiet(time);
    else audio.update(env, settings.volume, w->rain, covered, settings.windSounds, time);

    frame = {w->level, player, w->playerRenderer, w->dimension, w->weatherRenderer, w->camera,
             env, w->rain, w->thunder, w->light, time};
}

bool update(void* client, bool a, bool b) {
    const bool result = updateOriginal(client, a, b);
    if (!active()) return result;

    std::scoped_lock lock(sceneMutex);
    if (!settings.enabled || !frame.player) return result;
    auto* player = localPlayer(client);
    if (player != frame.player || field<void*>(player, offset::ActorLevel) != frame.level
        || field<void*>(player, offset::ActorDimension) != frame.dimension) {
        clearWorld();
    } else if (now() - frame.time > 0.4) {
        audio.quiet(now());
    }
    return result;
}

void fog(void* renderer, void* context, float fraction) {
    fogOriginal(renderer, context, fraction);
    if (!active()) return;
    Environment env;
    {
        std::scoped_lock lock(sceneMutex);
        if (!settings.enabled || !settings.fog || renderer != frame.renderer) return;
        env = frame.env;
    }
    if (field<bool>(renderer, offset::RendererMedium) || (env.distance > 0.999f && env.fog == White)) return;
    auto& tint = field<Rgba>(renderer, offset::FogColor);
    auto& start = field<float>(renderer, offset::FogStart);
    auto& end = field<float>(renderer, offset::FogEnd);
    tint.r *= env.fog.r;
    tint.g *= env.fog.g;
    tint.b *= env.fog.b;
    if (std::isfinite(start) && std::isfinite(end) && end > 0.0f && end < 16384.f && env.distance < 0.999f) {
        end = std::max(3.f, end * env.distance);
        start = std::min(start * env.distance, end * 0.75f);
    }
}

Rgba sky(void* dimension, const void* properties, bool custom, float fraction) {
    auto color = skyOriginal(dimension, properties, custom, fraction);
    if (!active()) return color;
    std::scoped_lock lock(sceneMutex);
    if (!settings.enabled || !settings.fog || dimension != frame.dimension) return color;
    color.r *= frame.env.sky.r;
    color.g *= frame.env.sky.g;
    color.b *= frame.env.sky.b;
    return color;
}

WeatherDrawData extract(void* renderer, void* arguments) {
    auto result = extractOriginal(renderer, arguments);
    if (!active()) return result;
    std::scoped_lock lock(sceneMutex);
    if (settings.enabled && settings.blackRain && renderer && renderer == frame.weatherRenderer
        && frame.env.blackRain > 0.f && std::isfinite(result.rain)
        && particleMaterial(field<void*>(renderer, offset::WeatherOwner))) {
        result.rain *= 1.f - frame.env.blackRain;
    }
    return result;
}

thread_local void* weatherRegion{};

void emitWeather(void* output, void* renderer) {
    const auto previous = weatherRegion;
    if (active()) {
        std::scoped_lock lock(sceneMutex);
        if (settings.enabled && settings.blackRain && renderer == frame.renderer
            && frame.env.blackRain > 0.10f && particleMaterial(renderer)) {
            weatherRegion = field<void*>(renderer, offset::RendererRegion);
        }
    }
    emitterOriginal(output, renderer);
    weatherRegion = previous;
}

void* spawnParticle(void* system, unsigned type, const Vec3& position, const Vec3& velocity,
                    int data, const void* tag, bool always) {
    if (type == 0x27 && weatherRegion) {
        const auto b = biome(weatherRegion, position);
        std::scoped_lock lock(sceneMutex);
        if (evaluate(b, frame.rain, frame.thunder, settings, rules).blackRain > 0.35f) return nullptr;
    }
    return particleOriginal(system, type, position, velocity, data, tag, always);
}

void renderLevel(void* level, void* screen, void* info) {
    renderOriginal(level, screen, info);
    if (!active() || !screen || !level) return;
    auto* renderer = field<void*>(level, offset::PlayerRenderer);
    if (!renderer || field<bool>(renderer, offset::RendererMedium)) return;

    thread_local std::vector<Particle> visible;
    thread_local std::vector<Vertex> mesh;
    Frame current;
    {
        std::scoped_lock lock(sceneMutex);
        if (!settings.enabled || frame.renderer != renderer || particles.view().empty()) return;
        current = frame;
        visible.assign(particles.view().begin(), particles.view().end());
    }
    const auto camera = field<Vec3>(renderer, offset::RendererCamera);
    Vec3 right, up;
    if (!positionValid(camera) || std::hypot(camera.x - current.camera.x, camera.z - current.camera.z) > 40.f
        || !viewVectors(renderer, right, up)) return;
    const float fraction = float(std::clamp((now() - current.time) * 20., 0., 1.));
    buildMesh(mesh, visible, camera, right, up, fraction, current.light);
    draw(screen, renderer, mesh);
}

void configure(std::string_view key, std::string_view text) {
    std::scoped_lock lock(sceneMutex);
    const auto options = settingList();
    const auto row = std::ranges::find(options, key, &Setting::key);
    if (row == options.end()) return;
    const auto before = value(settings, *row);
    if (!set(settings, *row, text) || before == value(settings, *row)) return;
    if (row->control == Control::Toggle) {
        frame.env = {};
        particles.clear();
        audio.quiet(now());
    }
    if (!saveSettings(configPath, settings)) __android_log_print(ANDROID_LOG_WARN, Tag, "Could not save settings");
}

void registerMenu() {
    using namespace pl::modmenu;
    const auto add = hooks.symbol<RegisterMenu>("_ZN2pl7modmenu14registerModuleERKNS0_10ModuleInfoE");
    removeMenu = hooks.symbol<RemoveMenu>("_ZN2pl7modmenu16unregisterModuleENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE");
    setSchema = hooks.symbol<SetSchema>("_ZN2pl7modmenu19setConfigSchemaJsonENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEES5_");
    if (!add || !removeMenu) return;
    ModuleInfo module;
    module.moduleId = MenuId;
    module.displayName = "Immersive Storms";
    module.description = "Biome-specific sandstorms, blizzards, black rain, fog and wind.";
    module.modId = owner;
    module.defaultEnabled = settings.enabled;
    module.hideInHudEditor = true;
    module.onConfigChanged = [](auto, auto key, auto text) { configure(key, text); };
    module.onToggle = [](auto, bool enabled) {
        std::scoped_lock lock(sceneMutex);
        settings.enabled = enabled;
        if (!enabled) clearWorld();
        if (!saveSettings(configPath, settings)) __android_log_print(ANDROID_LOG_WARN, Tag, "Could not save module state");
    };
    ConfigSchemaBuilder schema;
    schema.defaultCategory("fog").category("fog", "Fog and sky").category("particles", "Particles").category("audio", "Audio");
    for (const auto& row : settingList()) {
        const auto type = row.control == Control::Toggle ? ConfigType::Toggle
            : row.control == Control::Integer ? ConfigType::SliderInt : ConfigType::SliderFloat;
        const auto format = [&](float n) { return row.integer ? std::to_string(int(n)) : std::to_string(n); };
        module.configs.push_back({std::string(row.key), std::string(row.title), type,
            value(settings, row), format(row.min), format(row.max), {}});
        ConfigNodeV2 node;
        node.id = node.key = row.key;
        node.title = row.title;
        node.description = row.description;
        node.category = row.category;
        node.defaultValue = value(Settings{}, row);
        node.type = type == ConfigType::Toggle ? ConfigControlTypeV2::Toggle
            : type == ConfigType::SliderInt ? ConfigControlTypeV2::SliderInt : ConfigControlTypeV2::SliderFloat;
        if (type != ConfigType::Toggle) {
            node.minValue = format(row.min);
            node.maxValue = format(row.max);
            node.step = format(row.step);
        }
        schema.node(std::move(node));
    }
    menuRegistered = add(module);
    if (menuRegistered && setSchema) setSchema(MenuId, schema.toJson());
}

void* openGame(const char* name, int flags) {
    void* result = openOriginal(name, flags);
    if (result && name && std::strstr(name, "libminecraftpe.so")) tryInstall();
    return result;
}

void* openGameExt(const char* name, int flags, const android_dlextinfo* info) {
    void* result = openExtOriginal(name, flags, info);
    if (result && name && std::strstr(name, "libminecraftpe.so")) tryInstall();
    return result;
}

void tryInstall() {
    if (installation.load(std::memory_order_acquire) != Installation::Waiting || !running.load(std::memory_order_acquire)) return;
    std::scoped_lock lock(installMutex);
    if (installation.load(std::memory_order_relaxed) != Installation::Waiting) return;
    const auto build = inspect();
    if (build == Build::Missing) return;
    if (build != Build::Supported) {
        installation.store(Installation::Unsupported, std::memory_order_release);
        __android_log_print(ANDROID_LOG_WARN, Tag, "This release requires Minecraft 1.26.66.01_RC0 arm64-v8a");
        return;
    }
    const auto first = hooks.size;
    const bool ready = hooks.at(rva::ClientUpdate, update, updateOriginal)
        && hooks.at(rva::PlayerTick, tick, playerTickOriginal, Priority::Lowest)
        && hooks.at(rva::SetupFog, fog, fogOriginal, Priority::Low)
        && hooks.at(rva::SkyColor, sky, skyOriginal)
        && hooks.at(rva::ExtractWeather, extract, extractOriginal)
        && hooks.at(rva::SpawnWeather, emitWeather, emitterOriginal)
        && hooks.at(rva::ClientParticle, spawnParticle, particleOriginal)
        && hooks.at(rva::RenderLevel, renderLevel, renderOriginal, Priority::Low);
    if (!ready) {
        running.store(false, std::memory_order_release);
        hooks.removeFrom(first);
        installation.store(Installation::Failed, std::memory_order_release);
        return;
    }
    installation.store(Installation::Ready, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, Tag, "%s loaded for Minecraft 1.26.66.01_RC0", BuildLabel);
}

}
}

extern "C" {

struct PLModInfo {
    std::uint32_t size;
    const char* modId;
    const char* displayName;
    const char* author;
    const char* version;
    const char* entryPath;
    const char* entryName;
    const char* libraryPath;
    const char* iconPath;
    const char* manifestPath;
    const char* rootPath;
};

#define STORMS_EXPORT __attribute__((visibility("default")))

STORMS_EXPORT bool PLMod_Load(JavaVM* vm, const PLModInfo* info) {
    using namespace storms;
    if (!vm || !info || info->size < sizeof(PLModInfo) || !info->modId || !info->rootPath) return false;
    if (!hooks.bind()) {
        hooks.close();
        return false;
    }
    owner = info->modId;
    const auto directory = std::filesystem::path(info->rootPath) / "config";
    configPath = directory / "immersive_storms.properties";
    rulesPath = directory / "biomes.cfg";
    settings = loadSettings(configPath);
    updateRules(now());
    registerMenu();
    return true;
}

STORMS_EXPORT bool PLMod_Enable() {
    using namespace storms;
    const auto state = installation.load(std::memory_order_acquire);
    if (state == Installation::Unsupported || state == Installation::Failed) return false;
    running.store(true, std::memory_order_release);
    if (state == Installation::Waiting && native::inspect() == native::Build::Missing) {
        std::scoped_lock lock(installMutex);
        if (hooks.size == 0) {
            const bool regular = hooks.add(dlsym(RTLD_DEFAULT, "dlopen"), openGame, openOriginal);
            const bool android = hooks.add(dlsym(RTLD_DEFAULT, "android_dlopen_ext"), openGameExt, openExtOriginal);
            if (!regular && !android) {
                running.store(false, std::memory_order_release);
                return false;
            }
        }
    }
    tryInstall();
    const auto after = installation.load(std::memory_order_acquire);
    return after == Installation::Waiting || after == Installation::Ready;
}

STORMS_EXPORT bool PLMod_Disable() {
    using namespace storms;
    running.store(false, std::memory_order_release);
    std::scoped_lock lock(sceneMutex);
    clearWorld();
    audio.close();
    return true;
}

STORMS_EXPORT bool PLMod_Unload() {
    using namespace storms;
    PLMod_Disable();
    std::scoped_lock lock(installMutex);
    if (!hooks.removeFrom(0)) return false;
    if (menuRegistered) removeMenu(MenuId);
    menuRegistered = false;
    hooks.close();
    installation.store(Installation::Waiting, std::memory_order_release);
    return true;
}

}

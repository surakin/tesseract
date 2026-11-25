#include "EngineScene.h"

#include "views/world/WorldAssetLoader.h"

#include <engine/engine.h>
#include <engine/engine_abi_version.h>
// engine_loader.h includes <windows.h>; keep its macros (near, small, min/max, ...) out of the unity chunk.
#include "tesseract/win32/windows_enable.h"
#include <engine/engine_loader.h>
#include "tesseract/win32/windows_disable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tesseract::views::world
{

namespace
{

// The running executable's path; empty when it cannot be found.
std::filesystem::path executable_path()
{
    std::error_code ec;
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
    {
        return {};
    }
    auto exe = std::filesystem::weakly_canonical(buf.c_str(), ec);
#elif defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size())
    {
        return {};
    }
    buf.resize(n);
    auto exe = std::filesystem::path(buf);
#else
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    return ec ? std::filesystem::path{} : exe;
}

// Where an installed Tesseract keeps the engine library and the world assets, relative to the
// executable: macOS <app>.app/Contents/{Frameworks,Resources/worlds}; Windows next to the exe;
// Linux <prefix>/lib/tesseract and <prefix>/share/tesseract/worlds (cmake/Engine.cmake installs
// them there). Empty when there is no executable path.
std::filesystem::path installed_library()
{
    const auto exe = executable_path();
    if (exe.empty())
    {
        return {};
    }
#if defined(__APPLE__)
    return exe.parent_path().parent_path() / "Frameworks" / "libengine.1.dylib";
#elif defined(_WIN32)
    return exe.parent_path() / "engine.dll";
#else
    return exe.parent_path().parent_path() / "lib" / "tesseract" / "libengine.so.1";
#endif
}

std::filesystem::path installed_assets()
{
    const auto exe = executable_path();
    if (exe.empty())
    {
        return {};
    }
#if defined(__APPLE__)
    return exe.parent_path().parent_path() / "Resources" / "worlds";
#elif defined(_WIN32)
    return exe.parent_path() / "worlds";
#else
    return exe.parent_path().parent_path() / "share" / "tesseract" / "worlds";
#endif
}

} // namespace

std::string EngineScene::default_library_path()
{
    std::error_code ec;
    const auto installed = installed_library();
    if (!installed.empty() && std::filesystem::exists(installed, ec))
    {
        return installed.string();
    }
    return TESSERACT_ENGINE_LIBRARY_PATH; // a build tree runs the library it built
}

std::string EngineScene::default_assets_dir()
{
    std::error_code ec;
    const auto installed = installed_assets();
    if (!installed.empty() && std::filesystem::is_directory(installed, ec))
    {
        return installed.string();
    }
    return TESSERACT_WORLD_ASSETS_DIR;
}

namespace
{

namespace eh = engine_host;

constexpr std::uint32_t kScreenW = 64, kScreenH = 36;
constexpr auto kFramePeriod = std::chrono::microseconds(16667); // 60 Hz

std::vector<std::uint8_t> flat_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::vector<std::uint8_t> px(kScreenW * kScreenH * 4);
    for (std::size_t i = 0; i < px.size(); i += 4)
    {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = 255;
    }
    return px;
}

bool read_file(const std::string& path, std::vector<std::uint8_t>* out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out->empty();
}

eh::Status make_texture(const eh::Engine& engine, const char* name, eh::Texture* out)
{
    auto info = eh::Init<engine_texture_info>();
    info.flags = ENGINE_TEXTURE_SRGB;
    info.width = kScreenW;
    info.height = kScreenH;
    info.name = eh::Str(name);
    return engine.CreateTexture(info, out);
}

eh::Status upload(const eh::Texture& tex, const std::vector<std::uint8_t>& pixels, std::uint32_t w,
                  std::uint32_t update_flags = 0)
{
    auto info = eh::Init<engine_update_info>();
    info.flags = update_flags;
    return tex.Update(pixels.data(), static_cast<std::size_t>(w) * 4, info);
}

// What the frame thread keeps of one spawned robot. Everything about how it behaves is the robot
// script's (assets/worlds/robot.lua); the host keeps the entities and the picture's texture.
struct Robot
{
    std::uint64_t root = 0;
    std::vector<std::uint64_t> keys; // every entity of the spawn, the root first
    eh::Texture texture;             // the picture its script shows
    std::uint32_t tex_w = kScreenW, tex_h = kScreenH;
    std::string prefix; // the spawn's name prefix: its entities are called "<prefix>/<name>"
};

// A list of numbers as one value pool root (an ARRAY).
struct NumberArray
{
    std::vector<engine_value> items;
    engine_value_pool pool{};
    explicit NumberArray(std::initializer_list<double> numbers)
    {
        engine_value array{};
        array.tag = ENGINE_VALUE_ARRAY;
        array.count = static_cast<std::uint32_t>(numbers.size());
        array.i = 1;
        items.push_back(array);
        for (const double n : numbers)
        {
            engine_value v{};
            v.tag = ENGINE_VALUE_NUMBER;
            v.f = n;
            items.push_back(v);
        }
        pool.struct_size = sizeof(pool);
        pool.root_count = 1;
        pool.item_count = pool.item_capacity = items.size();
        pool.items = items.data();
    }
};

// An event payload as a value pool: the root, then each container's children after it.
struct ValuePool
{
    std::vector<engine_value> items;
    std::vector<std::uint8_t> bytes;
    engine_value_pool pool{};
    explicit ValuePool(const EventValue& root)
    {
        items.emplace_back();
        std::vector<std::pair<const EventValue*, std::size_t>> queue{{&root, 0}};
        for (std::size_t q = 0; q < queue.size(); ++q)
        {
            const EventValue* v = queue[q].first;
            engine_value out{};
            switch (v->kind)
            {
            case EventValue::Kind::Empty:
                out.tag = ENGINE_VALUE_NIL;
                break;
            case EventValue::Kind::Number:
                out.tag = ENGINE_VALUE_NUMBER;
                out.f = v->number;
                break;
            case EventValue::Kind::String:
            case EventValue::Kind::Bytes:
                out.tag =
                    v->kind == EventValue::Kind::String ? ENGINE_VALUE_STRING : ENGINE_VALUE_BYTES;
                out.i = static_cast<std::int64_t>(bytes.size());
                out.count = static_cast<std::uint32_t>(v->text.size());
                bytes.insert(bytes.end(), v->text.begin(), v->text.end());
                break;
            case EventValue::Kind::Array:
                out.tag = ENGINE_VALUE_ARRAY;
                out.count = static_cast<std::uint32_t>(v->items.size());
                out.i = static_cast<std::int64_t>(items.size());
                for (const EventValue& child : v->items)
                {
                    queue.emplace_back(&child, items.size());
                    items.emplace_back();
                }
                break;
            }
            items[queue[q].second] = out;
        }
        pool.struct_size = sizeof(pool);
        pool.root_count = 1;
        pool.item_count = pool.item_capacity = items.size();
        pool.items = items.data();
        pool.byte_count = pool.byte_capacity = bytes.size();
        pool.bytes = bytes.data();
    }
    ValuePool(const ValuePool&) = delete;
};

// A script's params: a MAP root, then each key (a STRING) and its value, a number or a string.
struct ParamMap
{
    std::vector<engine_value> items;
    std::vector<std::uint8_t> bytes;
    engine_value_pool pool{};
    void add_text_(const std::string& text, engine_value* out)
    {
        out->tag = ENGINE_VALUE_STRING;
        out->i = static_cast<std::int64_t>(bytes.size());
        out->count = static_cast<std::uint32_t>(text.size());
        bytes.insert(bytes.end(), text.begin(), text.end());
    }
    explicit ParamMap(const std::vector<std::pair<std::string, ScriptSpec::Param>>& entries)
    {
        engine_value map{};
        map.tag = ENGINE_VALUE_MAP;
        map.count = static_cast<std::uint32_t>(entries.size());
        map.i = 1;
        items.push_back(map);
        for (const auto& [key, value] : entries)
        {
            engine_value k{};
            add_text_(key, &k);
            items.push_back(k);
            engine_value v{};
            if (const double* number = std::get_if<double>(&value))
            {
                v.tag = ENGINE_VALUE_NUMBER;
                v.f = *number;
            }
            else
            {
                add_text_(std::get<std::string>(value), &v);
            }
            items.push_back(v);
        }
        pool.struct_size = sizeof(pool);
        pool.root_count = 1;
        pool.item_count = pool.item_capacity = items.size();
        pool.items = items.data();
        pool.byte_count = pool.byte_capacity = bytes.size();
        pool.bytes = bytes.data();
    }
    ParamMap(const ParamMap&) = delete;
};

// A payload the scripts emitted, as a tree (nested at most 8 deep, 4096 items in a list).
EventValue decode_value(const engine_value_pool& p, std::size_t at, int depth)
{
    EventValue out;
    if (at >= p.item_count || depth > 8)
    {
        return out;
    }
    const engine_value& v = p.items[at];
    switch (v.tag)
    {
    case ENGINE_VALUE_BOOL:
    case ENGINE_VALUE_INT:
        out = EventValue::of(static_cast<double>(v.i));
        break;
    case ENGINE_VALUE_NUMBER:
        out = EventValue::of(v.f);
        break;
    case ENGINE_VALUE_STRING:
    case ENGINE_VALUE_BYTES:
        if (v.i >= 0 && static_cast<std::size_t>(v.i) + v.count <= p.byte_count)
        {
            out = EventValue::bytes(p.bytes + v.i, v.count);
            if (v.tag == ENGINE_VALUE_STRING)
            {
                out.kind = EventValue::Kind::String;
            }
        }
        break;
    case ENGINE_VALUE_ARRAY:
        out.kind = EventValue::Kind::Array;
        for (std::uint32_t k = 0; k < v.count && k < 4096; ++k)
        {
            out.items.push_back(decode_value(p, static_cast<std::size_t>(v.i) + k, depth + 1));
        }
        break;
    default:
        break;
    }
    return out;
}

// Where the events the app listens to go; it outlives the world they are subscribed on.
struct EventSink
{
    std::function<void(std::string, EventValue)> store;
};

void on_script_event(void* user, engine_string name, std::uint64_t,
                     const engine_value_pool* payload)
{
    EventValue value;
    if (payload != nullptr && payload->root_count >= 1 && payload->item_count >= 1)
    {
        value = decode_value(*payload, 0, 0);
    }
    static_cast<EventSink*>(user)->store(std::string(name.data, name.length), std::move(value));
}

} // namespace

EngineScene::EngineScene() = default;

EngineScene::~EngineScene()
{
    stop();
}

void EngineScene::start(Options options)
{
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::Stopped && state_ != State::Failed)
        {
            return;
        }
    }
    if (thread_.joinable())
    {
        thread_.join(); // a failed run's thread has already finished
    }
    {
        std::lock_guard lock(mutex_);
        state_ = State::Starting;
        asset_loads_.clear();
        failure_ = Failure::None;
        detail_.clear();
        stop_requested_ = false;
        resize_w_ = resize_h_ = 0;
        robots_.clear();
        inbox_.clear();
        outbox_.clear();
        latest_.clear();
        screens_.clear();
        joint_matrices_.clear();
        picture_ = Picture{};
    }
    thread_ = std::thread(
        [this, options = std::move(options)]() mutable
        {
            run_(std::move(options));
        });
}

void EngineScene::stop()
{
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
    std::lock_guard lock(mutex_);
    state_ = State::Stopped;
    failure_ = Failure::None;
}

EngineScene::State EngineScene::state() const
{
    std::lock_guard lock(mutex_);
    return state_;
}

EngineScene::Failure EngineScene::failure() const
{
    std::lock_guard lock(mutex_);
    return failure_;
}

std::vector<AssetLoadReport> EngineScene::asset_loads() const
{
    std::lock_guard lock(mutex_);
    return asset_loads_;
}

std::string EngineScene::detail() const
{
    std::lock_guard lock(mutex_);
    return detail_;
}

void EngineScene::resize(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return;
    }
    std::lock_guard lock(mutex_);
    resize_w_ = width;
    resize_h_ = height;
}

void EngineScene::set_robots(std::vector<std::string> ids)
{
    std::lock_guard lock(mutex_);
    robots_ = std::move(ids);
}

void EngineScene::post_event(std::string name, EventValue value)
{
    std::lock_guard lock(mutex_);
    if (inbox_.size() >= 4096)
    {
        inbox_.erase(inbox_.begin()); // a stalled scene does not grow without bound
    }
    inbox_.push_back({std::move(name), std::move(value)});
}

std::vector<ScriptEvent> EngineScene::take_events()
{
    std::lock_guard lock(mutex_);
    return std::exchange(outbox_, {});
}

std::optional<EventValue> EngineScene::latest_event(const std::string& name) const
{
    std::lock_guard lock(mutex_);
    const auto it = latest_.find(name);
    if (it == latest_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void EngineScene::watch_robot_joint(const std::string& node, const std::string& joint)
{
    std::lock_guard lock(mutex_);
    watched_joint_ = std::make_pair(node, joint);
}

std::optional<std::array<float, 16>> EngineScene::robot_joint_matrix(const std::string& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = joint_matrices_.find(id);
    if (it == joint_matrices_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void EngineScene::set_robot_screen_bgra(const std::string& id,
                                        std::shared_ptr<const std::vector<std::uint8_t>> bgra,
                                        std::uint32_t width, std::uint32_t height)
{
    if (!bgra || width == 0 || height == 0 ||
        bgra->size() < static_cast<std::size_t>(width) * height * 4)
    {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (std::find(robots_.begin(), robots_.end(), id) == robots_.end())
        {
            return;
        }
    }
    // The frame goes to the engine as it is: it takes BGRA pixels itself (video is opaque).
    PendingScreen screen;
    screen.pixels = std::move(bgra);
    screen.update_flags = ENGINE_UPDATE_BGRA;
    screen.w = width;
    screen.h = height;
    std::lock_guard lock(mutex_);
    screens_[id] = std::move(screen);
}

void EngineScene::clear_robot_screen(const std::string& id)
{
    std::lock_guard lock(mutex_);
    if (std::find(robots_.begin(), robots_.end(), id) == robots_.end())
    {
        return;
    }
    screens_[id] =
        PendingScreen{std::make_shared<const std::vector<std::uint8_t>>(flat_rgba(20, 20, 24)), 0,
                      kScreenW, kScreenH};
}

Picture EngineScene::latest_picture() const
{
    std::lock_guard lock(mutex_);
    return picture_;
}

bool EngineScene::stopping_() const
{
    std::lock_guard lock(mutex_);
    return stop_requested_;
}

void EngineScene::fail_(Failure failure, std::string detail)
{
    std::fprintf(stderr, "[world] 3D scene failed (%d): %s\n", static_cast<int>(failure),
                 detail.c_str());
    std::lock_guard lock(mutex_);
    state_ = State::Failed;
    failure_ = failure;
    detail_ = std::move(detail);
}

EngineScene::Failure EngineScene::probe(const Options& options)
{
    namespace eh = engine_host;
    engine_loader loader{};
    if (engine_load(options.library_path.c_str(), ENGINE_ABI_VERSION_MAJOR,
                    ENGINE_ABI_VERSION_MINOR, &loader) != ENGINE_OK)
    {
        return Failure::LoadFailed;
    }
    Failure result = Failure::None;
    {
        // Destroyed in reverse: the surface, then the engine, before the library goes.
        eh::Engine engine;
        eh::Surface surface;
        auto create = eh::Init<engine_create_info>();
        create.flags = ENGINE_CREATE_LOG_SUPPRESS_CONSOLE;
        auto about = eh::Init<engine_info>();
        eh::Status made = eh::Engine::Create(loader.api, create, &engine);
        if (made.Ok())
        {
            made = engine.Info(&about);
        }
        if (!made.Ok())
        {
            result = Failure::SceneFailed;
        }
        else if ((about.caps & ENGINE_CAP_SURFACE_OFFSCREEN) == 0 ||
                 (about.caps & ENGINE_CAP_IMPORT_GLTF) == 0 ||
                 (about.caps & ENGINE_CAP_SCRIPT) == 0)
        {
            result = Failure::NoOffscreen;
        }
        else
        {
            auto sinfo = eh::Init<engine_surface_info>();
            sinfo.kind = ENGINE_SURFACE_OFFSCREEN;
            sinfo.width = 16;
            sinfo.height = 16;
            sinfo.scale = 1.0f;
            if (!engine.CreateSurface(sinfo, &surface))
            {
                result = Failure::NoDevice;
            }
        }
    }
    loader.api->thread_detach();
    (void)engine_unload(&loader);
    return result;
}

void EngineScene::run_(Options options)
{
    engine_loader loader{};
    if (engine_load(options.library_path.c_str(), ENGINE_ABI_VERSION_MAJOR,
                    ENGINE_ABI_VERSION_MINOR, &loader) != ENGINE_OK)
    {
        fail_(Failure::LoadFailed, loader.message);
        return;
    }
    if (!stopping_())
    {
        run_engine_(loader.api, options); // every engine object is gone when this returns
    }
    loader.api->thread_detach();
    (void)engine_unload(&loader);
}

void EngineScene::run_engine_(const engine_api* api, const Options& options)
{
    // Declared in this order, destroyed in reverse: textures, surface, world, then the engine.
    // The sink goes before them all: the world's event subscription points at it.
    EventSink event_sink{[this](std::string name, EventValue value)
                         {
                             std::lock_guard lock(mutex_);
                             latest_[name] = value;
                             if (outbox_.size() >= 4096)
                             {
                                 outbox_.erase(outbox_.begin());
                             }
                             outbox_.push_back({std::move(name), std::move(value)});
                         }};
    eh::Engine engine;
    eh::World world;
    eh::Surface surface;
    // The robots own their textures, so this goes last: destroyed first, before the surface and world.
    std::map<std::string, Robot> robots;

    auto fail_with_error = [&](Failure f)
    {
        eh::Buffer message;
        (void)eh::LastError(api, &message);
        fail_(f, message.Data() != nullptr ? std::string(message.Data(), message.Size())
                                           : std::string());
    };
    auto step = [&](eh::Status status)
    {
        if (!status)
        {
            fail_with_error(Failure::SceneFailed);
        }
        return status.Ok();
    };

    // World scripts come from other people: run them budgeted (no JIT, instruction and time limits),
    // whatever the engine's default becomes.
    const engine_setting_entry settings[] = {
        {eh::Str("script.budget"), eh::Str("budgeted")},
    };
    auto create = eh::Init<engine_create_info>();
    create.flags = ENGINE_CREATE_LOG_SUPPRESS_CONSOLE;
    create.settings = settings;
    create.setting_count = std::size(settings);
    auto about = eh::Init<engine_info>();
    eh::Status made = eh::Engine::Create(api, create, &engine);
    if (made.Ok())
    {
        made = engine.Info(&about);
    }
    if (!made.Ok())
    {
        eh::Buffer message;
        (void)eh::LastError(api, &message);
        const engine_string name = made.Name(api);
        fail_(Failure::SceneFailed,
              std::string(name.data, name.length) + ": " +
                  (message.Data() != nullptr ? std::string(message.Data(), message.Size())
                                             : std::string()));
        return;
    }
    if ((about.caps & ENGINE_CAP_SURFACE_OFFSCREEN) == 0 ||
        (about.caps & ENGINE_CAP_IMPORT_GLTF) == 0 || (about.caps & ENGINE_CAP_SCRIPT) == 0)
    {
        fail_(Failure::NoOffscreen,
              "the engine has no off-screen surfaces, no glTF importer or no scripts");
        return;
    }

    if (stopping_())
    {
        return;
    }

    auto sinfo = eh::Init<engine_surface_info>();
    sinfo.kind = ENGINE_SURFACE_OFFSCREEN;
    sinfo.width = options.width;
    sinfo.height = options.height;
    sinfo.scale = 1.0f;
    if (!engine.CreateSurface(sinfo, &surface))
    {
        fail_with_error(Failure::NoDevice);
        return;
    }

    if (stopping_())
    {
        return;
    }

    std::vector<std::uint8_t> robot_glb, field_glb;
    if (!read_file(options.assets_dir + "/" + options.user_asset, &robot_glb) ||
        !read_file(options.assets_dir + "/" + options.field_asset, &field_glb))
    {
        fail_(Failure::ImportFailed, "cannot read the world assets in " + options.assets_dir);
        return;
    }
    auto iopt = eh::Init<engine_import_options>();
    iopt.flags = ENGINE_IMPORT_EMIT_LEVEL;
    iopt.id_seed = 20261117;
    iopt.path_prefix = eh::Str("/robot");
    const std::unique_ptr<AssetCacheStore> store =
        options.asset_cache_dir.empty()
            ? nullptr
            : std::make_unique<AssetCacheStore>(options.asset_cache_dir);
    WorldAssetLoader loader(api, engine, about.caps, store.get(),
                            library_stamp(options.library_path));
    ImportedAsset robot, field;
    AssetLoadReport robot_report, field_report;
    bool imported = loader.load("robot", robot_glb, iopt, &robot, &robot_report);
    if (stopping_())
    {
        return;
    }
    if (imported)
    {
        iopt.id_seed = 20261118;
        iopt.path_prefix = eh::Str("/field");
        imported = loader.load("field", field_glb, iopt, &field, &field_report);
    }
    if (stopping_())
    {
        return;
    }
    if (!imported || robot.ids.empty() || field.ids.empty())
    {
        fail_with_error(Failure::ImportFailed);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        asset_loads_ = {robot_report, field_report};
    }
    const std::uint64_t robot_level = robot.level_id;
    const std::uint64_t field_level = field.level_id;
    // The clips the robot model has, for its script to pick from (one path per line).
    std::string robot_animations;
    for (std::size_t i = 0;
         i < robot.ids.size() && i < robot.types.size() && i < robot.paths.size(); ++i)
    {
        if (robot.types[i] == ENGINE_RESOURCE_ANIMATION)
        {
            robot_animations += robot.paths[i] + "\n";
        }
    }

    // The world: a sky to start from (assets/worlds/daylight.lua takes it over, with the sun and
    // the moon) and a camera.
    if (stopping_())
    {
        return;
    }
    if (!step(engine.CreateWorld(&world)))
    {
        return;
    }
    auto environment = eh::Init<engine_environment>();
    environment.ambient_color[0] = environment.ambient_color[1] = environment.ambient_color[2] =
        1.0f;
    environment.ambient_intensity = 0.35f;
    environment.sky_color[0] = 0.45f;
    environment.sky_color[1] = 0.65f;
    environment.sky_color[2] = 0.9f;
    environment.exposure = 1.0f;
    if (!step(world.SetEnvironment(environment)))
    {
        return;
    }

    std::uint64_t camera_entity = 0;
    if (!step(world.Entity(&camera_entity)))
    {
        return;
    }
    auto cam_t = eh::Init<engine_transform>();
    cam_t.position[1] = 1.1f;
    cam_t.position[2] = 2.6f;
    cam_t.rotation[3] = 1.0f;
    cam_t.scale[0] = cam_t.scale[1] = cam_t.scale[2] = 1.0f;
    auto camera = eh::Init<engine_camera>();
    camera.fov_y = 1.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 200.0f;
    if (!step(world.SetComponent(camera_entity, ENGINE_COMPONENT_TRANSFORM, &cam_t)) ||
        !step(world.SetComponent(camera_entity, ENGINE_COMPONENT_CAMERA, &camera)) ||
        !step(surface.SetCamera(camera_entity)) || !step(surface.SetWorld(world)))
    {
        return;
    }

    auto load_script = [&](const ScriptSpec& spec) -> bool
    {
        std::vector<std::uint8_t> source;
        if (!read_file(options.assets_dir + "/" + spec.file, &source))
        {
            fail_(Failure::ImportFailed, "cannot read " + spec.file + " in " + options.assets_dir);
            return false;
        }
        auto script = eh::Init<engine_script_load>();
        script.name = eh::Str(spec.file.c_str());
        script.source = {reinterpret_cast<const char*>(source.data()), source.size()};
        return step(world.ScriptLoad(script));
    };

    // What the app listens to.
    for (const std::string& name : options.events_out)
    {
        std::uint32_t subscription = 0;
        if (!step(world.ScriptEventSubscribe(eh::Str(name.c_str()), &on_script_event, &event_sink,
                                             &subscription)))
        {
            return;
        }
    }

    // The scripts of the world, in order; one on the camera entity, the others on entities of their own.
    for (const ScriptSpec& spec : options.scripts)
    {
        if (!load_script(spec))
        {
            return;
        }
        std::uint64_t entity = camera_entity;
        if (!spec.on_camera && !step(world.Entity(&entity)))
        {
            return;
        }
        auto component = eh::Init<engine_script_component>();
        component.enabled = 1;
        std::strncpy(component.behavior, spec.behavior.c_str(), sizeof(component.behavior) - 1);
        const ParamMap params{spec.params};
        component.params = &params.pool;
        if (!step(world.SetComponent(entity, ENGINE_COMPONENT_SCRIPT, &component)))
        {
            return;
        }
    }
    if (!load_script(options.user_script)) // attached to each robot as it is spawned
    {
        return;
    }

    // The meadow. Its "Spawn" node is where Task 2's spawn circle is centred: the origin.
    auto place_field = eh::Init<engine_spawn_params>();
    place_field.rotation[3] = 1.0f;
    place_field.scale = 1.0f;
    std::uint64_t root = 0;
    std::size_t total = 0;
    std::uint64_t keys[64];
    if (!step(world.SpawnLevel(field_level, place_field, &root, keys, 0, &total)))
    {
        return;
    }
    {
        std::uint64_t spawn_key = 0;
        auto spawn_t = eh::Init<engine_transform>();
        if (world.Find("Spawn", &spawn_key).Ok() &&
            world.GetComponent(spawn_key, ENGINE_COMPONENT_TRANSFORM, &spawn_t).Ok())
        {
            if (std::fabs(spawn_t.position[0]) > 1e-3f || std::fabs(spawn_t.position[1]) > 1e-3f ||
                std::fabs(spawn_t.position[2]) > 1e-3f)
            {
                std::fprintf(stderr,
                             "[world] the meadow's Spawn node is at (%g, %g, %g), not the origin\n",
                             spawn_t.position[0], spawn_t.position[1], spawn_t.position[2]);
            }
        }
        else
        {
            std::fprintf(stderr, "[world] the meadow has no Spawn node\n");
        }
    }

    unsigned spawn_counter = 0;

    // A robot for a participant: its own level copy and its own texture for the screen, then its
    // script, which does the rest.
    auto spawn_robot = [&](const std::string& id) -> bool
    {
        Robot r;
        auto place =
            eh::Init<engine_spawn_params>(); // at the origin: its script puts it where it belongs
        place.rotation[3] = 1.0f;
        place.scale = 1.0f;
        r.prefix = "p" + std::to_string(spawn_counter++); // up to 15 bytes
        place.name_prefix = eh::Str(r.prefix.c_str());
        std::size_t spawned = 0;
        const std::string texture_name = "screen/" + r.prefix;
        if (!step(world.SpawnLevel(robot_level, place, &r.root, keys, 64, &spawned)) ||
            !step(make_texture(engine, texture_name.c_str(), &r.texture)) ||
            !step(upload(r.texture, flat_rgba(20, 20, 24), kScreenW)))
        {
            return false;
        }
        for (std::size_t k = 0; k < spawned && k < 64; ++k)
        {
            r.keys.push_back(keys[k]);
        }
        auto component = eh::Init<engine_script_component>();
        component.enabled = 1;
        std::strncpy(component.behavior, options.user_script.behavior.c_str(),
                     sizeof(component.behavior) - 1);
        auto entries = options.user_script.params;
        entries.emplace_back("id", id);
        entries.emplace_back("prefix", r.prefix);
        entries.emplace_back("screen", texture_name);
        entries.emplace_back("animations", robot_animations);
        const ParamMap params{entries};
        component.params = &params.pool;
        if (!step(world.SetComponent(r.root, ENGINE_COMPONENT_SCRIPT, &component)))
        {
            return false;
        }
        robots.emplace(id, std::move(r));
        return true;
    };

    {
        std::lock_guard lock(mutex_);
        state_ = State::Running;
    }

    // Draw at 60 Hz until stopped.
    auto next = std::chrono::steady_clock::now();
    auto last = next;
    unsigned frames_run = 0;
    while (!stopping_())
    {
        std::uint32_t rw = 0, rh = 0;
        std::vector<std::string> wanted;
        std::vector<ScriptEvent> events;
        std::map<std::string, PendingScreen> pending;
        std::optional<std::pair<std::string, std::string>> watched;
        {
            std::lock_guard lock(mutex_);
            watched = watched_joint_;
            rw = std::exchange(resize_w_, 0);
            rh = std::exchange(resize_h_, 0);
            wanted = robots_;
            // A script subscribes during its first tick, so what is sent before that finds nobody:
            // keep the events until a frame has run.
            if (frames_run > 0)
            {
                events.swap(inbox_);
            }
            pending.swap(screens_);
        }
        if (rw != 0 && rh != 0 && !step(surface.Resized(rw, rh, 1.0f)))
        {
            return;
        }

        // Robots that are gone, then the ones that are new.
        const std::set<std::string> ids(wanted.begin(), wanted.end());
        for (auto it = robots.begin(); it != robots.end();)
        {
            if (ids.count(it->first) != 0)
            {
                ++it;
                continue;
            }
            for (auto k = it->second.keys.rbegin(); k != it->second.keys.rend(); ++k)
            {
                (void)world.DestroyEntity(*k); // the texture goes with the map entry, after these
            }
            {
                std::lock_guard lock(mutex_);
                joint_matrices_.erase(it->first);
            }
            it = robots.erase(it);
        }
        for (const std::string& id : wanted)
        {
            if (robots.count(id) == 0 && !spawn_robot(id))
            {
                return;
            }
        }

        // What the app posted, for the scripts to hear at the start of the tick inside the frame below.
        for (const ScriptEvent& e : events)
        {
            const ValuePool payload{e.value};
            (void)world.ScriptEventEmit(eh::Str(e.name.c_str()), &payload.pool, 0);
        }

        // New screen pixels.
        for (auto& [id, screen] : pending)
        {
            auto it = robots.find(id);
            if (it == robots.end())
            {
                continue;
            }
            Robot& rb = it->second;
            if ((screen.w != rb.tex_w || screen.h != rb.tex_h) &&
                !step(rb.texture.Resize(screen.w, screen.h)))
            {
                return;
            }
            rb.tex_w = screen.w;
            rb.tex_h = screen.h;
            if (!step(upload(rb.texture, *screen.pixels, screen.w, screen.update_flags)))
            {
                return;
            }
        }

        // Tell every robot's script how big its picture is (every frame: a script that has not
        // subscribed yet loses nothing that matters).
        for (const std::string& id : wanted)
        {
            const Robot& rb = robots.at(id);
            const NumberArray shape{static_cast<double>(rb.tex_w), static_cast<double>(rb.tex_h)};
            (void)world.ScriptEventEmit(eh::Str("robot_screen"), &shape.pool, rb.root);
            if (watched.has_value())
            {
                std::uint64_t node = 0;
                std::uint32_t joint = 0;
                std::array<float, 16> m{};
                if (world.Find((rb.prefix + "/" + watched->first).c_str(), &node).Ok() &&
                    world.FindJoint(node, watched->second.c_str(), &joint).Ok() &&
                    world.JointTransform(node, joint, m.data()).Ok())
                {
                    std::lock_guard lock(mutex_);
                    joint_matrices_[id] = m;
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        auto frame = eh::Init<engine_frame_info>();
        frame.delta_seconds = std::chrono::duration<double>(now - last).count();
        last = now;
        if (!step(engine.Frame(frame)))
        {
            return;
        }
        ++frames_run;

        engine_frame_image image;
        const eh::Status got = surface.AcquireFrame(&image);
        if (got.Ok())
        {
            auto drawn_state = eh::Init<engine_surface_state>();
            const std::uint32_t drawn =
                surface.State(&drawn_state).Ok() ? drawn_state.objects_drawn : 0;
            auto copy = std::make_shared<std::vector<std::uint8_t>>(
                static_cast<std::size_t>(image.width) * image.height * 4);
            const auto* src = static_cast<const std::uint8_t*>(image.data);
            for (std::uint32_t y = 0; y < image.height; ++y)
            {
                std::memcpy(copy->data() + static_cast<std::size_t>(y) * image.width * 4,
                            src + static_cast<std::size_t>(y) * image.stride,
                            static_cast<std::size_t>(image.width) * 4);
            }
            {
                std::lock_guard lock(mutex_);
                picture_.rgba = std::move(copy);
                picture_.width = image.width;
                picture_.height = image.height;
                picture_.frame = image.frame;
                picture_.objects_drawn = drawn;
            }
            (void)surface.ReleaseFrame(image);
        }
        else if (got != ENGINE_ERROR_NOT_READY)
        {
            fail_with_error(Failure::SceneFailed);
            return;
        }

        next += kFramePeriod;
        if (next < std::chrono::steady_clock::now())
        {
            next = std::chrono::steady_clock::now(); // behind: do not try to catch up
        }
        std::this_thread::sleep_until(next);
    }
}

} // namespace tesseract::views::world

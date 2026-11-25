#pragma once

// The 3D engine (docs/3D-CALL-ROOMS.md) drawing a meadow with one robot per call
// participant into an off-screen picture, on a thread of its own. Built only with
// TESSERACT_WITH_ENGINE. Every engine call happens on that thread;
// the rest of the app talks to it through the methods below, from any thread.

#include "views/world/AssetCacheStore.h"

#include <tesseract/paths.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

struct engine_api;

namespace tesseract::views::world
{

// What an event carries between the app and the world's scripts: a number, text, a blob of bytes, or a list of
// those. The scene only moves them; what they mean is the scripts' business.
struct EventValue
{
    enum class Kind
    {
        Nil,
        Number,
        String,
        Bytes,
        Array
    };
    Kind kind = Kind::Nil;
    double number = 0;
    std::string text;              // String and Bytes
    std::vector<EventValue> items; // Array

    static EventValue of(double n)
    {
        EventValue v;
        v.kind = Kind::Number;
        v.number = n;
        return v;
    }
    static EventValue of(std::string s)
    {
        EventValue v;
        v.kind = Kind::String;
        v.text = std::move(s);
        return v;
    }
    static EventValue bytes(const std::uint8_t* data, std::size_t size)
    {
        EventValue v;
        v.kind = Kind::Bytes;
        v.text.assign(reinterpret_cast<const char*>(data), size);
        return v;
    }
    static EventValue array(std::vector<EventValue> items)
    {
        EventValue v;
        v.kind = Kind::Array;
        v.items = std::move(items);
        return v;
    }
    // The i-th item of an array as a number (0 when absent or not a number).
    double num(std::size_t i) const
    {
        return i < items.size() && items[i].kind == Kind::Number ? items[i].number : 0.0;
    }
};

struct ScriptEvent
{
    std::string name;
    EventValue value;
};

// One script of the world: a Lua file in the assets directory and the behavior in it to attach.
struct ScriptSpec
{
    using Param = std::variant<double, std::string>;
    std::string file;
    std::string behavior;
    bool on_camera = false; // else on an entity of its own
    std::vector<std::pair<std::string, Param>> params;
};

struct Picture
{
    std::shared_ptr<const std::vector<std::uint8_t>>
        rgba; // straight-alpha RGBA8, stride = width * 4; null until the first frame
    std::uint32_t width = 0, height = 0;
    std::uint64_t frame = 0;         // engine_frame_image.frame; changes on every new picture
    std::uint32_t objects_drawn = 0; // the engine's count for this picture (tests, diagnostics)
};

// The engine allows one instance per process, so only one EngineScene may run at a time.
class EngineScene
{
public:
    enum class State
    {
        Stopped,
        Starting,
        Running,
        Failed
    };
    enum class Failure
    {
        None,
        LoadFailed,
        NoOffscreen,
        NoDevice,
        ImportFailed,
        SceneFailed
    };
    static std::string default_library_path();
    static std::string default_assets_dir();

    struct Options
    {
        // The library and assets inside the macOS app bundle when present, else the
        // build-tree paths compiled in (see default_library_path()).
        std::string library_path = default_library_path();
        std::string assets_dir = default_assets_dir();
        // Where the minted resources of the assets are kept between runs; empty turns the cache off.
        std::filesystem::path asset_cache_dir = default_asset_cache_dir();
        std::uint32_t width = 0, height = 0; // device pixels, both > 0
        // The models (in assets_dir): the field is spawned once, a robot for each participant.
        std::string field_asset, user_asset;
        // The scripts of the world, loaded in this order.
        std::vector<ScriptSpec> scripts;
        // The script attached to every robot, which gets the params `id` (the participant), `prefix`
        // (its entities are called "<prefix>/<name>"), `screen` (the name of the host texture on its
        // "Screen" slot) and `animations` (the model's clip paths, one per line) besides its own.
        ScriptSpec user_script;
        // The names of the script events the app wants to hear (see take_events, latest_event).
        std::vector<std::string> events_out;
    };

    EngineScene();
    ~EngineScene(); // calls stop()
    EngineScene(const EngineScene&) = delete;
    EngineScene& operator=(const EngineScene&) = delete;

    // Spawns the frame thread and returns at once. No-op unless Stopped or Failed.
    void start(Options options);
    // Whether the engine can draw here: loads the library, creates an engine, checks its
    // capabilities, makes a 16x16 off-screen surface, then destroys and unloads it all, on the
    // calling thread. Blocks (run it off the UI thread); only while no scene runs. None = usable.
    // `options.width/height` are ignored.
    static Failure probe(const Options& options);
    // Joins the thread; the engine is destroyed and the library unloaded. Idempotent.
    void stop();

    State state() const;
    Failure failure() const;
    std::string
    detail() const; // the engine's message (loader.message / last_error), for the log only

    // Device pixels; ignored when either is 0; applied before the next frame.
    void resize(std::uint32_t width, std::uint32_t height);
    // Any thread. The scene shows exactly these robots, one for each id: the frame thread spawns one for
    // each id it has not got and destroys the ones that are gone. Where they stand is the scripts' doing.
    void set_robots(std::vector<std::string> ids);
    // Any thread. Sends a script event to every script that listens to it. Events are delivered in the
    // order posted, none before the scripts have had their first tick (they would find nobody listening).
    void post_event(std::string name, EventValue value);
    // Any thread. The events of Options::events_out the scripts emitted since the last call.
    std::vector<ScriptEvent> take_events();
    // Any thread. The newest value of an event of Options::events_out; nullopt before the first one.
    std::optional<EventValue> latest_event(const std::string& name) const;
    // Any thread. Diagnostic: from now on the frame thread measures the joint called `joint` of the
    // entity called `node` in every robot (the robot's own "<prefix>/<node>").
    void watch_robot_joint(const std::string& node, const std::string& joint);
    // Any thread. Diagnostic: the model-space matrix (column-major) of the watched joint of the robot
    // `id`, as the engine's last frame left it; empty when there is no such robot or joint.
    std::optional<std::array<float, 16>> robot_joint_matrix(const std::string& id) const;
    // Any thread; ignored for an id set_robots has not named. Premultiplied BGRA (opaque video);
    // the pixels are shared, not copied.
    void set_robot_screen_bgra(const std::string& id,
                               std::shared_ptr<const std::vector<std::uint8_t>> bgra,
                               std::uint32_t width, std::uint32_t height);
    // Any thread; the robot's screen goes back to flat (20, 20, 24).
    void clear_robot_screen(const std::string& id);
    Picture latest_picture() const; // any thread
    // Any thread. How the assets of the current (or last) run were loaded, in load order: robot, field.
    std::vector<AssetLoadReport> asset_loads() const;

private:
    void run_(Options options);
    void run_engine_(const engine_api* api, const Options& options);
    void fail_(Failure failure, std::string detail);
    bool stopping_() const;

    mutable std::mutex mutex_;
    std::thread thread_;
    State state_ = State::Stopped;
    Failure failure_ = Failure::None;
    std::string detail_;
    std::vector<AssetLoadReport> asset_loads_;
    bool stop_requested_ = false;
    std::uint32_t resize_w_ = 0, resize_h_ = 0; // pending, 0 = none
    struct PendingScreen
    {
        std::shared_ptr<const std::vector<std::uint8_t>> pixels; // w * h * 4 bytes
        std::uint32_t update_flags = 0;                          // ENGINE_UPDATE_* of the layout
        std::uint32_t w = 0, h = 0;
    };
    std::vector<std::string> robots_;                                  // wanted
    std::map<std::string, std::array<float, 16>> joint_matrices_;      // measured, by robot id
    std::optional<std::pair<std::string, std::string>> watched_joint_; // node, joint
    std::vector<ScriptEvent> inbox_;                                   // posted, not yet delivered
    std::vector<ScriptEvent> outbox_;                                  // emitted, not yet taken
    std::map<std::string, EventValue> latest_;                         // by name
    std::map<std::string, PendingScreen> screens_;                     // pending, by robot id
    Picture picture_;
};

} // namespace tesseract::views::world

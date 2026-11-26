// EngineScene: the 3D engine on its own thread, drawn off-screen.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "views/world/EngineScene.h"
#include "views/world/WorldAssetLoader.h"
#include "views/world/world_room.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using tesseract::views::world::AssetLoadReport;
using tesseract::views::world::EngineScene;
using tesseract::views::world::Picture;
using tesseract::views::world::EventValue;
using tesseract::views::world::room_options;

namespace
{

template <typename Pred>
bool wait_for(Pred pred, std::chrono::seconds timeout = std::chrono::seconds(20))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// The daylight cycle's clock the tests pin: noon of a 1440 s day, the sun high.
constexpr double kNoon = 720.0;

// The room's options at a size and a time of day.
EngineScene::Options room(std::uint32_t w, std::uint32_t h, double clock = kNoon)
{
    EngineScene::Options o = room_options(clock);
    o.events_out.push_back("robot_pose"); // where the room's script draws each participant
    o.width = w;
    o.height = h;
    return o;
}

// Starts a scene and waits until it is no longer starting.
void start_settled(EngineScene& scene, std::uint32_t w, std::uint32_t h, double clock = kNoon)
{
    scene.start(room(w, h, clock));
    REQUIRE(wait_for(
        [&]
        {
            return scene.state() != EngineScene::State::Starting;
        }));
}

} // namespace

TEST_CASE("EngineScene reports a library that cannot be loaded", "[world]")
{
    EngineScene scene;
    scene.start({.library_path = "/nonexistent/libengine.so.1", .width = 64, .height = 36});
    REQUIRE(wait_for(
        [&]
        {
            return scene.state() == EngineScene::State::Failed;
        }));
    CHECK(scene.failure() == EngineScene::Failure::LoadFailed);
    CHECK_FALSE(scene.detail().empty());
    scene.stop(); // no hang after a failure
}

TEST_CASE("EngineScene::probe says whether the engine is usable", "[world]")
{
    // A bad library path is a load failure, at once.
    CHECK(EngineScene::probe({.library_path = "/nonexistent/libengine.so.1"}) ==
          EngineScene::Failure::LoadFailed);

    const EngineScene::Failure f = EngineScene::probe({});
    if (f == EngineScene::Failure::NoDevice)
    {
        SKIP("no graphics device");
    }
    CHECK(f == EngineScene::Failure::None);
    // The probe leaves nothing behind: a scene starts after it.
    EngineScene scene;
    start_settled(scene, 64, 36);
    CHECK(scene.state() == EngineScene::State::Running);
}

TEST_CASE("EngineScene draws the meadow into a picture of its size", "[world]")
{
    EngineScene scene;
    start_settled(scene, 320, 180);
    if (scene.failure() == EngineScene::Failure::NoDevice)
    {
        SKIP("no graphics device");
    }
    REQUIRE(scene.state() == EngineScene::State::Running);
    REQUIRE(wait_for(
        [&]
        {
            return scene.latest_picture().rgba != nullptr;
        }));
    const Picture p = scene.latest_picture();
    CHECK(p.width == 320);
    CHECK(p.height == 180);
    CHECK(p.rgba->size() == 320u * 180u * 4u);
    // not one flat colour: the sky (top-left pixel) differs from the ground (pixel 160, 170)
    const auto* px = p.rgba->data();
    const std::size_t ground = (170u * 320u + 160u) * 4u;
    CHECK(std::memcmp(px, px + ground, 3) != 0);
    const std::uint64_t first = p.frame;
    REQUIRE(wait_for(
        [&]
        {
            return scene.latest_picture().frame > first;
        })); // it keeps drawing
}

TEST_CASE("EngineScene follows a resize and ignores a zero size", "[world]")
{
    EngineScene scene;
    start_settled(scene, 320, 180);
    if (scene.failure() == EngineScene::Failure::NoDevice)
    {
        SKIP("no graphics device");
    }
    REQUIRE(scene.state() == EngineScene::State::Running);
    REQUIRE(wait_for(
        [&]
        {
            return scene.latest_picture().rgba != nullptr;
        }));

    scene.resize(0, 100);
    const std::uint64_t before = scene.latest_picture().frame;
    REQUIRE(wait_for(
        [&]
        {
            return scene.latest_picture().frame >= before + 10;
        }));
    CHECK(scene.latest_picture().width == 320);
    CHECK(scene.latest_picture().height == 180);

    scene.resize(160, 90);
    REQUIRE(wait_for(
        [&]
        {
            const Picture p = scene.latest_picture();
            return p.width == 160 && p.height == 90;
        }));
}

namespace
{

using tesseract::views::world::MoveInput;
using tesseract::views::world::Pose;

// Waits until `n` more pictures have been drawn, then returns the newest.
Picture next_pictures(EngineScene& scene, std::uint64_t n = 6)
{
    const std::uint64_t f = scene.latest_picture().frame;
    REQUIRE(wait_for(
        [&]
        {
            return scene.latest_picture().frame >= f + n;
        }));
    return scene.latest_picture();
}

std::uint64_t checksum(const Picture& p)
{
    std::uint64_t sum = 1469598103934665603ull;
    for (std::uint8_t b : *p.rgba)
    {
        sum = (sum ^ b) * 1099511628211ull;
    }
    return sum;
}

// Pixels whose colour differs from `base` by more than the dither.
std::size_t differing(const Picture& a, const Picture& base)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < a.rgba->size(); i += 4)
    {
        int worst = 0;
        for (int c = 0; c < 3; ++c)
        {
            worst = std::max(worst, std::abs(int((*a.rgba)[i + c]) - int((*base.rgba)[i + c])));
        }
        count += worst > 6 ? 1 : 0;
    }
    return count;
}

// Mean RGB of the picture's top 20 rows.
std::array<double, 3> top_rows_mean(const Picture& p)
{
    std::array<double, 3> sum{0, 0, 0};
    const std::size_t n = std::size_t{20} * p.width;
    for (std::size_t i = 0; i < n; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            sum[c] += (*p.rgba)[i * 4 + c];
        }
    }
    for (double& c : sum)
    {
        c /= static_cast<double>(n);
    }
    return sum;
}

// The share of pixels within 10 levels of `sky`.
double sky_share(const Picture& p, const std::array<double, 3>& sky)
{
    std::size_t n = 0;
    const std::size_t total = std::size_t{p.width} * p.height;
    for (std::size_t i = 0; i < total; ++i)
    {
        bool near = true;
        for (int c = 0; c < 3; ++c)
        {
            near = near && std::fabs((*p.rgba)[i * 4 + c] - sky[c]) < 10.0;
        }
        n += near ? 1 : 0;
    }
    return static_cast<double>(n) / static_cast<double>(total);
}

Pose at(float x, float z)
{
    Pose p;
    p.x = x;
    p.z = z;
    return p;
}

// ---- The room as the app drives it: events in, events out (assets/worlds/world.lua does the rest) ----

const std::string kMe = "@me:test";

void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        b[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}
void putf(std::vector<std::uint8_t>& b, std::size_t at, float f)
{
    std::uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(b, at, v);
}
std::uint32_t get32(const std::vector<std::uint8_t>& b, std::size_t at)
{
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
float getf(const std::vector<std::uint8_t>& b, std::size_t at)
{
    const std::uint32_t v = get32(b, at);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// A pose frame of the wire format (docs/3D-DATA-STREAMS.md), as a remote client would send it.
std::vector<std::uint8_t> pose_frame(std::uint32_t seq, std::uint32_t time_ms, const Pose& p)
{
    std::vector<std::uint8_t> b(32, 0);
    b[0] = 1;
    b[1] = 1;
    b[2] = p.walking ? 1 : 0;
    put32(b, 4, seq);
    put32(b, 8, time_ms);
    putf(b, 12, p.x);
    putf(b, 16, p.y);
    putf(b, 20, p.z);
    const long yaw = std::lround(p.yaw / (2 * std::numbers::pi) * 65536.0) & 0xFFFF;
    b[24] = static_cast<std::uint8_t>(yaw & 0xFF);
    b[25] = static_cast<std::uint8_t>(yaw >> 8);
    const long pitch = std::lround(p.pitch / (std::numbers::pi / 2) * 32767.0);
    b[26] = static_cast<std::uint8_t>(pitch & 0xFF);
    b[27] = static_cast<std::uint8_t>((pitch >> 8) & 0xFF);
    const long speed = std::lround(p.speed * 1000.0);
    b[28] = static_cast<std::uint8_t>(speed & 0xFF);
    b[29] = static_cast<std::uint8_t>((speed >> 8) & 0xFF);
    return b;
}

// What a frame the room sent says: a pose frame (kind 1) or a nudge (kind 2) for `target`.
struct SentFrame
{
    int kind = 0;
    Pose pose;          // kind 1
    std::string target; // kind 2
    float dx = 0, dz = 0;
    std::uint32_t seq = 0;
};
std::optional<SentFrame> read_frame(const std::vector<std::uint8_t>& b)
{
    if (b.size() < 12 || b[0] != 1)
    {
        return std::nullopt;
    }
    SentFrame f;
    f.kind = b[1];
    f.seq = get32(b, 4);
    if (f.kind == 1 && b.size() == 32)
    {
        f.pose.walking = (b[2] & 1) != 0;
        f.pose.x = getf(b, 12);
        f.pose.y = getf(b, 16);
        f.pose.z = getf(b, 20);
        f.pose.yaw = static_cast<float>((b[24] | (b[25] << 8)) * 2 * std::numbers::pi / 65536.0);
        f.pose.pitch = static_cast<float>(static_cast<std::int16_t>(b[26] | (b[27] << 8)) /
                                          32767.0 * std::numbers::pi / 2);
        f.pose.speed = (b[28] | (b[29] << 8)) / 1000.0f;
        return f;
    }
    if (f.kind == 2 && b.size() > 21 && b.size() == 21u + b[20])
    {
        f.dx = getf(b, 12);
        f.dz = getf(b, 16);
        f.target.assign(reinterpret_cast<const char*>(b.data() + 21), b[20]);
        return f;
    }
    return std::nullopt;
}

struct RobotSetup
{
    std::string id;
    Pose pose;
};

// The others in the call, standing where `robots` says (a frame of each: their script draws them
// there at once).
void set_robots(EngineScene& scene, const std::vector<RobotSetup>& robots)
{
    static std::map<std::string, std::uint32_t> seqs;
    std::vector<std::string> ids;
    for (const RobotSetup& r : robots)
    {
        ids.push_back(r.id);
    }
    scene.set_robots(ids);
    scene.post_event("participants", tesseract::views::world::participants_event(kMe, ids));
    for (const RobotSetup& r : robots)
    {
        // The sender's clock runs a millisecond a frame, so a new pose is shown at once.
        const std::uint32_t seq = ++seqs[r.id];
        const auto f = pose_frame(seq, seq, r.pose);
        scene.post_event("frame", tesseract::views::world::frame_event(r.id, f.data(), f.size()));
    }
}

std::optional<Pose> local_pose(const EngineScene& scene)
{
    const auto v = scene.latest_event("pose");
    return v ? tesseract::views::world::pose_from_event(*v) : std::nullopt;
}

float local_height(const EngineScene& scene)
{
    const auto v = scene.latest_event("pose");
    return v ? tesseract::views::world::eye_height_from_event(*v) : 0.0f;
}

void set_local_pose(EngineScene& scene, const Pose& p)
{
    scene.post_event("reset", EventValue::array({EventValue::of(p.x), EventValue::of(p.z),
                                                 EventValue::of(p.yaw), EventValue::of(p.pitch)}));
}

void nudge_local(EngineScene& scene, float dx, float dz)
{
    scene.post_event("nudge", EventValue::array({EventValue::of(dx), EventValue::of(dz)}));
}

void set_input(EngineScene& scene, const tesseract::views::world::MoveInput& in)
{
    scene.post_event("input", tesseract::views::world::input_event(in));
}

// A running scene whose camera stands at the origin looking along +Z.
void start_running(EngineScene& scene, double clock = kNoon)
{
    start_settled(scene, 320, 180, clock);
    if (scene.failure() == EngineScene::Failure::NoDevice)
    {
        SKIP("no graphics device");
    }
    REQUIRE(scene.state() == EngineScene::State::Running);
    // The room's script starts the local player on its spawn circle (3 m round the origin); only then
    // is a teleport of ours not undone by it.
    set_robots(scene, {});
    REQUIRE(wait_for(
        [&]
        {
            const auto p = local_pose(scene);
            return p && std::fabs(std::hypot(p->x, p->z) - 3.0f) < 0.3f;
        }));
    set_local_pose(scene, Pose{});
    REQUIRE(wait_for(
        [&]
        {
            return scene.latest_picture().rgba != nullptr;
        }));
    // A teleport puts the player a little above the floor; wait until it has landed and stands still.
    float last = -1.0f;
    int still = 0;
    REQUIRE(wait_for(
        [&]
        {
            const float h = local_height(scene);
            still = std::fabs(h - last) < 1e-4f && h > 1.0f ? still + 1 : 0;
            last = h;
            return still >= 5;
        }));
}

} // namespace

TEST_CASE("EngineScene spawns a robot for each id and removes it when the id goes", "[world]")
{
    EngineScene scene;
    start_running(scene);
    const Picture meadow = next_pictures(scene);
    REQUIRE(meadow.objects_drawn > 0);

    set_robots(scene, {{"a", at(-0.8f, 2.5f)}, {"b", at(0.8f, 2.5f)}});
    const Picture two = next_pictures(scene);
    CHECK(two.objects_drawn > meadow.objects_drawn);
    CHECK(differing(two, meadow) > 100);

    set_robots(scene, {{"a", at(-0.8f, 2.5f)}});
    const Picture one = next_pictures(scene);
    CHECK(one.objects_drawn < two.objects_drawn);
    CHECK(one.objects_drawn > meadow.objects_drawn);

    set_robots(scene, {});
    const Picture none = next_pictures(scene);
    CHECK(none.objects_drawn == meadow.objects_drawn);
    CHECK(scene.state() == EngineScene::State::Running);
}

TEST_CASE("EngineScene moves a robot by its pose", "[world]")
{
    EngineScene scene;
    start_running(scene);
    const Picture meadow = next_pictures(scene);

    set_robots(scene, {{"a", at(0.0f, 2.5f)}});
    const Picture in_front = next_pictures(scene);
    CHECK(differing(in_front, meadow) > 100);

    // Behind the camera: the robot's children moved with its root, out of the picture.
    set_robots(scene, {{"a", at(0.0f, -6.0f)}});
    const Picture behind = next_pictures(scene);
    CHECK(differing(behind, meadow) < 50);

    // Walking changes the picture from frame to frame; standing still does not.
    Pose walking = at(0.0f, 2.5f);
    walking.walking = true;
    walking.speed = 1.6f;
    set_robots(scene, {{"a", walking}});
    const Picture w1 = next_pictures(scene, 10);
    const Picture w2 = next_pictures(scene, 10);
    CHECK(checksum(w1) != checksum(w2));
}

TEST_CASE("EngineScene takes robot screens for known ids only", "[world]")
{
    EngineScene scene;
    start_running(scene);
    auto red = std::make_shared<std::vector<std::uint8_t>>(640 * 480 * 4, 0);
    for (std::size_t i = 0; i < red->size(); i += 4)
    {
        (*red)[i + 2] = 255; // R of BGRA
        (*red)[i + 3] = 255;
    }
    Pose facing_us = at(0.0f, 1.2f);
    facing_us.yaw = 3.14159265f; // robots face +Z at 0: turn to face the camera
    set_robots(scene, {{"a", facing_us}});
    next_pictures(scene);
    const Picture flat = next_pictures(scene);

    scene.set_robot_screen_bgra("nobody", red, 640, 480); // unknown id: ignored
    scene.clear_robot_screen("nobody");
    const Picture still = next_pictures(scene);
    // An idle robot still breathes: allow a little motion.
    const std::size_t idle_motion = differing(still, flat);
    CHECK(idle_motion < 600);
    CHECK(scene.state() == EngineScene::State::Running);

    scene.set_robot_screen_bgra("a", red, 640, 480);
    const Picture shown = next_pictures(scene);
    INFO("idle " << idle_motion << " shown " << differing(shown, flat));
    CHECK(differing(shown, flat) > idle_motion * 3 + 300); // the screen is red now
    CHECK(scene.state() == EngineScene::State::Running);

    auto small = std::make_shared<std::vector<std::uint8_t>>(32 * 18 * 4, 255);
    scene.set_robot_screen_bgra("a", small, 32, 18); // a new size
    next_pictures(scene);
    scene.clear_robot_screen("a");
    const Picture cleared = next_pictures(scene);
    CHECK(differing(cleared, flat) < idle_motion * 2 + 300); // flat again, but for the breathing
    CHECK(scene.state() == EngineScene::State::Running);

    scene.stop();
    CHECK(scene.state() == EngineScene::State::Stopped);
    scene.stop(); // a second stop is a no-op
    CHECK(scene.state() == EngineScene::State::Stopped);
}

TEST_CASE("EngineScene's camera follows the pose", "[world]")
{
    EngineScene scene;
    start_running(scene);
    const Picture ahead = next_pictures(scene);

    Pose turned;
    turned.yaw = 3.1f; // facing back along -Z
    set_local_pose(scene, turned);
    const Picture behind = next_pictures(scene);
    CHECK(checksum(behind) != checksum(ahead));

    Pose looking_up;
    looking_up.pitch = 0.6f;
    set_local_pose(scene, looking_up);
    const Picture up = next_pictures(scene);
    CHECK(checksum(up) != checksum(ahead));
    // Looking up shows more sky: the pixel row just above the old horizon is no longer ground.
    CHECK(differing(up, ahead) > 1000);

    // More sky: the share of sky-coloured pixels (the sky seen straight up is the reference) rises
    // as the camera looks up and falls as it looks down.
    Pose zenith;
    zenith.pitch = 1.5f;
    set_local_pose(scene, zenith);
    const auto sky_ref = top_rows_mean(next_pictures(scene));
    const double sky_up = sky_share(up, sky_ref);
    const double sky_ahead = sky_share(ahead, sky_ref);
    CHECK(sky_up > sky_ahead);

    Pose down;
    down.pitch = -0.6f;
    set_local_pose(scene, down);
    const Picture ground = next_pictures(scene);
    CHECK(sky_share(ground, sky_ref) < sky_ahead);
}

TEST_CASE("the player script walks, turns and looks from the input", "[world]")
{
    EngineScene scene;
    start_running(scene);
    REQUIRE(wait_for(
        [&]
        {
            return local_pose(scene).has_value(); // the script has reported
        }));
    const Pose start = *local_pose(scene);

    // Forward along +Z (yaw 0) at 2.8 m/s.
    MoveInput forward;
    forward.forward = true;
    set_input(scene, forward);
    REQUIRE(wait_for(
        [&]
        {
            return local_pose(scene)->z > start.z + 0.3f;
        }));
    CHECK(local_pose(scene)->speed == Catch::Approx(2.8f));
    CHECK(local_pose(scene)->walking);
    CHECK(local_pose(scene)->x == Catch::Approx(start.x).margin(1e-3));

    // Stopping reports a standing pose.
    set_input(scene, MoveInput{});
    REQUIRE(wait_for(
        [&]
        {
            return local_pose(scene)->speed == 0.0f;
        }));

    // A drag to the right and down turns right (yaw falls) and looks down (pitch falls), once.
    const Pose before = *local_pose(scene);
    MoveInput drag;
    drag.look_dx = 0.1f;
    drag.look_dy = 0.05f;
    set_input(scene, drag);
    drag.look_dx = drag.look_dy = 0;
    set_input(scene, drag);
    REQUIRE(wait_for(
        [&]
        {
            return local_pose(scene)->pitch < before.pitch - 0.04f;
        }));
    CHECK(local_pose(scene)->pitch - before.pitch == Catch::Approx(-0.05f).margin(1e-3));
    float dyaw = std::remainder(local_pose(scene)->yaw - before.yaw, 6.2831853f);
    CHECK(dyaw == Catch::Approx(-0.1f).margin(1e-3));
}

TEST_CASE("the player script keeps the player in the meadow and takes a teleport", "[world]")
{
    EngineScene scene;
    start_running(scene);
    Pose far;
    far.x = 500.0f;
    far.z = -500.0f;
    set_local_pose(scene, far);
    REQUIRE(wait_for(
        [&]
        {
            const auto p = local_pose(scene);
            return p && p->x > 79.0f;
        }));
    CHECK(local_pose(scene)->x == Catch::Approx(80.0f));
    CHECK(local_pose(scene)->z == Catch::Approx(-80.0f));
}

TEST_CASE("EngineScene stops at once while it is still starting", "[world]")
{
    EngineScene scene;
    scene.start(room(320, 180));
    scene.stop();
    CHECK(scene.state() == EngineScene::State::Stopped);
    CHECK(scene.failure() == EngineScene::Failure::None);
}

namespace
{
namespace fs = std::filesystem;
using Source = AssetLoadReport::Source;

struct CacheTempDir
{
    fs::path path;
    CacheTempDir()
    {
        static int n = 0;
        path = fs::temp_directory_path() /
               ("tess_wes_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(n++));
        fs::create_directories(path);
    }
    ~CacheTempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int count_twa(const fs::path& dir, const std::string& prefix)
{
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() == ".twa" && e.path().filename().string().rfind(prefix, 0) == 0)
        {
            ++n;
        }
    }
    return n;
}

fs::path find_twa(const fs::path& dir, const std::string& prefix)
{
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() == ".twa" && e.path().filename().string().rfind(prefix, 0) == 0)
        {
            return e.path();
        }
    }
    return {};
}

// Runs one scene to its first picture; returns false (leaving `skip` set) without a device.
struct RunResult
{
    bool skipped = false;
    std::vector<AssetLoadReport> loads;
    std::uint32_t objects_drawn = 0;
    EngineScene::State state = EngineScene::State::Stopped;
};

RunResult run_once(const std::string& assets_dir, const std::string& cache_dir)
{
    RunResult r;
    EngineScene scene;
    EngineScene::Options options = room(160, 90);
    options.assets_dir = assets_dir;
    options.asset_cache_dir = fs::path(cache_dir);
    scene.start(std::move(options));
    REQUIRE(wait_for(
        [&]
        {
            return scene.state() != EngineScene::State::Starting;
        }));
    if (scene.failure() == EngineScene::Failure::NoDevice)
    {
        r.skipped = true;
        return r;
    }
    r.state = scene.state();
    if (r.state == EngineScene::State::Running)
    {
        REQUIRE(wait_for(
            [&]
            {
                return scene.latest_picture().rgba != nullptr;
            }));
        r.objects_drawn = scene.latest_picture().objects_drawn;
    }
    r.loads = scene.asset_loads();
    scene.stop();
    return r;
}

fs::path copy_assets(const fs::path& into)
{
    const fs::path dst = into / "worlds";
    fs::create_directories(dst);
    for (const char* f : {"robot.glb", "green_field.glb"})
    {
        fs::copy_file(fs::path(EngineScene::default_assets_dir()) / f, dst / f,
                      fs::copy_options::overwrite_existing);
    }
    return dst;
}

} // namespace

TEST_CASE("EngineScene loads from the cache on the second start", "[world][assetcache]")
{
    CacheTempDir tmp;
    const std::string cache = (tmp.path / "cache").string();
    const RunResult a = run_once(EngineScene::default_assets_dir(), cache);
    if (a.skipped)
    {
        SKIP("no graphics device");
    }
    REQUIRE(a.state == EngineScene::State::Running);
    REQUIRE(a.loads.size() == 2);
    CHECK(a.loads[0].name == "robot");
    CHECK(a.loads[0].source == Source::Imported);
    CHECK(a.loads[0].stored);
    CHECK(a.loads[1].name == "field");
    CHECK(a.loads[1].source == Source::Imported);
    CHECK(a.loads[1].stored);

    const RunResult b = run_once(EngineScene::default_assets_dir(), cache);
    REQUIRE(b.state == EngineScene::State::Running);
    REQUIRE(b.loads.size() == 2);
    CHECK(b.loads[0].source == Source::Cache);
    CHECK(b.loads[1].source == Source::Cache);
    CHECK(a.objects_drawn > 0);
    CHECK(b.objects_drawn == a.objects_drawn);
    for (const auto& l : a.loads)
    {
        INFO("imported " << l.name << " in " << l.milliseconds << " ms");
    }
    for (const auto& l : b.loads)
    {
        INFO("from cache " << l.name << " in " << l.milliseconds << " ms");
    }
}

TEST_CASE("EngineScene re-imports only the asset whose file changed", "[world][assetcache]")
{
    CacheTempDir tmp;
    const std::string cache = (tmp.path / "cache").string();
    const fs::path assets = copy_assets(tmp.path);

    const RunResult a = run_once(assets.string(), cache);
    if (a.skipped)
    {
        SKIP("no graphics device");
    }
    REQUIRE(a.loads.size() == 2);

    // robot.glb now holds the field's bytes
    fs::copy_file(assets / "green_field.glb", assets / "robot.glb",
                  fs::copy_options::overwrite_existing);
    const RunResult b = run_once(assets.string(), cache);
    REQUIRE(b.loads.size() == 2);
    CHECK(b.loads[0].source == Source::Imported);
    CHECK(b.loads[1].source == Source::Cache);
    CHECK(count_twa(cache, "robot-") == 1);
    CHECK(count_twa(cache, "field-") == 1);
}

TEST_CASE("EngineScene imports again when the original file comes back", "[world][assetcache]")
{
    CacheTempDir tmp;
    const std::string cache = (tmp.path / "cache").string();
    const fs::path assets = copy_assets(tmp.path);
    const std::string original = slurp(assets / "robot.glb");
    const RunResult a = run_once(assets.string(), cache);
    if (a.skipped)
    {
        SKIP("no graphics device");
    }
    fs::copy_file(assets / "green_field.glb", assets / "robot.glb",
                  fs::copy_options::overwrite_existing);
    (void)run_once(assets.string(), cache);

    // the original comes back: its entry was pruned, so it imports again, then hits
    {
        std::ofstream out(assets / "robot.glb", std::ios::binary | std::ios::trunc);
        out.write(original.data(), static_cast<std::streamsize>(original.size()));
    }
    const RunResult c = run_once(assets.string(), cache);
    REQUIRE(c.loads.size() == 2);
    CHECK(c.loads[0].source == Source::Imported);
    CHECK(c.loads[1].source == Source::Cache);
    const RunResult d = run_once(assets.string(), cache);
    REQUIRE(d.loads.size() == 2);
    CHECK(d.loads[0].source == Source::Cache);
    CHECK(d.loads[1].source == Source::Cache);
}

TEST_CASE("EngineScene falls back to import on a damaged cache file", "[world][assetcache]")
{
    CacheTempDir tmp;
    const std::string cache = (tmp.path / "cache").string();
    const RunResult a = run_once(EngineScene::default_assets_dir(), cache);
    if (a.skipped)
    {
        SKIP("no graphics device");
    }
    const fs::path twa = find_twa(cache, "field-");
    REQUIRE_FALSE(twa.empty());
    fs::resize_file(twa, fs::file_size(twa) / 2);

    const RunResult b = run_once(EngineScene::default_assets_dir(), cache);
    REQUIRE(b.state == EngineScene::State::Running);
    REQUIRE(b.loads.size() == 2);
    CHECK(b.loads[0].source == Source::Cache);
    CHECK(b.loads[1].source == Source::Imported);

    const RunResult c = run_once(EngineScene::default_assets_dir(), cache);
    REQUIRE(c.loads.size() == 2);
    CHECK(c.loads[0].source == Source::Cache);
    CHECK(c.loads[1].source == Source::Cache);
}

TEST_CASE("EngineScene with no cache directory imports every time", "[world][assetcache]")
{
    CacheTempDir tmp;
    const RunResult a = run_once(EngineScene::default_assets_dir(), "");
    if (a.skipped)
    {
        SKIP("no graphics device");
    }
    REQUIRE(a.state == EngineScene::State::Running);
    REQUIRE(a.loads.size() == 2);
    for (const auto& l : a.loads)
    {
        CHECK(l.source == Source::Imported);
        CHECK_FALSE(l.stored);
    }
    CHECK(fs::is_empty(tmp.path));
}

TEST_CASE("the player script jumps and lands again", "[world]")
{
    EngineScene scene;
    start_running(scene);
    REQUIRE(wait_for(
        [&]
        {
            const auto p = local_pose(scene);
            return p && local_height(scene) > 0.0f;
        }));
    // The player drops the last centimetres onto the meadow and stands.
    float standing = 0.0f;
    REQUIRE(wait_for(
        [&]
        {
            const float h = local_height(scene);
            const bool settled = std::fabs(h - standing) < 1e-4f;
            standing = h;
            return settled && h > 1.0f && h < 1.2f;
        }));

    // Space lifts the eye by about v^2 / 2g (3.5 m/s: 0.6 m), then gravity brings it back.
    MoveInput jump;
    jump.jump = true;
    set_input(scene, jump);
    REQUIRE(wait_for(
        [&]
        {
            return local_height(scene) > standing + 0.3f;
        }));
    set_input(scene, MoveInput{});
    CHECK(local_height(scene) < standing + 0.8f);
    REQUIRE(wait_for(
        [&]
        {
            return std::fabs(local_height(scene) - standing) < 0.02f;
        }));
    // Standing is a steady height (the ground), so an idle robot does not bob on the others' screens.
    // (it settles within a few frames of landing; the ground under the start is at 0)
    CHECK(wait_for(
        [&]
        {
            return std::fabs(local_pose(scene)->y) < 0.01f;
        }));
}

TEST_CASE("the daylight cycle lights the meadow by day and darkens it by night", "[world]")
{
    auto mean_brightness = [](double clock, double* sky_blue)
    {
        EngineScene scene;
        scene.start(room(160, 90, clock));
        REQUIRE(wait_for(
            [&]
            {
                return scene.state() != EngineScene::State::Starting;
            }));
        if (scene.failure() == EngineScene::Failure::NoDevice)
        {
            SKIP("no graphics device");
        }
        REQUIRE(scene.state() == EngineScene::State::Running);
        const Picture p = next_pictures(scene, 12);
        double sum = 0;
        for (std::size_t i = 0; i + 3 < p.rgba->size(); i += 4)
        {
            sum += (*p.rgba)[i] + (*p.rgba)[i + 1] + (*p.rgba)[i + 2];
        }
        *sky_blue = top_rows_mean(p)[2];
        return sum / (3.0 * static_cast<double>(p.rgba->size() / 4));
    };
    double noon_sky = 0, midnight_sky = 0, dusk_sky = 0;
    const double noon = mean_brightness(720.0, &noon_sky);
    const double midnight = mean_brightness(0.0, &midnight_sky);
    mean_brightness(1080.0, &dusk_sky); // sunset: the sun is on the horizon
    CHECK(noon > 2.0 * midnight);
    CHECK(midnight_sky < 0.5 * noon_sky); // a dark sky against a bright blue one
    CHECK(noon_sky > 100.0);
    CHECK(dusk_sky < noon_sky);
}

TEST_CASE("the sun and the moon are drawn on the sky", "[world]")
{
    // Looks east (+X, yaw pi/2) at `clock` and counts the pixels of a colour only a disc has.
    auto discs = [](double clock, auto is_disc)
    {
        EngineScene scene;
        EngineScene::Options options = room(160, 90, clock);
        options.scripts[0].params = {
            {"yaw", std::numbers::pi / 2}}; // the player starts looking east
        scene.start(std::move(options));
        REQUIRE(wait_for(
            [&]
            {
                return scene.state() != EngineScene::State::Starting;
            }));
        if (scene.failure() == EngineScene::Failure::NoDevice)
        {
            SKIP("no graphics device");
        }
        REQUIRE(scene.state() == EngineScene::State::Running);
        const Picture p = next_pictures(scene, 20);
        std::size_t n = 0;
        for (std::size_t i = 0; i + 3 < p.rgba->size(); i += 4)
        {
            n += is_disc((*p.rgba)[i], (*p.rgba)[i + 1], (*p.rgba)[i + 2]) ? 1 : 0;
        }
        return n;
    };
    // The sun is a warm white (1, 0.95, 0.75) and the moon a cool one (0.85, 0.9, 1); the day sky
    // is a mid blue, the night sky nearly black.
    auto sun = [](int r, int g, int b)
    {
        return r >= b + 12 && r + g + b > 620;
    };
    auto moon = [](int r, int g, int b)
    {
        return b >= r + 8 && r + g + b > 660;
    };
    CHECK(discs(0.28 * 1440, sun) > 8);  // morning: the sun low in the east
    CHECK(discs(0.0, sun) == 0);         // midnight: it is on the other side of the world
    CHECK(discs(0.78 * 1440, moon) > 8); // the moon rises in the east as the sun sets
    CHECK(discs(0.5 * 1440, moon) == 0); // noon: it is below the ground
}

TEST_CASE("the player cannot walk through a robot, and a robot that moves lets go", "[world]")
{
    EngineScene scene;
    start_running(scene);
    set_robots(scene, {{"a", at(0.0f, 1.5f)}});
    next_pictures(scene);

    MoveInput forward;
    forward.forward = true;
    set_input(scene, forward);
    REQUIRE(wait_for(
        [&]
        {
            return local_pose(scene)->z > 0.5f;
        }));
    // Pushing on for a second: without a body the player would be past the robot by now. Two
    // capsules of radius 0.25 meet when their axes are 0.5 m apart.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const float stopped = local_pose(scene)->z;
    INFO("stopped at " << stopped);
    CHECK(stopped < 1.1f);
    CHECK(stopped > 0.8f);

    // The robot walks away: its body goes with it, and the player follows it up to the same gap.
    set_robots(scene, {{"a", at(0.0f, 4.0f)}});
    REQUIRE(wait_for(
        [&]
        {
            return local_pose(scene)->z > 2.0f;
        }));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(local_pose(scene)->z < 3.6f);
    scene.stop();
}

TEST_CASE("a robot's head tilts up and down with its player's pitch", "[world]")
{
    EngineScene scene;
    scene.watch_robot_joint("Robot", "head");
    start_running(scene);
    // How far the head's forward axis (the joint matrix's z column) points up: sin of the tilt.
    auto head_up = [&](float pitch)
    {
        Pose p = at(0.0f, 1.5f);
        p.pitch = pitch;
        set_robots(scene, {{"a", p}});
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const auto m = scene.robot_joint_matrix("a");
        REQUIRE(m);
        return (*m)[9];
    };
    // The idle clip nods the head a few degrees by itself, so allow that much.
    CHECK(head_up(0.0f) == Catch::Approx(0.0f).margin(0.12));
    CHECK(head_up(0.5f) == Catch::Approx(std::sin(0.5f)).margin(0.12));
    CHECK(head_up(-0.5f) == Catch::Approx(-std::sin(0.5f)).margin(0.12));
    // A head turns only so far: a player looking straight up is a 45 degree tilt.
    CHECK(head_up(1.4f) == Catch::Approx(std::sin(0.7853982f)).margin(0.12));
    scene.stop();
}

TEST_CASE("a nudge slides the player, and a robot still stops it", "[world]")
{
    EngineScene scene;
    start_running(scene);
    const float x0 = local_pose(scene)->x;
    nudge_local(scene, 0.4f, 0.0f);
    REQUIRE(wait_for(
        [&]
        {
            return std::fabs(local_pose(scene)->x - (x0 + 0.4f)) < 0.05f;
        }));
    // It is a slide, not a teleport, and it ends: the player stands again where it was sent.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(local_pose(scene)->x == Catch::Approx(x0 + 0.4f).margin(0.05f));
    CHECK(local_pose(scene)->z == Catch::Approx(0.0f).margin(0.05f));

    // A robot in the way holds it: the push does not go through the body.
    set_robots(scene, {{"a", at(x0 + 0.4f, 0.5f)}});
    next_pictures(scene);
    nudge_local(scene, 0.0f, 0.45f);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK(local_pose(scene)->z < 0.1f);
    scene.stop();
}

TEST_CASE("the player starts and teleports onto the ground under the place", "[world]")
{
    EngineScene scene;
    start_running(scene);
    // Heights of the meadow's ground at three of its vertices (assets/worlds/green_field.glb):
    // a hill, a hollow and a gentle rise.
    struct Place
    {
        float x, z, ground;
    };
    for (const Place place : {Place{-50.0f, 10.0f, 0.770f}, Place{30.0f, -40.0f, -0.568f},
                              Place{-40.0f, -30.0f, 0.362f}})
    {
        set_local_pose(scene, at(place.x, place.z));
        float last = -99.0f;
        int still = 0;
        REQUIRE(wait_for(
            [&]
            {
                const float h = local_height(scene);
                still = std::fabs(h - last) < 1e-4f && h > place.ground + 0.5f ? still + 1 : 0;
                last = h;
                return still >= 5;
            }));
        INFO("at " << place.x << ", " << place.z);
        // Standing on it: the feet are at the ground (a capsule on a slope rests a little higher).
        CHECK(local_pose(scene)->y == Catch::Approx(place.ground).margin(0.12f));
    }
}

TEST_CASE("a robot's screen glows in the dark", "[world]")
{
    EngineScene scene;
    start_running(scene, 0.0); // midnight: only the moon and a little ambient light
    auto red = std::make_shared<std::vector<std::uint8_t>>(640 * 360 * 4, 0);
    for (std::size_t i = 0; i < red->size(); i += 4)
    {
        (*red)[i + 2] = 255; // R of BGRA
        (*red)[i + 3] = 255;
    }
    Pose facing_us = at(0.0f, 1.2f);
    facing_us.yaw = 3.14159265f;
    set_robots(scene, {{"a", facing_us}});
    scene.set_robot_screen_bgra("a", red, 640, 360);
    next_pictures(scene);
    const Picture p = next_pictures(scene);
    std::size_t glowing = 0;
    for (std::size_t i = 0; i + 3 < p.rgba->size(); i += 4)
    {
        glowing += (*p.rgba)[i] > 180 && (*p.rgba)[i + 1] < 90 && (*p.rgba)[i + 2] < 90 ? 1 : 0;
    }
    CHECK(glowing > 300); // a lit screen would be a dark red at this hour
}

// ---- assets/worlds/world.lua: the wire format and the rules of the room ----

namespace
{

// Every frame the room's script sent since the last call (the other events are dropped).
std::vector<SentFrame> sent_frames(EngineScene& scene)
{
    std::vector<SentFrame> out;
    for (const auto& e : scene.take_events())
    {
        if (e.name == "send" && e.value.kind == EventValue::Kind::Bytes)
        {
            const std::vector<std::uint8_t> bytes(e.value.text.begin(), e.value.text.end());
            if (const auto f = read_frame(bytes))
            {
                out.push_back(*f);
            }
        }
    }
    return out;
}

// Where the script last drew participant `id`: x, y, z, yaw, pitch, speed.
std::optional<std::array<double, 6>> drawn(const EngineScene& scene, const std::string& id)
{
    // 'robot_pose' is broadcast for every participant; the latest one may be another's, so ask for
    // the pose through a robot of that id only.
    const auto v = scene.latest_event("robot_pose");
    if (!v || v->kind != EventValue::Kind::Array || v->items.size() < 7 || v->items[0].text != id)
    {
        return std::nullopt;
    }
    return std::array<double, 6>{v->num(1), v->num(2), v->num(3), v->num(4), v->num(5), v->num(6)};
}

void post_frame(EngineScene& scene, const std::string& sender, const std::vector<std::uint8_t>& f)
{
    scene.post_event("frame", tesseract::views::world::frame_event(sender, f.data(), f.size()));
}

} // namespace

TEST_CASE("the room's script sends the local pose often while moving and rarely standing",
          "[world]")
{
    EngineScene scene;
    start_running(scene);
    sent_frames(scene); // what the start sent
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto standing = sent_frames(scene);
    CHECK(standing.size() >= 3); // every 500 ms
    CHECK(standing.size() <= 6);
    for (const SentFrame& f : standing)
    {
        CHECK(f.kind == 1);
        CHECK_FALSE(f.pose.walking);
    }

    tesseract::views::world::MoveInput forward;
    forward.forward = true;
    set_input(scene, forward);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto walking = sent_frames(scene);
    set_input(scene, tesseract::views::world::MoveInput{});
    CHECK(walking.size() >= 12); // about every 50 ms
    CHECK(walking.size() <= 30);
    CHECK(walking.back().pose.z > 1.0f);
    CHECK(walking.back().pose.walking);
    for (std::size_t i = 1; i < walking.size(); ++i)
    {
        CHECK(walking[i].seq == walking[i - 1].seq + 1);
    }
    scene.stop();
}

TEST_CASE("the room's script draws a remote participant from its frames, newest first", "[world]")
{
    EngineScene scene;
    start_running(scene);
    set_robots(scene, {});
    scene.set_robots({"a"});
    scene.post_event("participants", tesseract::views::world::participants_event(kMe, {"a"}));

    // Nothing arrived yet: the participant stands on its spawn circle, 3 m round the origin.
    REQUIRE(wait_for(
        [&]
        {
            return drawn(scene, "a").has_value();
        }));
    {
        const auto d = *drawn(scene, "a");
        CHECK(std::hypot(d[0], d[2]) == Catch::Approx(3.0).margin(0.01));
    }

    Pose p = at(1.5f, 2.5f);
    p.y = 0.25f;
    p.yaw = 1.0f;
    p.pitch = 0.3f;
    post_frame(scene, "a", pose_frame(10, 1000, p));
    REQUIRE(wait_for(
        [&]
        {
            const auto d = drawn(scene, "a");
            return d && std::fabs((*d)[0] - 1.5) < 0.01;
        }));
    const auto d = *drawn(scene, "a");
    CHECK(d[1] == Catch::Approx(0.25).margin(0.001));
    CHECK(d[2] == Catch::Approx(2.5).margin(0.001));
    CHECK(d[3] == Catch::Approx(1.0).margin(0.001));
    CHECK(d[4] == Catch::Approx(0.3).margin(0.001));

    // An older frame, a frame of the wrong size, kind or number, and the local participant's own echo
    // change nothing.
    post_frame(scene, "a", pose_frame(9, 1100, at(-7.0f, -7.0f)));
    post_frame(scene, "a", std::vector<std::uint8_t>(31, 0));
    auto wrong_kind = pose_frame(11, 1200, at(-7.0f, -7.0f));
    wrong_kind[1] = 9;
    post_frame(scene, "a", wrong_kind);
    auto nan = pose_frame(12, 1300, at(-7.0f, -7.0f));
    nan[12] = nan[13] = 0;
    nan[14] = 0xC0;
    nan[15] = 0x7F; // a NaN x
    post_frame(scene, "a", nan);
    post_frame(scene, kMe, pose_frame(13, 1400, at(-7.0f, -7.0f)));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK((*drawn(scene, "a"))[0] == Catch::Approx(1.5).margin(0.01));
    scene.stop();
}

TEST_CASE("the room's script moves a participant out of the way of a body it overlaps", "[world]")
{
    EngineScene scene;
    start_running(scene);
    sent_frames(scene);
    // A robot 0.3 m from us: the bodies overlap by 0.2 m, so we are asked to take half of it and so
    // is the other player.
    scene.set_robots({"zed"});
    scene.post_event("participants", tesseract::views::world::participants_event(kMe, {"zed"}));
    post_frame(scene, "zed",
               pose_frame(1, 1000, at(local_pose(scene)->x + 0.3f, local_pose(scene)->z)));
    std::vector<SentFrame> nudges;
    REQUIRE(wait_for(
        [&]
        {
            for (const SentFrame& f : sent_frames(scene))
            {
                if (f.kind == 2)
                {
                    nudges.push_back(f);
                }
            }
            return !nudges.empty();
        }));
    CHECK(nudges[0].target == "zed");
    CHECK(std::fabs(nudges[0].dx) + std::fabs(nudges[0].dz) > 0.05f);
    CHECK(std::hypot(nudges[0].dx, nudges[0].dz) < 0.2f);
    scene.stop();
}

TEST_CASE("the room's script believes a push only from a close player, once per interval",
          "[world]")
{
    EngineScene scene;
    start_running(scene);
    const float x0 = local_pose(scene)->x, z0 = local_pose(scene)->z;
    scene.set_robots({"zed"});
    scene.post_event("participants", tesseract::views::world::participants_event(kMe, {"zed"}));
    // `zed` stands 0.52 m east of us (not quite touching, so the bodies do not push by themselves) and
    // asks us to move 0.1 m west.
    post_frame(scene, "zed", pose_frame(1, 1000, at(x0 + 0.52f, z0)));
    auto push = [&](std::uint32_t seq, const std::string& target, float dx)
    {
        std::vector<std::uint8_t> f(21 + target.size(), 0);
        f[0] = 1;
        f[1] = 2;
        put32(f, 4, seq);
        put32(f, 8, seq);
        putf(f, 12, dx);
        putf(f, 16, 0.0f);
        f[20] = static_cast<std::uint8_t>(target.size());
        std::copy(target.begin(), target.end(), f.begin() + 21);
        return f;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const float before = local_pose(scene)->x;
    // For somebody else: nothing happens.
    post_frame(scene, "zed", push(2, "@someone:else", -0.1f));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(local_pose(scene)->x == Catch::Approx(before).margin(0.02f));
    // For us: we slide west.
    post_frame(scene, "zed", push(3, kMe, -0.1f));
    REQUIRE(wait_for(
        [&]
        {
            return local_pose(scene)->x < before - 0.05f;
        }));
    // A player nowhere near us cannot push us at all.
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // the slide is walked off
    const float moved = local_pose(scene)->x;
    post_frame(scene, "zed", pose_frame(4, 1100, at(x0 + 40.0f, z0)));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    post_frame(scene, "zed", push(5, kMe, -0.1f));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(local_pose(scene)->x == Catch::Approx(moved).margin(0.02f));
    scene.stop();
}

TEST_CASE("world scripts are sandboxed", "[world]")
{
    namespace fs = std::filesystem;
    CacheTempDir dir;
    for (const char* asset : {"robot.glb", "green_field.glb", "robot.lua"})
    {
        fs::copy_file(fs::path(EngineScene::default_assets_dir()) / asset, dir.path / asset);
    }
    // A probe that reports what a script can reach and beats once a tick, and a script that never
    // gives the CPU back.
    std::ofstream(dir.path / "probe.lua") << R"(
behavior('probe', {
  on_create = function(self)
    self.beats = 0
    event.emit('sandbox', { type(io), type(os), type(package), type(debug), type(ffi) })
  end,
  on_update = function(self)
    self.beats = self.beats + 1
    event.emit('beat', self.beats)
  end,
})
)";
    std::ofstream(dir.path / "spin.lua") << R"(
behavior('spin', {
  on_update = function(self)
    while true do end
  end,
})
)";

    EngineScene::Options options;
    options.assets_dir = dir.path.string();
    options.asset_cache_dir.clear();
    options.field_asset = "green_field.glb";
    options.user_asset = "robot.glb";
    options.user_script = {"robot.lua", "robot", false, {}};
    options.scripts.push_back({"probe.lua", "probe", false, {}});
    options.scripts.push_back({"spin.lua", "spin", false, {}});
    options.events_out = {"sandbox", "beat"};
    options.width = 160;
    options.height = 90;

    EngineScene scene;
    scene.start(options);
    REQUIRE(wait_for(
        [&]
        {
            return scene.state() != EngineScene::State::Starting;
        }));
    if (scene.failure() == EngineScene::Failure::NoDevice)
    {
        SKIP("no graphics device");
    }
    REQUIRE(scene.state() == EngineScene::State::Running);

    // No io, os, package, debug or ffi: every one of them is nil.
    REQUIRE(wait_for(
        [&]
        {
            return scene.latest_event("sandbox").has_value();
        }));
    const EventValue sandbox = *scene.latest_event("sandbox");
    REQUIRE(sandbox.items.size() == 5);
    for (const EventValue& type : sandbox.items)
    {
        CHECK(type.text == "nil");
    }

    // The endless loop ends on its budget, and the scene keeps ticking: the beats keep rising.
    const auto beat = [&]
    {
        const auto v = scene.latest_event("beat");
        return v ? v->number : 0.0;
    };
    REQUIRE(wait_for(
        [&]
        {
            return beat() >= 1;
        }));
    const double before = beat();
    CHECK(wait_for(
        [&]
        {
            return beat() >= before + 5;
        },
        std::chrono::seconds(30)));
    CHECK(scene.state() == EngineScene::State::Running);
    scene.stop();
}

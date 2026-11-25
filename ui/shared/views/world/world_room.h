#pragma once

// The 3D call room as the app sees it: which scripts make the world (assets/worlds/*.lua), and the
// events the app and the scripts exchange. Everything the room does (the walking, the poses on the
// wire, the pushing apart of bodies) is the scripts'; this only names the pieces. Standard library
// and EngineScene only.

#include "views/world/EngineScene.h"

#include <optional>
#include <string>
#include <vector>

namespace tesseract::views::world
{

// Where something stands. y is the feet's height in the world.
struct Pose
{
    float x = 0, y = 0, z = 0;
    float yaw = 0;   // radians, heading vector is (sin yaw, 0, cos yaw)
    float pitch = 0; // radians, up is positive
    float speed = 0; // m/s
    bool walking = false;
};

// What the user is doing to the local player this paint (assets/worlds/player.lua moves them).
struct MoveInput
{
    bool forward = false, back = false;
    bool left = false, right = false;               // turn (arrow keys)
    bool strafe_left = false, strafe_right = false; // sideways (A and D)
    bool jump = false;                              // Space is held
    float look_dx = 0, look_dy = 0; // radians; dx > 0 turns right, dy > 0 looks down
};

// The call room's scenes: the meadow and a robot for each participant, the player, the daylight
// and the world's rules. `day_clock`: seconds into the daylight cycle's day (0 to 1440); unset
// follows the UTC wall clock.
// It is the shipped meadow bundle (assets/worlds/world.json) loaded by options_from_manifest.
EngineScene::Options room_options(std::optional<double> day_clock = std::nullopt);

// The options a world bundle describes: its world.json text (format 1) and the directory holding
// its files, which becomes assets_dir. `day_clock` replaces the numeric param named "clock" of
// every script that has one. On a bad manifest (not JSON, unknown field, a format newer than this
// build reads, an invalid attach or event name, a file path that leaves the bundle) it returns nullopt and
// puts the reason in `error` (may be null).
std::optional<EngineScene::Options> options_from_manifest(const std::string& manifest_json,
                                                          const std::string& bundle_dir,
                                                          std::optional<double> day_clock,
                                                          std::string* error);
// True only when `bundle_dir` is the installed assets directory: its scripts are the app's own.
bool scripts_are_builtin(const std::string& bundle_dir);
// The files the manifest needs, relative to the bundle root, without duplicates: world.json, the
// field and robot assets, every script file. Empty when the manifest is invalid.
std::vector<std::string> bundle_files(const std::string& manifest_json);

// 'input' (to the player script): the held directions and the look deltas since the last one.
EventValue input_event(const MoveInput& input);
// 'participants' (to the world script): the local participant's id, then the others'.
EventValue participants_event(const std::string& local_id, const std::vector<std::string>& others);
// 'frame' (to the world script): bytes that arrived from `sender`.
EventValue frame_event(const std::string& sender, const std::uint8_t* data, std::size_t size);
// The local player's 'pose' (from the player script).
std::optional<Pose> pose_from_event(const EventValue& pose);
// How high the local player's eye is, metres, from the same event.
float eye_height_from_event(const EventValue& pose);

} // namespace tesseract::views::world

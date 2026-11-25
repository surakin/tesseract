#include "views/world/world_room.h"

#include "views/WorldConsentDialog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cctype>
#include <cstdio>

namespace tesseract::views::world
{

namespace
{

constexpr int kFormat = 1;
const char* const kManifestName = "world.json";

using nlohmann::json;

bool fail(std::string* error, const std::string& why)
{
    if (error)
    {
        // The message may quote names the bundle chose.
        *error = sanitize_world_text(why, 200);
    }
    return false;
}

constexpr std::size_t kMaxManifestBytes = 256 * 1024;
constexpr std::size_t kMaxNameBytes = 63; // what the engine keeps of a behaviour or event name
constexpr std::size_t kMaxPathParts = 16;

bool reserved_device_name(std::string part)
{
    part = part.substr(0, part.find('.'));
    while (!part.empty() && (part.back() == ' ' || part.front() == ' '))
    {
        part.erase(part.back() == ' ' ? part.size() - 1 : 0, 1);
    }
    for (char& c : part)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (part == "CON" || part == "PRN" || part == "AUX" || part == "NUL")
    {
        return true;
    }
    return part.size() == 4 && (part.compare(0, 3, "COM") == 0 || part.compare(0, 3, "LPT") == 0) &&
           part[3] >= '1' && part[3] <= '9';
}

// A relative path that stays inside the bundle and is valid on every platform (the same rules as
// the Rust extractor): no empty or '.' / '..' parts, no absolute, drive or backslash forms, no
// control or Windows-invalid characters, device names, trailing dot or space, or deep nesting.
bool safe_relative_path(const std::string& p)
{
    if (p.empty() || p.front() == '/' || p.size() > 255)
    {
        return false;
    }
    std::size_t parts = 0;
    std::size_t start = 0;
    while (start <= p.size())
    {
        std::size_t end = p.find('/', start);
        if (end == std::string::npos)
        {
            end = p.size();
        }
        const std::string part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
            part.back() == ' ' || reserved_device_name(part) || ++parts > kMaxPathParts)
        {
            return false;
        }
        start = end + 1;
    }
    // Invisible, bidi and other characters that make a name read as something else (the same
    // set the consent dialog strips); also refuses malformed UTF-8.
    if (sanitize_world_text(p, p.size() + 1) != p)
    {
        return false;
    }
    return std::none_of(p.begin(), p.end(),
                        [](unsigned char c)
                        {
                            return c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '<' ||
                                   c == '>' || c == '"' || c == '|' || c == '?' || c == '*';
                        });
}

// A name the engine takes whole: it truncates at 63 bytes and cuts at a NUL.
bool sane_name(const std::string& n)
{
    return !n.empty() && n.size() <= kMaxNameBytes &&
           std::none_of(n.begin(), n.end(),
                        [](unsigned char c)
                        {
                            return c < 0x20 || c == 0x7f;
                        });
}

bool read_script(const json& j, ScriptSpec* out, std::string* error)
{
    if (!j.is_object())
    {
        return fail(error, "a script entry is not an object");
    }
    for (const auto& [key, value] : j.items())
    {
        (void)value;
        if (key != "file" && key != "attach" && key != "on_camera" && key != "params")
        {
            return fail(error, "unknown script field \"" + key + "\"");
        }
    }
    if (!j.contains("file") || !j["file"].is_string() || !j.contains("attach") ||
        !j["attach"].is_string())
    {
        return fail(error, "a script needs a \"file\" and an \"attach\"");
    }
    ScriptSpec spec;
    spec.file = j["file"].get<std::string>();
    spec.behavior = j["attach"].get<std::string>();
    if (!safe_relative_path(spec.file))
    {
        return fail(error, "script path \"" + spec.file + "\" is not inside the bundle");
    }
    if (!sane_name(spec.behavior))
    {
        return fail(error, "script \"" + spec.file + "\" has an invalid attach name");
    }
    if (j.contains("on_camera"))
    {
        if (!j["on_camera"].is_boolean())
        {
            return fail(error, "\"on_camera\" must be true or false");
        }
        spec.on_camera = j["on_camera"].get<bool>();
    }
    if (j.contains("params"))
    {
        if (!j["params"].is_object())
        {
            return fail(error, "\"params\" must be an object");
        }
        for (const auto& [key, value] : j["params"].items())
        {
            if (value.is_number())
            {
                spec.params.emplace_back(key, value.get<double>());
            }
            else if (value.is_string())
            {
                spec.params.emplace_back(key, value.get<std::string>());
            }
            else
            {
                return fail(error, "param \"" + key + "\" must be a number or a string");
            }
        }
    }
    *out = std::move(spec);
    return true;
}

} // namespace

std::optional<EngineScene::Options> options_from_manifest(const std::string& manifest_json,
                                                          const std::string& bundle_dir,
                                                          std::optional<double> day_clock,
                                                          std::string* error)
{
    if (manifest_json.size() > kMaxManifestBytes)
    {
        fail(error, "the manifest is too large");
        return std::nullopt;
    }
    const json j = json::parse(manifest_json, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        fail(error, "the manifest is not a JSON object");
        return std::nullopt;
    }
    for (const auto& [key, value] : j.items())
    {
        (void)value;
        if (key != "format" && key != "name" && key != "field_asset" && key != "user_asset" &&
            key != "scripts" && key != "user_script" && key != "events_out")
        {
            fail(error, "unknown manifest field \"" + key + "\"");
            return std::nullopt;
        }
    }
    if (!j.contains("format") || !j["format"].is_number_integer())
    {
        fail(error, "the manifest has no format");
        return std::nullopt;
    }
    if (j["format"].get<long long>() > kFormat)
    {
        fail(error, "the manifest's format is newer than this version of Tesseract understands");
        return std::nullopt;
    }
    if (j["format"].get<long long>() != kFormat)
    {
        fail(error, "unsupported manifest format");
        return std::nullopt;
    }
    if (!j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty())
    {
        fail(error, "the manifest has no name");
        return std::nullopt;
    }
    EngineScene::Options options;
    options.assets_dir = bundle_dir;
    for (const char* key : {"field_asset", "user_asset"})
    {
        if (!j.contains(key) || !j[key].is_string() ||
            !safe_relative_path(j[key].get<std::string>()))
        {
            fail(error, std::string("\"") + key + "\" is missing or not inside the bundle");
            return std::nullopt;
        }
    }
    options.field_asset = j["field_asset"].get<std::string>();
    options.user_asset = j["user_asset"].get<std::string>();
    if (j.contains("scripts"))
    {
        if (!j["scripts"].is_array())
        {
            fail(error, "\"scripts\" must be a list");
            return std::nullopt;
        }
        for (const json& entry : j["scripts"])
        {
            ScriptSpec spec;
            if (!read_script(entry, &spec, error))
            {
                return std::nullopt;
            }
            options.scripts.push_back(std::move(spec));
        }
    }
    if (!j.contains("user_script") || !read_script(j["user_script"], &options.user_script, error))
    {
        if (!j.contains("user_script"))
        {
            fail(error, "the manifest has no user_script");
        }
        return std::nullopt;
    }
    if (j.contains("events_out"))
    {
        if (!j["events_out"].is_array())
        {
            fail(error, "\"events_out\" must be a list");
            return std::nullopt;
        }
        for (const json& name : j["events_out"])
        {
            if (!name.is_string() || !sane_name(name.get<std::string>()))
            {
                fail(error, "\"events_out\" must hold plain names");
                return std::nullopt;
            }
            options.events_out.push_back(name.get<std::string>());
        }
    }
    if (day_clock)
    {
        auto apply = [&](ScriptSpec& s)
        {
            for (auto& [name, value] : s.params)
            {
                if (name == "clock" && std::holds_alternative<double>(value))
                {
                    value = *day_clock;
                }
            }
        };
        for (ScriptSpec& s : options.scripts)
        {
            apply(s);
        }
        apply(options.user_script);
    }
    return options;
}

bool scripts_are_builtin(const std::string& bundle_dir)
{
    namespace fs = std::filesystem;
    std::error_code a, b;
    const fs::path mine = fs::canonical(bundle_dir, a);
    const fs::path builtin = fs::canonical(EngineScene::default_assets_dir(), b);
    return !a && !b && mine == builtin;
}

std::vector<std::string> bundle_files(const std::string& manifest_json)
{
    const auto options = options_from_manifest(manifest_json, "", std::nullopt, nullptr);
    std::vector<std::string> files;
    if (!options)
    {
        return files;
    }
    auto add = [&](const std::string& f)
    {
        if (std::find(files.begin(), files.end(), f) == files.end())
        {
            files.push_back(f);
        }
    };
    add(kManifestName);
    add(options->field_asset);
    add(options->user_asset);
    for (const ScriptSpec& s : options->scripts)
    {
        add(s.file);
    }
    add(options->user_script.file);
    return files;
}

EngineScene::Options room_options(std::optional<double> day_clock)
{
    const std::string dir = EngineScene::default_assets_dir();
    std::ifstream in(std::filesystem::path(dir) / kManifestName, std::ios::binary);
    std::string text(kMaxManifestBytes + 1, '\0'); // one over the cap, so too large is noticed
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    std::string error;
    if (auto options = options_from_manifest(text, dir, day_clock, &error))
    {
        return std::move(*options);
    }
    std::fprintf(stderr, "[world] world.json: %s\n", error.c_str());
    // The shipped bundle is unreadable: an empty world, which the scene reports as failed to load.
    EngineScene::Options empty;
    empty.assets_dir = dir;
    return empty;
}

EventValue input_event(const MoveInput& input)
{
    const double forward = (input.forward ? 1.0 : 0.0) - (input.back ? 1.0 : 0.0);
    const double strafe = (input.strafe_left ? 1.0 : 0.0) - (input.strafe_right ? 1.0 : 0.0);
    const double turn = (input.left ? 1.0 : 0.0) - (input.right ? 1.0 : 0.0);
    return EventValue::array({EventValue::of(forward), EventValue::of(strafe), EventValue::of(turn),
                              EventValue::of(input.look_dx), EventValue::of(input.look_dy),
                              EventValue::of(input.jump ? 1.0 : 0.0)});
}

EventValue participants_event(const std::string& local_id, const std::vector<std::string>& others)
{
    std::vector<EventValue> items;
    items.push_back(EventValue::of(local_id));
    for (const std::string& id : others)
    {
        items.push_back(EventValue::of(id));
    }
    return EventValue::array(std::move(items));
}

EventValue frame_event(const std::string& sender, const std::uint8_t* data, std::size_t size)
{
    return EventValue::array({EventValue::of(sender), EventValue::bytes(data, size)});
}

std::optional<Pose> pose_from_event(const EventValue& v)
{
    if (v.kind != EventValue::Kind::Array || v.items.size() < 7)
    {
        return std::nullopt;
    }
    Pose p;
    p.x = static_cast<float>(v.num(0));
    p.z = static_cast<float>(v.num(1));
    p.yaw = static_cast<float>(v.num(2));
    p.pitch = static_cast<float>(v.num(3));
    p.speed = static_cast<float>(v.num(4));
    p.y = static_cast<float>(v.num(5));
    p.walking = p.speed > 0.0f;
    return p;
}

float eye_height_from_event(const EventValue& v)
{
    return v.kind == EventValue::Kind::Array && v.items.size() >= 7 ? static_cast<float>(v.num(6))
                                                                    : 0.0f;
}

} // namespace tesseract::views::world

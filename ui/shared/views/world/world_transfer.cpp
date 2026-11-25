#include "views/world/world_transfer.h"

#include "tk/i18n.h"
#include "views/WorldConsentDialog.h"
#include "views/world/EngineScene.h"
#include "views/world/world_room.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>

namespace tesseract::views::world
{

namespace fs = std::filesystem;

namespace
{

// The most a zip may be (the SDK refuses to publish more).
constexpr std::uintmax_t kMaxZipBytes = 64ull * 1024 * 1024;
constexpr std::size_t kMaxDirName = 64;
constexpr std::size_t kMaxManifestRead = 256 * 1024;

fs::path to_path(const std::string& utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string from_path(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A directory that is removed with its contents when the scope ends.
struct Scratch
{
    fs::path path; // empty when there is no temporary directory to use
    Scratch()
    {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path(ec);
        if (ec || tmp.empty())
        {
            return;
        }
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = tmp / ("tesseract-world-" + std::to_string(tick) + "-" +
                      std::to_string(counter.fetch_add(1)));
    }
    ~Scratch()
    {
        if (!path.empty())
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

// Whether `s` is well-formed UTF-8 (the SDK takes paths as UTF-8 text and throws on anything else).
bool valid_utf8(const std::string& s)
{
    for (std::size_t i = 0; i < s.size();)
    {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80                   ? 1
                          : (c >= 0xC2 && c <= 0xDF) ? 2
                          : (c >> 4) == 0xE          ? 3
                          : (c >= 0xF0 && c <= 0xF4) ? 4
                                                     : 0;
        if (len == 0 || i + len > s.size())
        {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k)
        {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
            {
                return false;
            }
        }
        i += len;
    }
    return true;
}

bool fail(std::string* error, std::string message)
{
    if (error)
    {
        *error = std::move(message);
    }
    return false;
}

std::string manifest_name(const std::string& manifest_json)
{
    try
    {
        const auto j = nlohmann::json::parse(manifest_json);
        if (j.is_object() && j.contains("name") && j["name"].is_string())
        {
            return j["name"].get<std::string>();
        }
    }
    catch (const nlohmann::json::exception&)
    {
    }
    return {};
}

bool ends_with_zip(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return ext == ".zip";
}

bool windows_reserved(const std::string& name)
{
    // The part before the first dot decides ("con.txt" is the console too).
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ')
    {
        stem.pop_back();
    }
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::toupper(c));
                   });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
    {
        return true;
    }
    if (stem == "CONIN$" || stem == "CONOUT$")
    {
        return true;
    }
    if (stem.size() >= 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0))
    {
        const std::string tail = stem.substr(3);
        // One digit, or a superscript 1, 2 or 3 (Windows treats those as ports too).
        return (tail.size() == 1 && tail[0] >= '0' && tail[0] <= '9') || tail == "\xC2\xB9" ||
               tail == "\xC2\xB2" || tail == "\xC2\xB3";
    }
    return false;
}

} // namespace

std::string safe_world_dir_name(const std::string& name)
{
    std::string out;
    for (std::size_t i = 0; i < name.size();)
    {
        const auto c = static_cast<unsigned char>(name[i]);
        std::size_t len = c < 0x80                   ? 1
                          : (c >= 0xC2 && c <= 0xDF) ? 2
                          : (c >> 4) == 0xE          ? 3
                          : (c >= 0xF0 && c <= 0xF4) ? 4
                                                     : 0;
        bool well_formed = len != 0 && i + len <= name.size();
        for (std::size_t k = 1; well_formed && k < len; ++k)
        {
            well_formed = (static_cast<unsigned char>(name[i + k]) & 0xC0) == 0x80;
        }
        if (!well_formed)
        {
            ++i; // a stray, overlong or truncated byte: dropped, the next byte stands alone
            continue;
        }
        const std::string ch = name.substr(i, len);
        i += len;
        if (len == 1)
        {
            if (c < 0x20 || c == 0x7f ||
                std::string_view("/\\:*?\"<>|").find(static_cast<char>(c)) !=
                    std::string_view::npos)
            {
                continue;
            }
        }
        else if (len == 2 && c == 0xC2 && static_cast<unsigned char>(ch[1]) < 0xA0)
        {
            continue; // C1 controls
        }
        if (len > 1 && sanitize_world_text(ch, 1).empty())
        {
            continue; // zero-width, bidi and other characters that hide in a name
        }
        out += ch;
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' '))
    {
        out.erase(out.begin());
    }
    if (out.size() > kMaxDirName)
    {
        std::size_t cut = kMaxDirName;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
        {
            --cut; // not in the middle of a character
        }
        out.resize(cut);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
    {
        out.pop_back();
    }
    if (out.empty())
    {
        return "world";
    }
    if (windows_reserved(out))
    {
        out.insert(out.begin(), '_');
    }
    return out;
}

TransferOps client_transfer_ops(WithClient with_client)
{
    TransferOps ops;
    ops.pack = [](const std::string& dir)
    {
        return pack_world_bundle(dir);
    };
    ops.extract = [](const std::vector<std::uint8_t>& bytes, const std::string& dest)
    {
        return extract_world_bundle(bytes, dest);
    };
    // Each call reaches the client only for its own duration; when it is gone, the call ended.
    const auto gone = []
    {
        return Result{false, tk::tr("The call has ended.")};
    };
    ops.set_room_world = [with_client, gone](const std::string& room, const std::string& name,
                                             const std::vector<std::uint8_t>& bytes)
    {
        Result r = gone();
        with_client(
            [&](Client& c)
            {
                r = c.set_room_world(room, name, bytes);
            });
        return r;
    };
    ops.room_world = [with_client, gone](const std::string& room)
    {
        RoomWorldRead r;
        r.kind = RoomWorldRead::Kind::Error;
        r.error = gone().message;
        with_client(
            [&](Client& c)
            {
                r = c.room_world(room);
            });
        return r;
    };
    ops.download = [with_client, gone](const RoomWorld& w, std::vector<std::uint8_t>& out)
    {
        Result r = gone();
        with_client(
            [&](Client& c)
            {
                r = c.download_world_bundle(w, out);
            });
        return r;
    };
    ops.meadow_dir = []
    {
        return EngineScene::default_assets_dir();
    };
    return ops;
}

TransferOps client_transfer_ops(Client& client)
{
    return client_transfer_ops(
        [&client](const std::function<void(Client&)>& fn)
        {
            fn(client);
            return true;
        });
}

namespace
{
bool import_world_(const TransferOps& ops, const std::string& room_id, const std::string& path,
                   std::string* error);
bool export_world_(const TransferOps& ops, const std::string& room_id, const std::string& dest_dir,
                   std::string* error, std::string* out_dir);
} // namespace

bool import_world(const TransferOps& ops, const std::string& room_id, const std::string& path,
                  std::string* error)
{
    if (!valid_utf8(path) || !valid_utf8(room_id))
    {
        return fail(error, tk::tr("The name of that folder or file cannot be used."));
    }
    try
    {
        return import_world_(ops, room_id, path, error);
    }
    catch (const std::exception& e)
    {
        return fail(error, tk::trf(tk::tr("The world could not be imported: {0}"), {e.what()}));
    }
}

bool export_world(const TransferOps& ops, const std::string& room_id, const std::string& dest_dir,
                  std::string* error, std::string* out_dir)
{
    if (!valid_utf8(dest_dir) || !valid_utf8(room_id))
    {
        return fail(error, tk::tr("The name of that folder cannot be used."));
    }
    try
    {
        return export_world_(ops, room_id, dest_dir, error, out_dir);
    }
    catch (const std::exception& e)
    {
        return fail(error, tk::trf(tk::tr("The world could not be exported: {0}"), {e.what()}));
    }
}

namespace
{
bool import_world_(const TransferOps& ops, const std::string& room_id, const std::string& path,
                   std::string* error)
{
    std::error_code ec;
    const fs::path p = to_path(path);
    const fs::file_status link = fs::symlink_status(p, ec);
    if (ec || !fs::exists(link))
    {
        return fail(error, tk::tr("That folder or file cannot be read."));
    }
    if (fs::is_symlink(link))
    {
        return fail(error, tk::tr("A shortcut or symbolic link cannot be imported; choose the "
                                  "folder or zip itself."));
    }

    std::vector<std::uint8_t> bytes;
    if (fs::is_directory(link))
    {
        const WorldBundlePack packed = ops.pack(from_path(p));
        if (!packed.ok)
        {
            return fail(error,
                        tk::trf(tk::tr("This folder is not a valid world: {0}"), {packed.error}));
        }
        bytes = packed.bytes;
    }
    else if (fs::is_regular_file(link) && ends_with_zip(p))
    {
        const std::uintmax_t size = fs::file_size(p, ec);
        if (ec)
        {
            return fail(error, tk::tr("That folder or file cannot be read."));
        }
        if (size > kMaxZipBytes)
        {
            return fail(error, tk::tr("This world is too large."));
        }
        std::ifstream in(p, std::ios::binary);
        if (!in)
        {
            return fail(error, tk::tr("That folder or file cannot be read."));
        }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (bytes.size() > kMaxZipBytes)
        {
            return fail(error, tk::tr("This world is too large."));
        }
    }
    else
    {
        return fail(error, tk::tr("Choose a world folder or a .zip file."));
    }

    // Whatever came in is checked as a peer's client would check it, before anything leaves.
    Scratch scratch;
    if (scratch.path.empty())
    {
        return fail(error, tk::tr("There is no temporary folder to check the world in."));
    }
    const WorldBundleExtract checked = ops.extract(bytes, from_path(scratch.path));
    if (!checked.ok)
    {
        return fail(error, tk::trf(tk::tr("This is not a valid world: {0}"),
                                   {sanitize_world_text(checked.error, 200)}));
    }
    // Refuse what no client could load (the loader's own check) before anything is uploaded. The
    // file extraction left is what a loader reads, not the JSON the extractor reported.
    {
        std::string manifest = checked.manifest_json;
        {
            std::ifstream in(scratch.path / "world.json", std::ios::binary);
            if (in)
            {
                std::string text(kMaxManifestRead + 1, '\0');
                in.read(text.data(), static_cast<std::streamsize>(text.size()));
                text.resize(static_cast<std::size_t>(in.gcount()));
                manifest = std::move(text);
            }
        }
        std::string why;
        if (!options_from_manifest(manifest, from_path(scratch.path), std::nullopt, &why))
        {
            return fail(error, tk::trf(tk::tr("This is not a valid world: {0}"),
                                       {sanitize_world_text(why, 200)}));
        }
    }
    std::string name = manifest_name(checked.manifest_json);
    if (name.empty())
    {
        name = tk::tr("World");
    }
    const Result r = ops.set_room_world(room_id, name, bytes);
    if (!r.ok)
    {
        return fail(error, tk::trf(tk::tr("Could not publish the world: {0}"), {r.message}));
    }
    if (ops.trust_published)
    {
        ops.trust_published(world_bundle_hash(bytes));
    }
    return true;
}

} // namespace

bool import_world(Client& client, const std::string& room_id, const std::string& path,
                  std::string* error)
{
    return import_world(client_transfer_ops(client), room_id, path, error);
}

namespace
{
bool export_world_(const TransferOps& ops, const std::string& room_id, const std::string& dest_dir,
                   std::string* error, std::string* out_dir)
{
    std::error_code ec;
    const fs::path dest = to_path(dest_dir);
    if (!fs::is_directory(dest, ec))
    {
        return fail(error, tk::tr("The chosen folder cannot be written to."));
    }

    const RoomWorldRead read = ops.room_world(room_id);
    if (read.kind == RoomWorldRead::Kind::Error)
    {
        return fail(error, tk::trf(tk::tr("Could not read the room's world: {0}"), {read.error}));
    }

    if (read.kind == RoomWorldRead::Kind::World)
    {
        // The bytes come from the room (verified against the event's hash), not from the cache
        // of the loaded world, which may have been pruned.
        const fs::path target = dest / to_path(safe_world_dir_name(read.world.name));
        if (fs::exists(fs::symlink_status(target, ec)))
        {
            return fail(error, tk::trf(tk::tr("\"{0}\" already exists in that folder. Choose "
                                              "another folder or rename it."),
                                       {from_path(target.filename())}));
        }
        std::vector<std::uint8_t> bytes;
        const Result dl = ops.download(read.world, bytes);
        if (!dl.ok)
        {
            return fail(error, tk::trf(tk::tr("Could not download the world: {0}"), {dl.message}));
        }
        // Creates the directory, or leaves nothing behind.
        const WorldBundleExtract ex = ops.extract(bytes, from_path(target));
        if (!ex.ok)
        {
            return fail(error, tk::trf(tk::tr("The world is not valid: {0}"),
                                       {sanitize_world_text(ex.error, 200)}));
        }
        if (out_dir)
        {
            *out_dir = from_path(target);
        }
        return true;
    }

    // No world in the room: the shipped meadow, only the files its manifest names (its folder
    // holds other things).
    const fs::path src = to_path(ops.meadow_dir());
    std::string manifest;
    {
        std::ifstream in(src / "world.json", std::ios::binary);
        if (!in)
        {
            return fail(error, tk::tr("The built-in world could not be found."));
        }
        manifest.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::vector<std::string> files = bundle_files(manifest);
    if (files.empty())
    {
        return fail(error, tk::tr("The built-in world could not be found."));
    }
    for (const std::string& rel : files)
    {
        const fs::path rp = to_path(rel);
        bool inside =
            !rp.empty() && !rp.is_absolute() && !rp.has_root_name() && !rp.has_root_directory();
        for (const fs::path& part : rp)
        {
            inside = inside && part != "..";
        }
        if (!inside)
        {
            return fail(error, tk::tr("The built-in world is not valid."));
        }
    }
    std::string name = manifest_name(manifest);
    const fs::path target = dest / to_path(safe_world_dir_name(name));
    if (fs::exists(fs::symlink_status(target, ec)))
    {
        return fail(error, tk::trf(tk::tr("\"{0}\" already exists in that folder. Choose "
                                          "another folder or rename it."),
                                   {from_path(target.filename())}));
    }
    // create_directory fails when the directory appeared meanwhile: never write into another.
    if (!fs::create_directory(target, ec) || ec)
    {
        return fail(error, tk::tr("The chosen folder cannot be written to."));
    }
    auto abandon = [&](std::string message)
    {
        std::error_code e;
        fs::remove_all(target, e);
        return fail(error, std::move(message));
    };
    for (const std::string& rel : files)
    {
        const fs::path from = src / to_path(rel);
        const fs::path to = target / to_path(rel);
        fs::create_directories(to.parent_path(), ec);
        if (ec || !fs::is_regular_file(fs::symlink_status(from, ec)))
        {
            return abandon(tk::trf(tk::tr("The built-in world is missing {0}."), {rel}));
        }
        fs::copy_file(from, to, fs::copy_options::none, ec);
        if (ec)
        {
            return abandon(tk::tr("The chosen folder cannot be written to."));
        }
    }
    if (out_dir)
    {
        *out_dir = from_path(target);
    }
    return true;
}

} // namespace

bool export_world(Client& client, const std::string& room_id, const std::string& dest_dir,
                  std::string* error, std::string* out_dir)
{
    return export_world(client_transfer_ops(client), room_id, dest_dir, error, out_dir);
}

} // namespace tesseract::views::world

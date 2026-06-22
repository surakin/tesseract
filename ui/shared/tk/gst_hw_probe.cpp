// GStreamer hardware-decoder capability cache.
//
// On first use (or after the 7-day TTL expires), probes each well-known
// hardware decoder element by trying GST_STATE_READY with GStreamer debug
// silenced.  Results are written to <cache_dir>/gst_hw_probe.dat.  On
// subsequent runs the cached "broken" set is applied immediately by demoting
// those factories to GST_RANK_NONE, so GStreamer's auto-plugger (decodebin /
// playbin) never attempts them — and never emits the per-play hardware probe
// errors that gst-libav otherwise prints via av_log on every setSourceDevice.

#include "gst_hw_probe.h"

// Qt defines 'signals' as 'public' for its meta-object system.
// GLib's gdbusintrospection.h declares a struct field named 'signals', which
// would be expanded to 'public' — a C++ keyword — causing a parse error.
// Push/pop the macro around the GLib/GStreamer includes as the standard fix.
#pragma push_macro("signals")
#undef signals
#include <glib.h>
#include <gst/gst.h>
#pragma pop_macro("signals")

#include <cstdint>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace tk::gst {

void ensure_gst_init()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        gst_init(nullptr, nullptr);
        // AV_LOG_QUIET = -8; silence FFmpeg/libav noise emitted via gst-libav.
        // gst_init doesn't load plugins eagerly, so libavutil isn't in the
        // process yet — dlopen it directly with versioned fallbacks.
        //
        // Crash sentinel: create a file before the call; delete it on success.
        // If the process crashes inside av_log_set_level, the file survives and
        // the next run finds it and skips suppression entirely.
        //
        // Prefer XDG_RUNTIME_DIR (mode 0700 — only this user can write there,
        // so no symlink attack is possible). Fall back to /tmp with a uid suffix.
        // In both cases use lstat (no symlink follow) for the existence check,
        // O_CREAT|O_EXCL|O_NOFOLLOW for creation, and lstat-then-unlink for
        // removal, so a symlink planted by another user at the path is always
        // refused rather than followed.
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        const std::string sentinel = xdg
            ? std::string(xdg) + "/tesseract-avlog"
            : "/tmp/tesseract-avlog-" + std::to_string(getuid());

        struct stat st;
        if (lstat(sentinel.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            return;

        int fd = ::open(sentinel.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0)
            ::close(fd);

        void* sym = dlsym(RTLD_DEFAULT, "av_log_set_level");
        if (!sym) {
            static const char* const kNames[] = {
                "libavutil.so",    // unversioned (dev package)
                "libavutil.so.60", // FFmpeg 8.x
                "libavutil.so.59", // FFmpeg 7.x
                "libavutil.so.58", // FFmpeg 6.x
                "libavutil.so.57", // FFmpeg 5.x
                "libavutil.so.56", // FFmpeg 4.x
            };
            for (const char* name : kNames) {
                if (void* h = dlopen(name, RTLD_LAZY | RTLD_GLOBAL)) {
                    sym = dlsym(h, "av_log_set_level");
                    if (sym)
                        break;
                }
            }
        }
        if (sym)
            reinterpret_cast<void(*)(int)>(sym)(-8);

        if (lstat(sentinel.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            ::unlink(sentinel.c_str());
    });
}

namespace {

// Hardware decoder element names to probe.  Only factories that are actually
// installed (gst_element_factory_find succeeds) are recorded in the cache, so
// a future plugin install is detected automatically on the next TTL expiry.
static const char* const kHwDecoders[] = {
    // VA-API (Intel / AMD / Mesa)
    "vaapih264dec", "vaapih265dec", "vaapimpeg2dec",
    "vaapivp8dec",  "vaapivp9dec",  "vaapiav1dec",
    // NVDEC (gst-plugins-bad NVIDIA)
    "nvh264dec",    "nvh265dec",    "nvvp8dec",    "nvvp9dec",   "nvav1dec",
    // VDPAU
    "vdpauh264dec", "vdpaumpeg2dec",
    // CUDA decode
    "cudah264dec",  "cudah265dec",
};

constexpr std::int64_t kTtlSeconds = 7 * 24 * 3600; // 7 days

// Cache file format (plain text, no external parser required):
//   <unix_timestamp>
//   +<working_element>
//   -<broken_element>
//   ...
// Entries for factories that are not installed are omitted entirely.

struct ProbeResult {
    std::vector<std::string> broken; // installed but failed READY transition
    bool valid = false;              // true when cache was read successfully
};

ProbeResult read_cache(const std::string& path)
{
    ProbeResult r;
    std::ifstream f(path);
    if (!f.is_open())
    {
        return r;
    }
    std::string line;
    if (!std::getline(f, line))
    {
        return r;
    }
    std::int64_t ts = 0;
    try { ts = std::stoll(line); }
    catch (...) { return r; }

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (now - ts > kTtlSeconds)
    {
        return r; // stale — caller will re-probe
    }
    while (std::getline(f, line))
    {
        if (line.size() >= 2 && line[0] == '-')
        {
            r.broken.push_back(line.substr(1));
        }
    }
    r.valid = true;
    return r;
}

void write_cache(const std::string& path,
                 const std::vector<std::string>& working,
                 const std::vector<std::string>& broken)
{
    std::ofstream f(path);
    if (!f.is_open())
    {
        return;
    }
    f << static_cast<std::int64_t>(std::time(nullptr)) << '\n';
    for (const auto& name : working) { f << '+' << name << '\n'; }
    for (const auto& name : broken)  { f << '-' << name << '\n'; }
}

void demote(const std::vector<std::string>& broken)
{
    for (const auto& name : broken)
    {
        GstElementFactory* f = gst_element_factory_find(name.c_str());
        if (f)
        {
            gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(f), GST_RANK_NONE);
            gst_object_unref(f);
        }
    }
}

} // namespace

void apply_hw_decoder_cache(const std::string& cache_dir)
{
    ensure_gst_init();

    const std::string cache_path = cache_dir + "/gst_hw_probe.dat";
    ProbeResult cached = read_cache(cache_path);
    if (cached.valid)
    {
        demote(cached.broken);
        return;
    }

    // Cache is absent or stale — probe now.  Suppress GStreamer debug (which
    // also captures av_log via gst-libav) so this first-run probe is silent.
    const GstDebugLevel saved = gst_debug_get_default_threshold();
    gst_debug_set_default_threshold(GST_LEVEL_NONE);

    std::vector<std::string> working;
    std::vector<std::string> broken;

    for (const char* name : kHwDecoders)
    {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (!factory)
        {
            continue; // plugin not installed — omit from cache
        }

        GstElement* el = gst_element_factory_create(factory, nullptr);
        gst_object_unref(factory);
        if (!el)
        {
            broken.push_back(name);
            continue;
        }

        const GstStateChangeReturn ret =
            gst_element_set_state(el, GST_STATE_READY);
        // Always return to NULL before freeing.
        gst_element_set_state(el, GST_STATE_NULL);
        gst_object_unref(el);

        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            broken.push_back(name);
        }
        else
        {
            working.push_back(name);
        }
    }

    gst_debug_set_default_threshold(saved);

    write_cache(cache_path, working, broken);
    demote(broken);
}

} // namespace tk::gst

// Win32 audio backend for tk::AudioPlayer. Uses IMFMediaEngine (audio-only
// flag) with an in-memory IStream source so voice payloads never spill to
// disk. Progress callbacks ride on a CreateTimerQueueTimer that fires every
// 60 ms on a thread-pool thread; each callback marshals through a
// post_to_ui funnel back to the UI thread before touching widget state.
//
// Safety model:
//   - All public methods are called on the UI thread.
//   - The timer callback fires on a thread-pool thread; it captures
//     `alive_` (shared_ptr<atomic<bool>>) and `on_progress` by value so the
//     posted lambda is self-contained even if the player is destroyed first.
//   - The destructor sets *alive_=false BEFORE calling stop_timer(), which
//     blocks (INVALID_HANDLE_VALUE) until any in-flight callback returns.
//   - IMFMediaEngineNotify::EventNotify fires on an MF worker thread and
//     follows the same capture pattern.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>     // SHCreateMemStream
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include "audio.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

namespace tk::win32 {

using PostFn = std::function<void(std::function<void()>)>;

// ─────────────────────────────────────────────────────────────────────────
//  One-shot MFStartup — called lazily the first time a player is created.
//  MFShutdown is intentionally omitted: the process exit reclaims MF state,
//  and calling it from a static destructor (after OleUninitialize) causes
//  ordering problems.
// ─────────────────────────────────────────────────────────────────────────
namespace {
void ensure_mf_started() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
    });
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  IMFMediaEngineNotify implementation
// ─────────────────────────────────────────────────────────────────────────
class MediaEngineNotify final : public IMFMediaEngineNotify {
public:
    MediaEngineNotify(std::shared_ptr<std::atomic<bool>> alive, PostFn post)
        : alive_(std::move(alive)), post_(std::move(post)) {}

    // Called by Win32AudioPlayer immediately after construction.
    void set_progress(std::function<void()> cb) { progress_ = std::move(cb); }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown ||
            riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMFMediaEngineNotify — fires on an MF worker thread.
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD  event,
                                          DWORD_PTR /*param1*/,
                                          DWORD  /*param2*/) override {
        if (event != MF_MEDIA_ENGINE_EVENT_ENDED &&
            event != MF_MEDIA_ENGINE_EVENT_ERROR  &&
            event != MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE)
            return S_OK;

        // Capture by value — the player may be destroyed before this lambda
        // runs on the UI thread.
        auto alive = alive_;
        auto cb    = progress_;
        post_([alive = std::move(alive), cb = std::move(cb)]() {
            if (*alive && cb) cb();
        });
        return S_OK;
    }

private:
    std::shared_ptr<std::atomic<bool>> alive_;
    PostFn                             post_;
    std::function<void()>              progress_;
    std::atomic<ULONG>                 ref_{1};
};

// ─────────────────────────────────────────────────────────────────────────
//  Win32AudioPlayer
// ─────────────────────────────────────────────────────────────────────────
class Win32AudioPlayer final : public tk::AudioPlayer {
public:
    explicit Win32AudioPlayer(PostFn post)
        : post_(std::move(post))
        , alive_(std::make_shared<std::atomic<bool>>(true))
    {
        ensure_mf_started();
        init_engine();
    }

    ~Win32AudioPlayer() override {
        *alive_ = false;
        stop_timer();
        if (engine_) {
            engine_->Pause();
            engine_->Shutdown();
            engine_->Release();
            engine_ = nullptr;
        }
        if (notify_) {
            notify_->set_progress(nullptr);
            notify_->Release();
            notify_ = nullptr;
        }
    }

    // ── Playback controls ────────────────────────────────────────────────

    void play(const std::uint8_t* data,
              std::size_t          size,
              std::string_view     mime) override {
        if (!engine_ || !data || size == 0) return;
        stop_timer();
        engine_->Pause();

        // Wrap bytes in an IStream (SHCreateMemStream copies the buffer).
        IStream* raw_stream = SHCreateMemStream(
            reinterpret_cast<const BYTE*>(data),
            static_cast<UINT>(size));
        if (!raw_stream) return;
        Microsoft::WRL::ComPtr<IStream> stream;
        stream.Attach(raw_stream);

        Microsoft::WRL::ComPtr<IMFByteStream> mf_stream;
        if (FAILED(MFCreateMFByteStreamOnStream(stream.Get(),
                                                mf_stream.GetAddressOf())))
            return;

        // Build a URL whose extension carries the MIME type hint.  The
        // engine uses it to select the right parser; the bytes are read
        // from the byte stream, not fetched over HTTP.
        std::wstring url = build_url(mime);
        BSTR burl = SysAllocString(url.c_str());
        if (!burl) return;

        Microsoft::WRL::ComPtr<IMFMediaEngineEx> engine_ex;
        if (SUCCEEDED(engine_->QueryInterface(engine_ex.GetAddressOf()))) {
            engine_ex->SetSourceFromByteStream(mf_stream.Get(), burl);
        }
        SysFreeString(burl);

        engine_->SetPlaybackRate(static_cast<double>(rate_));
        engine_->Play();
        start_timer();
    }

    void pause() override {
        if (engine_) engine_->Pause();
        stop_timer();
        if (on_progress) on_progress();
    }

    void resume() override {
        if (engine_) {
            engine_->SetPlaybackRate(static_cast<double>(rate_));
            engine_->Play();
        }
        start_timer();
    }

    void stop() override {
        stop_timer();
        if (engine_) {
            engine_->Pause();
            engine_->SetCurrentTime(0.0);
        }
        if (on_progress) on_progress();
    }

    void seek(std::uint64_t ms) override {
        if (!engine_) return;
        double target = static_cast<double>(ms) / 1000.0;
        double dur    = engine_->GetDuration();
        if (dur > 0.0 && target > dur) target = dur;
        if (target < 0.0) target = 0.0;
        engine_->SetCurrentTime(target);
        if (on_progress) on_progress();
    }

    void set_playback_rate(float rate) override {
        if (rate < 0.5f) rate = 0.5f;
        if (rate > 3.0f) rate = 3.0f;
        rate_ = rate;
        if (engine_) engine_->SetPlaybackRate(static_cast<double>(rate_));
    }
    float playback_rate() const override { return rate_; }

    std::uint64_t position_ms() const override {
        if (!engine_) return 0u;
        double t = engine_->GetCurrentTime();
        return (t > 0.0) ? static_cast<std::uint64_t>(t * 1000.0) : 0u;
    }
    std::uint64_t duration_ms() const override {
        if (!engine_) return 0u;
        double d = engine_->GetDuration();
        // GetDuration returns NaN or infinity when duration is unknown.
        return (d > 0.0 && d < 1.0e10) ? static_cast<std::uint64_t>(d * 1000.0) : 0u;
    }
    bool is_playing() const override {
        return engine_ && !engine_->IsPaused() && !engine_->IsEnded();
    }

private:
    void init_engine() {
        notify_ = new MediaEngineNotify(alive_, post_);
        notify_->set_progress([this]() { if (on_progress) on_progress(); });

        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.GetAddressOf(), 3))) return;
        attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK,     notify_);
        attrs->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND, 0);

        Microsoft::WRL::ComPtr<IMFMediaEngineClassFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory,
                                    nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(factory.GetAddressOf()))))
            return;

        Microsoft::WRL::ComPtr<IMFMediaEngine> engine;
        if (FAILED(factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY,
                                            attrs.Get(),
                                            engine.GetAddressOf())))
            return;

        engine_ = engine.Detach();
    }

    void start_timer() {
        if (timer_) return;
        BOOL ok = CreateTimerQueueTimer(
            &timer_,
            nullptr,          // default timer queue
            timer_callback,
            this,
            60,               // initial delay (ms)
            60,               // repeat period (ms)
            WT_EXECUTEDEFAULT);
        if (!ok) timer_ = nullptr;
    }

    void stop_timer() {
        if (!timer_) return;
        // INVALID_HANDLE_VALUE: block until any in-flight callback returns.
        // Safe to call from the UI thread since the callback only calls
        // PostMessageW (non-blocking) and returns immediately.
        DeleteTimerQueueTimer(nullptr, timer_, INVALID_HANDLE_VALUE);
        timer_ = nullptr;
    }

    static void CALLBACK timer_callback(PVOID ctx, BOOLEAN /*fired*/) {
        auto* self = static_cast<Win32AudioPlayer*>(ctx);
        // `self` is valid: stop_timer() blocks until this function returns.
        // Capture by value so the posted lambda is safe even if the player
        // is destroyed before the message is dispatched.
        auto alive = self->alive_;
        auto cb    = self->on_progress;
        self->post_([alive = std::move(alive), cb = std::move(cb)]() {
            if (*alive && cb) cb();
        });
    }

    static std::wstring build_url(std::string_view mime) {
        // Extract a file extension from the MIME type to help MF choose a
        // demuxer.  "audio/ogg" → L"audio://voice.ogg", "" → .ogg fallback.
        std::string ext;
        if (!mime.empty()) {
            auto slash = mime.rfind('/');
            ext = (slash != std::string_view::npos)
                ? std::string(mime.substr(slash + 1))
                : std::string(mime);
            auto semi = ext.find(';');
            if (semi != std::string::npos) ext.resize(semi);
            // trim trailing whitespace
            while (!ext.empty() && ext.back() == ' ') ext.pop_back();
        }
        if (ext.empty()) ext = "ogg";

        std::wstring url = L"audio://voice.";
        for (char c : ext) url += static_cast<wchar_t>(c);
        return url;
    }

    PostFn                             post_;
    std::shared_ptr<std::atomic<bool>> alive_;
    IMFMediaEngine*                    engine_ = nullptr;
    MediaEngineNotify*                 notify_ = nullptr;
    HANDLE                             timer_  = nullptr;
    float                              rate_   = 1.0f;
};

// ─────────────────────────────────────────────────────────────────────────
//  Factory
// ─────────────────────────────────────────────────────────────────────────
std::unique_ptr<tk::AudioPlayer>
make_audio_player_win32(PostFn post) {
    return std::make_unique<Win32AudioPlayer>(std::move(post));
}

} // namespace tk::win32

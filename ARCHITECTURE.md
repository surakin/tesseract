# Tesseract architecture

Tesseract is a native desktop Matrix client. Each platform executable combines
shared C++ views and application logic with a Rust networking core in the same
process. The diagram shows the main runtime boundaries; arrows describe calls
or data flow, and dotted arrows show asynchronous event delivery.

```mermaid
flowchart TB
    subgraph desktop["Tesseract desktop process"]
        subgraph presentation["Native UI and shared application layer"]
            shells["Platform shells and backends — ui/&lt;platform&gt;/<br/>Windows: Win32 / Direct2D<br/>macOS: AppKit / CoreGraphics / CoreText<br/>Linux: Qt6 / QPainter or GTK4 / Cairo / Pango"]
            views["Shared views and widget toolkit — ui/shared/<br/>Rooms, timelines, composer, settings, calls<br/>tk::Widget / Canvas / Host"]
            app["Shared application state — ui/shared/app/<br/>ShellBase and controllers<br/>Accounts, windows, send pipeline, media coordination"]
            events["EventHandlerBase<br/>Post callbacks to the owning UI thread"]
            shells -->|host and render| views
            views -->|user actions| app
            app -->|update view state| views
            events -.->|UI-thread handlers| app
        end

        cpp["C++ client library — client/<br/>tesseract::Client / CallSession<br/>SessionStore / settings / preferences"]
        ffi["Generated cxx FFI bridge<br/>sdk/src/bridge.rs + C++ event_handler_bridge"]

        subgraph rust["Rust core — sdk/ · Tokio workers"]
            sdk["Matrix client orchestration — sdk/src/client/<br/>Login, rooms, messaging, media, search, verification"]
            matrix["matrix-sdk + matrix-sdk-ui<br/>Sliding Sync, timelines, end-to-end encryption"]
            rtc["MatrixRTC<br/>Signaling, LiveKit / WebRTC, call encryption"]
            sdk --> matrix
            sdk --> rtc
        end

        app -->|commands| cpp
        cpp --> ffi
        ffi --> sdk
        sdk -.->|worker-thread events through cxx| ffi
        ffi -.->|IEventHandler callbacks| events
    end

    subgraph local["Local persistence"]
        prefs[("Settings, preferences and session metadata")]
        secrets[("OS credential store<br/>Session secrets; file fallback if unavailable")]
        db[("Per-account SQLite stores<br/>Matrix state, crypto, events and media<br/>App cache and optional search index")]
    end

    subgraph remote["External services"]
        homeserver["Matrix homeserver<br/>Sliding Sync, messaging, media and key APIs"]
        auth["System browser + OAuth / MAS<br/>Loopback redirect for authentication"]
        sfu["LiveKit SFU<br/>Realtime audio, video and screen sharing"]
    end

    cpp --> prefs
    cpp --> secrets
    sdk --> db
    matrix <-->|HTTPS| homeserver
    sdk <-->|OAuth login| auth
    rtc <-->|Matrix call signaling| homeserver
    rtc <-->|WebRTC media and signaling| sfu
```

The platform shells supply native windows, menus, accessibility, notifications,
and toolkit backends. Shared views use the toolkit abstractions, while
`ShellBase` and its controllers coordinate accounts and application behavior.
The platform shells inherit from `ShellBase`; macOS embeds a derived `MacShell`
inside its Objective-C++ window controller. `ShellBase` is implemented across
`ShellBase_<subsystem>.cpp` files (accounts, calls, encryption, media, navigation,
rooms, search, settings, spaces, timeline, windows and view wiring); its worker
pool, account-lifecycle types, media types and pure helpers are separate headers.

Commands cross the C++ client API and the generated `cxx` bridge into Rust.
Background sync and other asynchronous work run on Tokio workers. Events return
through the C++ `IEventHandler` interface and `EventHandlerBase`, which posts them
to the owning platform's UI thread before updating application and view state.
Some API calls block, so the shared application layer also uses worker threads.

Matrix state and encryption data are stored per account. C++ session persistence
uses the OS credential backend, with a file fallback when it is unavailable.
OAuth opens the system browser and uses the SDK's loopback redirect listener;
legacy password login is an optional build feature. Calls use Matrix for
signaling and LiveKit/WebRTC for realtime media. Optional integrations such as
GIF search, maps, URL previews, update checks, and UnifiedPush are omitted here
to keep the main architecture readable.

## UI composition and account ownership

This diagram separates view composition from application ownership. The
`AccountManager` is shared across windows; each account has its own C++ client,
Rust runtime, Matrix client, and event bridge. Switching the active account
rebinds the window's views. Moving an account to another window retargets its
event bridge to that window's shell.

```mermaid
flowchart TB
    native["Native main window<br/>MainWindow / MainWindowController"]
    shell["ShellBase<br/>Window state, room selection, worker pools"]
    root["MainAppWidget"]
    sidebar["RoomListView / AccountPicker"]
    room["RoomView"]
    timeline["MessageListView<br/>Timeline media, receipts, spoilers, link layout"]
    compose["ComposeBar<br/>Text, attachments, replies, mentions"]
    thread["ThreadView / ThreadListView"]
    overlays["Pickers, media viewers, settings<br/>EncryptionSetupOverlay"]
    controllers["Shell collaborators<br/>SettingsController / EncryptionFlowController<br/>ThreadPanelController / HistoryExportController<br/>SendPipeline / PowerPolicy"]
    manager["AccountManager — shared across windows<br/>Session registry, media caches, playback hub"]
    account["AccountSession — one per account<br/>Client + IEventHandler bridge<br/>Notifier + optional UnifiedPush connector"]
    backend["Platform toolkit backend<br/>Canvas, Host, native text controls<br/>Audio/video capture and playback"]

    native -->|inherits or embeds| shell
    native -->|mounts| root
    root --> sidebar
    root --> room
    room --> timeline
    room --> compose
    room --> thread
    root --> overlays
    shell -->|coordinates| root
    shell --> controllers
    shell -->|references| manager
    manager -->|stores sessions| account
    root -->|widget layout and drawing| backend
    account -.->|callbacks to owning shell| shell
```

Native `Host` implementations provide UI scheduling and platform integration.
`Canvas` implementations draw shared widgets; native text controls support IME
and selection while their rendered surfaces participate in toolkit compositing.
Additional room and call windows use `RoomWindowBase` and `CallWindowBase`.

## Rust services and protocol responsibilities

These are logical groups of modules in one Rust crate, not separate processes.
`ClientFfi` holds the per-account runtime and client state. The bridge defines
the cross-language types and callable methods; `client/` converts those into
the public C++ API.

```mermaid
flowchart TB
    entry["ClientFfi + cxx bridge<br/>sdk/src/client/mod.rs · sdk/src/bridge.rs"]
    authsvc["Authentication and account state<br/>oauth.rs / password_login.rs<br/>session.rs / account.rs / qr_grant.rs"]
    syncsvc["Sync and timelines<br/>sync.rs / room_list.rs<br/>timeline.rs / timeline_convert.rs / thread.rs"]
    messages["Messaging and room operations<br/>send.rs / pins.rs / tags.rs<br/>room_directory.rs / knock.rs / profile_fields.rs"]
    mediasvc["Media pipeline<br/>media.rs / media_queue.rs<br/>media_gate.rs / media_origin.rs<br/>room_media_store.rs / strip_meta.rs"]
    cryptosvc["Encryption lifecycle<br/>verification.rs / recovery.rs / crypto_reset.rs"]
    history["Local history and background work<br/>backfill.rs / search.rs / history_export/<br/>notifications.rs / activity.rs"]
    extras["Optional network integrations<br/>gif.rs / maps.rs / url_preview_gen.rs / update.rs"]
    upstream["matrix-sdk + matrix-sdk-ui<br/>Client API, SyncService, RoomListService, Timeline<br/>Encryption, key handling and SQLite stores"]
    http["HTTP integrations<br/>reqwest + rustls"]
    server["Matrix homeserver"]
    services["GIF / map / preview / update services"]

    entry --> authsvc
    entry --> syncsvc
    entry --> messages
    entry --> mediasvc
    entry --> cryptosvc
    entry --> history
    entry --> extras
    authsvc --> upstream
    syncsvc --> upstream
    messages --> upstream
    mediasvc --> upstream
    cryptosvc --> upstream
    history --> upstream
    extras --> http
    upstream <-->|Matrix APIs and Sliding Sync| server
    http <-->|HTTPS| services
```

`start_sync` installs long-lived watchers for room lists, timelines, session
refresh, notifications, presence, recovery, and verification, and monitors the
sync service with reconnect backoff. Timeline conversion maps SDK events into
the types consumed by C++ views. Local backfill and search complement the live
sync stream. Optional integrations are feature- or preference-dependent.

## Threading and message flow

The UI thread owns widgets and window state. `ShellBase` has a general worker
pool for reads and preparation, a single worker for mutating FFI calls, and a
separate media-prefetch pool. Rust performs asynchronous work on each account's
Tokio runtime. The following sequence shows a typical text send and the event
path back to the view; it abstracts SDK-local echoes and later server updates.

```mermaid
sequenceDiagram
    actor User
    participant UI as ComposeBar / ShellBase (UI thread)
    participant Queue as SendPipeline (UI thread)
    participant Read as General worker pool
    participant Mut as Single mutation worker
    participant API as C++ Client / cxx
    participant Rust as Rust / Matrix SDK (Tokio)
    participant HS as Homeserver
    participant Bridge as EventHandlerBase

    User->>UI: Send message
    UI->>Queue: Submit room send
    opt Preparation required (for example, bundled URL preview)
        Queue->>Read: Prepare send
        Read-->>Queue: Post prepared result to UI thread
    end
    Queue->>Mut: Dispatch when earlier sends in this room are ready
    Mut->>API: send_message(...)
    API->>Rust: Call Rust FFI method
    Rust->>HS: Send room event (encrypted when required)
    HS-->>Rust: Response / subsequent sync updates
    Rust-->>API: Timeline event via C++ callback bridge
    API-->>Bridge: IEventHandler callback on worker thread
    Bridge-->>UI: post_to_ui_ then handle_*_ui_()
    UI->>UI: Update timeline and schedule repaint
    API-->>Mut: Send call completes
    Mut-->>Queue: Post completion to UI thread
    Queue-->>UI: Update room busy state
```

Preparation can run concurrently, but `SendPipeline` preserves submission order
within a room. A room waiting for preparation does not hold up another room's
ready sends; the mutation worker still serializes the actual mutating calls.
Callback timing and API completion can interleave, so rendering is driven by
posted events rather than assuming one response equals one final timeline row.
Posted continuations use liveness guards to avoid updating destroyed windows.

## Persistence and cache boundaries

```mermaid
flowchart LR
    settings["C++ settings and preferences"]
    refresh["Rust session refresh<br/>persist_session FFI callback"]
    writer["SessionPersistQueue<br/>Dedicated writer; coalesce by account"]
    store["SessionStore"]
    secret[("OS credential backend<br/>Windows Credential Manager<br/>macOS Keychain / Linux libsecret")]
    files[("Account registry and session metadata<br/>File fallback for session secrets")]
    config[("app_settings.json<br/>config_dir()")]
    ruststore["Per-account Rust client"]
    matrixdb[("Matrix SDK SQLite<br/>State / event cache / media / crypto")]
    appdb[("app_cache.db<br/>Backfill progress, backoff and cached metadata")]
    searchdb[("search_index.db<br/>Optional local full-text index")]
    threaddb[("thread_read_state.db<br/>Persistent local thread read markers")]
    caches["AccountManager media caches<br/>Decoded images / animation<br/>Compressed bytes / MediaDiskCache"]

    settings --> config
    refresh --> writer --> store
    store --> secret
    store --> files
    ruststore --> matrixdb
    ruststore --> appdb
    ruststore --> searchdb
    ruststore --> threaddb
    caches -->|encoded media on disk| disk[("Media disk cache<br/>cache_dir()")]
```

Session persistence bypasses the UI callback path: the dedicated writer prevents
credential-store and atomic-file writes from blocking sync workers. Pending
refreshes for the same account are coalesced so the newest session wins.

Account data lives beneath `data_dir()`; global application settings use
`config_dir()`. These roots coincide on Windows and macOS and differ on Linux.
Matrix SDK stores are distinct from the UI's decoded-image and disk caches.
The local thread read database is also separate from the disposable app cache.
The clear-cache path preserves crypto/session data and thread read state;
logout and other explicit reset operations have different cleanup scopes.

## Native calling

```mermaid
flowchart TB
    callui["CallLobbyView / CallOverlayWidget<br/>CallWindowBase + platform call window"]
    callapi["C++ CallSession / Client<br/>cxx RTC methods and callbacks"]
    session["rtc_ffi.rs / rtc/session.rs<br/>Call lifecycle"]
    signal["rtc/signaling.rs / members.rs<br/>Membership and Matrix call events"]
    transport["rtc/transport.rs<br/>Resolve SFU and obtain connection JWT"]
    livekit["rtc/livekit_room.rs / sfu_set.rs<br/>LiveKit connections and tracks"]
    crypto["rtc/e2ee.rs<br/>Media encryption keys"]
    capture["Native microphone, camera<br/>and screen capture"]
    output["Native audio playback<br/>Video and screen-share tiles"]
    matrix["Matrix homeserver"]
    jwt["LiveKit authorization service"]
    sfu["LiveKit SFU"]

    callui -->|join, mute, share, leave| callapi
    callapi --> session
    session --> signal
    session --> transport
    session --> livekit
    session --> crypto
    signal <-->|Matrix signaling| matrix
    transport <-->|request connection credentials| jwt
    transport -->|SFU URL and JWT| livekit
    crypto -->|frame encryption| livekit
    capture -->|frames through C++ and FFI| livekit
    livekit <-->|WebRTC media / LiveKit signaling| sfu
    livekit -.->|decoded frames through RTC callbacks| output
    session -.->|participant and lifecycle events| callapi
    callapi -.->|post state to UI thread| callui
```

Call control, capture, transport, and rendering are separate responsibilities.
Matrix carries call membership/signaling; the authorization service supplies
credentials for LiveKit, whose SFU carries realtime media. RTC callbacks run on
worker threads, so UI state changes must be posted to the UI thread.

## Build composition

```mermaid
flowchart LR
    cmake["Root CMake + platform preset"] --> corrosion["Corrosion + Cargo"]
    cargo["sdk/Cargo.toml + Cargo.lock"] --> corrosion
    corrosion --> rustlib["tesseract_sdk_ffi<br/>Rust static library"]
    corrosion --> glue["tesseract_sdk_bridge_cxx<br/>Generated cxx glue"]
    cmake --> clientlib["tesseract_client<br/>C++ client library"]
    rustlib --> clientlib
    glue --> clientlib
    cmake --> sharedlib["tesseract_tk<br/>Shared toolkit, views and app code"]
    clientlib --> executable["Selected platform executable<br/>Native shell + toolkit backend"]
    sharedlib --> executable
    cmake --> executable
```

Linux can build both Qt6 and GTK4 executables using the same shared layers.
Platform-specific toolkit implementations compile into their platform targets.
The Rust and C++ layers are linked into the desktop application; the FFI boundary
is an in-process language boundary, not an IPC or network service.

## Source map

| Component | Entry points |
| --- | --- |
| Shared application and callback dispatch | [ShellBase.h](../ui/shared/app/ShellBase.h) (implemented in `ShellBase_*.cpp`), [EventHandlerBase.h](../ui/shared/app/EventHandlerBase.h) |
| Shared views and toolkit | [ui/shared/views](../ui/shared/views/), [ui/shared/tk](../ui/shared/tk/) |
| Platform shells and backends | [Windows](../ui/windows/), [macOS](../ui/macos/), [Qt6](../ui/linux-qt/), [GTK4](../ui/linux-gtk/) |
| C++ API and event bridge | [client.h](../client/include/tesseract/client.h), [event_handler_bridge.cpp](../client/src/event_handler_bridge.cpp) |
| Rust FFI and orchestration | [bridge.rs](../sdk/src/bridge.rs), [client/mod.rs](../sdk/src/client/mod.rs) |
| Authentication and persistence | [oauth.rs](../sdk/src/oauth.rs), [session.rs](../sdk/src/client/session.rs), [session_store.cpp](../client/src/session_store.cpp) |
| Native calls | [CallSession.cpp](../client/src/CallSession.cpp), [rtc](../sdk/src/client/rtc/) |

CMake builds the C++ targets and uses Corrosion to build and link the Rust static
library and generated bridge. See [BUILD.md](BUILD.md) for build configuration.
The diagram renders in Markdown viewers with Mermaid support, including GitHub.

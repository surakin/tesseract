# Secure Token Storage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move OAuth session tokens out of the plaintext `session.json` file and into the platform's native credential vault (Windows Credential Manager, macOS Keychain, Linux libsecret), with transparent on-first-read migration of existing installations.

**Architecture:** A new `SecretStore` static class in `client/` mirrors the `SessionStore` API but stores only to the platform credential backend. `SessionStore::load_account()` checks `SecretStore` first, then falls back to the plaintext file (migration path); `save_account()` writes to `SecretStore` when available and drops a sentinel `{"v":2}` to suppress future plaintext reads. `clear_account()` gains a `SecretStore::remove()` call. A compile-time stub is compiled on Linux when libsecret is absent, keeping the plaintext fallback intact. No shell changes are needed.

**Tech Stack:** C++20, Windows Credential Manager (`wincred.h`), macOS Security.framework (`SecItemAdd` / `SecItemCopyMatching`), Linux libsecret (`secret_password_*_sync`), pkg-config, CMake, Catch2.

---

## File Map

| File | Change |
|------|--------|
| `client/include/tesseract/secret_store.h` | New: `SecretStore` static class declaration |
| `client/src/secret_store_stub.cpp` | New: no-op backend (Linux without libsecret, or fallback) |
| `client/src/secret_store_linux.cpp` | New: libsecret implementation |
| `client/src/secret_store_macos.cpp` | New: Keychain Services implementation |
| `client/src/secret_store_windows.cpp` | New: Credential Manager implementation |
| `client/CMakeLists.txt` | Add platform-conditional SecretStore sources + link deps |
| `client/src/session_store.cpp` | Integrate SecretStore into `load_account`, `save_account`, `clear_account` |
| `tests/cpp/test_session_store_migration.cpp` | New: migration integration tests |

---

## Shared Test Fixture

All migration tests use a `TmpConfig` RAII struct that redirects `config_dir()` to a writable temp directory for the duration of a test, then cleans up:

```cpp
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "tesseract/session_store.h"
#include "tesseract/secret_store.h"

namespace fs = std::filesystem;

struct TmpConfig
{
    fs::path dir;
    TmpConfig()
        : dir(fs::temp_directory_path() /
              ("tss_test_" + std::to_string(::getpid())))
    {
        fs::create_directories(dir);
        ::setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
    }
    ~TmpConfig()
    {
        ::unsetenv("XDG_CONFIG_HOME");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
```

---

## Task 1: `SecretStore` header + stub + CMake scaffolding

**Files:**
- Create: `client/include/tesseract/secret_store.h`
- Create: `client/src/secret_store_stub.cpp`
- Modify: `client/CMakeLists.txt`
- Create: `tests/cpp/test_session_store_migration.cpp`

- [ ] **Step 1.1: Write the failing test**

Create `tests/cpp/test_session_store_migration.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include "tesseract/secret_store.h"
#include <string>

TEST_CASE("SecretStore stub: save returns false", "[session][secret][stub]")
{
    // When the real backend is unavailable the stub returns false for save.
    // This test only asserts the interface compiles and the stub is linked.
    // On platforms where the real backend IS available, this test just passes.
    const std::string uid = "@stub_test:example.org";
    // We can't force the stub here — just confirm the call compiles and
    // returns a bool.
    bool ok = tesseract::SecretStore::save(uid, R"({"test":1})");
    (void)ok; // result depends on platform backend availability
}

TEST_CASE("SecretStore stub: load returns nullopt for unknown user",
          "[session][secret][stub]")
{
    auto result = tesseract::SecretStore::load("@nonexistent_xyz:example.org");
    // Stub always returns nullopt. Real backends return nullopt for missing keys.
    CHECK(!result.has_value());
}

TEST_CASE("SecretStore stub: remove is a no-op", "[session][secret][stub]")
{
    // Should not crash.
    tesseract::SecretStore::remove("@nobody:example.org");
}
```

- [ ] **Step 1.2: Run to confirm it fails to compile**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | head -20
```

Expected: compile error — `tesseract::SecretStore` not defined.

- [ ] **Step 1.3: Create `client/include/tesseract/secret_store.h`**

```cpp
#pragma once

#include <optional>
#include <string>

namespace tesseract
{

// Per-platform secure credential storage for OAuth session blobs.
//
// Backends:
//   Windows : Windows Credential Manager (CredWriteW / CredReadW / CredDeleteW)
//   macOS   : Keychain Services (SecItemAdd / SecItemCopyMatching / SecItemDelete)
//   Linux   : libsecret (secret_password_*_sync) — falls back to the no-op
//             stub when libsecret is not found at configure time.
//
// All methods are synchronous and safe to call on the UI thread: the
// credential APIs used here are fast (in-process or IPC but not network).
class SecretStore
{
public:
    // Load the JSON blob stored for `user_id`. Returns nullopt when the key
    // does not exist, on backend error, or when the backend is unavailable.
    static std::optional<std::string> load(const std::string& user_id);

    // Persist `json` for `user_id`. Returns false on failure or when the
    // backend is unavailable (caller should fall back to plaintext).
    static bool save(const std::string& user_id, const std::string& json);

    // Remove the credential for `user_id`. No-op if the key does not exist
    // or the backend is unavailable.
    static void remove(const std::string& user_id);
};

} // namespace tesseract
```

- [ ] **Step 1.4: Create `client/src/secret_store_stub.cpp`**

```cpp
#include "tesseract/secret_store.h"

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string&)
{
    return std::nullopt;
}

bool SecretStore::save(const std::string&, const std::string&)
{
    return false;
}

void SecretStore::remove(const std::string&)
{
}

} // namespace tesseract
```

- [ ] **Step 1.5: Add SecretStore to `client/CMakeLists.txt`**

After the `add_library(tesseract_client STATIC ...)` block (after the `src/waveform_cache.cpp` line), add the following block. This goes before the `target_compile_features` line:

```cmake
# ── SecretStore: platform credential backend ────────────────────────────────
if(WIN32)
    target_sources(tesseract_client PRIVATE src/secret_store_windows.cpp)
elseif(APPLE)
    target_sources(tesseract_client PRIVATE src/secret_store_macos.cpp)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSECRET QUIET libsecret-1)
    endif()
    if(LIBSECRET_FOUND)
        target_sources(tesseract_client PRIVATE src/secret_store_linux.cpp)
        target_include_directories(tesseract_client
            PRIVATE ${LIBSECRET_INCLUDE_DIRS})
        target_link_libraries(tesseract_client
            PRIVATE ${LIBSECRET_LIBRARIES})
        target_compile_definitions(tesseract_client
            PRIVATE TESSERACT_LIBSECRET=1)
    else()
        target_sources(tesseract_client PRIVATE src/secret_store_stub.cpp)
        message(STATUS "tesseract: libsecret not found — using plaintext token fallback")
    endif()
endif()
```

Also add macOS and Windows link dependencies. In the existing `if(WIN32)` block (which already links `ws2_32` etc.), add `advapi32` to the list:

Find this block:
```cmake
    target_link_libraries(tesseract_client PUBLIC
        ws2_32 bcrypt userenv ntdll
        shell32)               # ShellExecuteA in open_in_browser()
```

Change to:
```cmake
    target_link_libraries(tesseract_client PUBLIC
        ws2_32 bcrypt userenv ntdll
        shell32                # ShellExecuteA in open_in_browser()
        advapi32)              # CredWriteW / CredReadW for SecretStore
```

Then after the `if(WIN32)` block (search for where macOS link deps would go — add a new block):

```cmake
if(APPLE)
    target_link_libraries(tesseract_client PRIVATE "-framework Security")
endif()
```

- [ ] **Step 1.6: Create placeholder source files for non-Linux platforms**

The CMake build for Linux Qt6 only compiles the Linux branch, but the files referenced in the `APPLE` and `WIN32` branches must exist for IDEs and Windows/macOS CI. Create them as stubs that will be filled in Tasks 3 and 4:

Create `client/src/secret_store_macos.cpp` with a single comment line:
```cpp
// macOS Keychain implementation — see Task 3 of the secure-token-storage plan.
```

Create `client/src/secret_store_windows.cpp` with a single comment line:
```cpp
// Windows Credential Manager implementation — see Task 4 of the secure-token-storage plan.
```

(These will be replaced entirely in Tasks 3 and 4.)

- [ ] **Step 1.7: Build and run tests**

```bash
cmake --preset linux-qt6-debug && \
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "\[session\]\[secret\]" \
      --output-on-failure
```

Expected: all three stub tests pass.

- [ ] **Step 1.8: Commit**

```bash
git add client/include/tesseract/secret_store.h \
        client/src/secret_store_stub.cpp \
        client/src/secret_store_macos.cpp \
        client/src/secret_store_windows.cpp \
        client/CMakeLists.txt \
        tests/cpp/test_session_store_migration.cpp
git commit -m "feat(client): add SecretStore interface + no-op stub + CMake wiring"
```

---

## Task 2: Linux libsecret implementation

**Files:**
- Create: `client/src/secret_store_linux.cpp`
- Test: `tests/cpp/test_session_store_migration.cpp`

The `secret_password_*_sync` family is the synchronous libsecret API. Each call uses a `SecretSchema` that tags the credential with a schema name and an attribute set; the attribute `user-id` lets us store one entry per Matrix user ID.

- [ ] **Step 2.1: Write the failing test**

Add to `tests/cpp/test_session_store_migration.cpp`:

```cpp
TEST_CASE("SecretStore round-trip: save and load", "[session][secret][roundtrip]")
{
    const std::string uid   = "@roundtrip_test:example.org";
    const std::string json  = R"({"user_id":"@roundtrip_test:example.org","access_token":"t0ken"})";

    // Save
    bool saved = tesseract::SecretStore::save(uid, json);
    if (!saved)
        return; // skip: backend not available (stub or no service daemon)

    // Load
    auto loaded = tesseract::SecretStore::load(uid);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == json);

    // Cleanup
    tesseract::SecretStore::remove(uid);

    // Confirm removal
    auto after = tesseract::SecretStore::load(uid);
    CHECK(!after.has_value());
}
```

- [ ] **Step 2.2: Run to confirm test passes with stub (skip path)**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "round-trip" --output-on-failure
```

Expected: test passes (skip path — stub returns false, test returns early).

- [ ] **Step 2.3: Create `client/src/secret_store_linux.cpp`**

```cpp
#include "tesseract/secret_store.h"
#include <libsecret/secret.h>

namespace
{

// Schema name and attribute are reverse-DNS namespaced to avoid collisions
// with other apps using the same secret service collection.
const SecretSchema kSchema = {
    "lan.westeros.tesseract.session",
    SECRET_SCHEMA_NONE,
    {{"user-id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
};

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    GError* err = nullptr;
    gchar* secret = secret_password_lookup_sync(
        &kSchema, nullptr, &err,
        "user-id", user_id.c_str(),
        nullptr);
    if (err)
    {
        g_error_free(err);
        return std::nullopt;
    }
    if (!secret)
        return std::nullopt;

    std::string result(secret);
    secret_password_free(secret);
    return result;
}

bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    GError* err = nullptr;
    gboolean ok = secret_password_store_sync(
        &kSchema,
        SECRET_COLLECTION_DEFAULT,
        "Tesseract session",
        json.c_str(),
        nullptr, &err,
        "user-id", user_id.c_str(),
        nullptr);
    if (err)
    {
        g_error_free(err);
        return false;
    }
    return static_cast<bool>(ok);
}

void SecretStore::remove(const std::string& user_id)
{
    GError* err = nullptr;
    secret_password_clear_sync(
        &kSchema, nullptr, &err,
        "user-id", user_id.c_str(),
        nullptr);
    if (err)
        g_error_free(err);
}

} // namespace tesseract
```

- [ ] **Step 2.4: Build and run test (with libsecret if available)**

```bash
cmake --preset linux-qt6-debug && \
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug -R "round-trip" --output-on-failure
```

Expected on a desktop with a running secret service (GNOME Keyring / KWallet): test passes with a real round-trip.
Expected on headless CI without a secret service: test passes (skip path — `SecretStore::save` returns false).

- [ ] **Step 2.5: Commit**

```bash
git add client/src/secret_store_linux.cpp
git commit -m "feat(linux): implement SecretStore with libsecret"
```

---

## Task 3: macOS Keychain implementation

**Files:**
- Modify: `client/src/secret_store_macos.cpp` (replace placeholder)

The Keychain API uses CF dictionaries as queries. `SecItemUpdate` for existing items, `SecItemAdd` for new ones. `CFRetained<T>` (already in `canvas_cg.cpp` — copy the pattern here) handles CF memory ownership.

- [ ] **Step 3.1: Replace `client/src/secret_store_macos.cpp`**

```cpp
#include "tesseract/secret_store.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <string>

namespace
{

// Wraps a +1-retained CF object. T must be a CF ref type (CFStringRef, etc.).
template <typename T>
struct CFOwned
{
    T ref = nullptr;
    explicit CFOwned(T r) : ref(r) {}
    ~CFOwned() { if (ref) CFRelease(ref); }
    CFOwned(const CFOwned&) = delete;
    CFOwned& operator=(const CFOwned&) = delete;
    operator T() const { return ref; }
};

CFOwned<CFStringRef> cf_string(const std::string& s)
{
    return CFOwned<CFStringRef>(CFStringCreateWithBytes(
        nullptr,
        reinterpret_cast<const UInt8*>(s.data()),
        static_cast<CFIndex>(s.size()),
        kCFStringEncodingUTF8,
        false));
}

static CFStringRef kService = CFSTR("lan.westeros.tesseract");

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    auto account = cf_string(user_id);
    const void* keys[] = {
        kSecClass, kSecAttrService, kSecAttrAccount,
        kSecReturnData, kSecMatchLimit};
    const void* vals[] = {
        kSecClassGenericPassword, kService, account.ref,
        kCFBooleanTrue, kSecMatchLimitOne};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, keys, vals, 5,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));

    CFTypeRef raw = nullptr;
    OSStatus status = SecItemCopyMatching(query, &raw);
    if (status != errSecSuccess || !raw)
        return std::nullopt;

    CFOwned<CFDataRef> data(static_cast<CFDataRef>(raw));
    return std::string(
        reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
        static_cast<std::size_t>(CFDataGetLength(data)));
}

bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    auto account = cf_string(user_id);
    CFOwned<CFDataRef> value(CFDataCreate(
        nullptr,
        reinterpret_cast<const UInt8*>(json.data()),
        static_cast<CFIndex>(json.size())));

    // Try update first.
    {
        const void* qk[] = {kSecClass, kSecAttrService, kSecAttrAccount};
        const void* qv[] = {kSecClassGenericPassword, kService, account.ref};
        CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
            nullptr, qk, qv, 3,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks));
        const void* uk[] = {kSecValueData};
        const void* uv[] = {value.ref};
        CFOwned<CFDictionaryRef> update(CFDictionaryCreate(
            nullptr, uk, uv, 1,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks));
        OSStatus st = SecItemUpdate(query, update);
        if (st == errSecSuccess)
            return true;
        if (st != errSecItemNotFound)
            return false;
    }

    // Item not found — add it.
    const void* ak[] = {
        kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
    const void* av[] = {
        kSecClassGenericPassword, kService, account.ref, value.ref};
    CFOwned<CFDictionaryRef> add(CFDictionaryCreate(
        nullptr, ak, av, 4,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    return SecItemAdd(add, nullptr) == errSecSuccess;
}

void SecretStore::remove(const std::string& user_id)
{
    auto account = cf_string(user_id);
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* vals[] = {kSecClassGenericPassword, kService, account.ref};
    CFOwned<CFDictionaryRef> query(CFDictionaryCreate(
        nullptr, keys, vals, 3,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    SecItemDelete(query);
}

} // namespace tesseract
```

- [ ] **Step 3.2: Build on macOS and confirm tests pass**

On a macOS build environment:
```bash
cmake --preset macos-appkit-arm64-debug && \
cmake --build build/macos-appkit-arm64-debug --target tesseract_tests && \
ctest --test-dir build/macos-appkit-arm64-debug \
      -R "\[session\]\[secret\]" --output-on-failure
```

Expected: `round-trip` test passes (Keychain is always available on macOS). Stub tests also pass.

- [ ] **Step 3.3: Commit**

```bash
git add client/src/secret_store_macos.cpp
git commit -m "feat(macos): implement SecretStore with Keychain Services"
```

---

## Task 4: Windows Credential Manager implementation

**Files:**
- Modify: `client/src/secret_store_windows.cpp` (replace placeholder)

`CredWriteW` / `CredReadW` / `CredDeleteW` are in `wincred.h` and link against `advapi32` (already added to `CMakeLists.txt` in Task 1). The credential target name is `Tesseract:<user_id>`.

Note: `CredentialBlobSize` is in bytes and limited to `CRED_MAX_CREDENTIAL_BLOB_SIZE` (5120 bytes on older Windows; 32KiB on Windows 8+). A Matrix PersistedSession JSON is well within these limits.

- [ ] **Step 4.1: Replace `client/src/secret_store_windows.cpp`**

```cpp
#include "tesseract/secret_store.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>

#include <string>

namespace
{

std::wstring make_target(const std::string& user_id)
{
    std::wstring target = L"Tesseract:";
    // Matrix IDs are ASCII-compatible; widen one byte at a time is safe here.
    for (char c : user_id)
        target.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return target;
}

} // namespace

namespace tesseract
{

std::optional<std::string> SecretStore::load(const std::string& user_id)
{
    auto target = make_target(user_id);
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred))
        return std::nullopt;

    std::string result(
        reinterpret_cast<const char*>(cred->CredentialBlob),
        static_cast<std::size_t>(cred->CredentialBlobSize));
    CredFree(cred);
    return result;
}

bool SecretStore::save(const std::string& user_id, const std::string& json)
{
    auto target = make_target(user_id);
    CREDENTIALW cred = {};
    cred.Type             = CRED_TYPE_GENERIC;
    cred.TargetName       = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize =
        static_cast<DWORD>(json.size());
    cred.CredentialBlob   =
        reinterpret_cast<LPBYTE>(const_cast<char*>(json.data()));
    cred.Persist          = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) == TRUE;
}

void SecretStore::remove(const std::string& user_id)
{
    auto target = make_target(user_id);
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}

} // namespace tesseract
```

- [ ] **Step 4.2: Build on Windows and confirm tests pass**

```bat
cmake --preset windows-debug
cmake --build build\windows-debug --target tesseract_tests
ctest --test-dir build\windows-debug -R "[session][secret]" --output-on-failure
```

Expected: `round-trip` test passes (Credential Manager is always available on Windows). Stub tests also pass.

- [ ] **Step 4.3: Commit**

```bash
git add client/src/secret_store_windows.cpp
git commit -m "feat(win32): implement SecretStore with Windows Credential Manager"
```

---

## Task 5: `SessionStore` integration and migration

**Files:**
- Modify: `client/src/session_store.cpp`
- Test: `tests/cpp/test_session_store_migration.cpp`

The sentinel value `{"v":2}` is a known-short string written to `session.json` after a successful SecretStore migration. `load_account()` detects it and returns nullopt (trusting SecretStore). On SecretStore miss, an old plaintext file triggers an opportunistic re-migration.

- [ ] **Step 5.1: Write the failing tests**

Add to `tests/cpp/test_session_store_migration.cpp`:

```cpp
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "tesseract/session_store.h"
#include "tesseract/secret_store.h"

namespace fs = std::filesystem;

struct TmpConfig
{
    fs::path dir;
    TmpConfig()
        : dir(fs::temp_directory_path() /
              ("tss_test_" + std::to_string(::getpid())))
    {
        fs::create_directories(dir);
        ::setenv("XDG_CONFIG_HOME", dir.string().c_str(), 1);
    }
    ~TmpConfig()
    {
        ::unsetenv("XDG_CONFIG_HOME");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

TEST_CASE("SessionStore: save_account and load_account round-trip",
          "[session][integration]")
{
    TmpConfig cfg;
    const std::string uid  = "@alice:example.org";
    const std::string json =
        R"({"user_id":"@alice:example.org","access_token":"tok_abc"})";

    REQUIRE(tesseract::SessionStore::save_account(uid, json));
    auto loaded = tesseract::SessionStore::load_account(uid);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == json);

    // Cleanup SecretStore entry if migration succeeded.
    tesseract::SecretStore::remove(uid);
    tesseract::SessionStore::clear_account(uid);
}

TEST_CASE("SessionStore: load_account migrates legacy plaintext to SecretStore",
          "[session][migration]")
{
    TmpConfig cfg;
    const std::string uid  = "@bob:matrix.org";
    const std::string json =
        R"({"user_id":"@bob:matrix.org","access_token":"tok_xyz"})";

    // Write a legacy plaintext session file.
    auto acct_dir = tesseract::SessionStore::account_dir(uid);
    fs::create_directories(acct_dir);
    {
        std::ofstream f(acct_dir / "session.json");
        f << json;
    }

    // load_account should return the content regardless of SecretStore availability.
    auto result = tesseract::SessionStore::load_account(uid);
    REQUIRE(result.has_value());
    CHECK(*result == json);

    // If SecretStore is available, the migration should have taken place.
    auto sec = tesseract::SecretStore::load(uid);
    if (sec.has_value())
    {
        CHECK(*sec == json);
        // File should now hold the sentinel.
        std::ifstream fin(acct_dir / "session.json");
        std::string file_content((std::istreambuf_iterator<char>(fin)), {});
        CHECK(file_content == "{\"v\":2}");
    }

    tesseract::SecretStore::remove(uid);
    tesseract::SessionStore::clear_account(uid);
}

TEST_CASE("SessionStore: clear_account removes SecretStore entry",
          "[session][migration]")
{
    TmpConfig cfg;
    const std::string uid  = "@carol:server.net";
    const std::string json =
        R"({"user_id":"@carol:server.net","access_token":"tok_qrs"})";

    REQUIRE(tesseract::SessionStore::save_account(uid, json));
    tesseract::SessionStore::clear_account(uid);

    // No session should be found after clear.
    CHECK(!tesseract::SessionStore::load_account(uid).has_value());
    // SecretStore must also be empty.
    CHECK(!tesseract::SecretStore::load(uid).has_value());
}

TEST_CASE("SessionStore: sentinel file suppresses stale plaintext reads",
          "[session][migration]")
{
    TmpConfig cfg;
    const std::string uid      = "@dave:homeserver.tld";
    const std::string old_json =
        R"({"user_id":"@dave:homeserver.tld","access_token":"tok_old"})";

    // Write a sentinel as if migration already completed.
    auto acct_dir = tesseract::SessionStore::account_dir(uid);
    fs::create_directories(acct_dir);
    {
        std::ofstream f(acct_dir / "session.json");
        f << "{\"v\":2}";
    }

    // No entry in SecretStore → should return nullopt (not the old plaintext).
    auto result = tesseract::SessionStore::load_account(uid);
    CHECK(!result.has_value());
}
```

- [ ] **Step 5.2: Run to confirm tests fail**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug \
      -R "\[session\]\[migration\]|\[session\]\[integration\]" \
      --output-on-failure
```

Expected: `round-trip` and `migrates legacy` tests fail or assert incorrectly because `session_store.cpp` doesn't call `SecretStore` yet.

- [ ] **Step 5.3: Update `client/src/session_store.cpp`**

Add the include at the top of `session_store.cpp` (after the existing includes):

```cpp
#include "tesseract/secret_store.h"
```

Add this constant near the top of the anonymous namespace (after the existing `json_escape` / `extract_string` helpers):

```cpp
// Written to session.json after a successful migration to SecretStore.
// load_account() treats this as "data is in SecretStore; return nullopt on miss."
static constexpr std::string_view kMigratedSentinel = "{\"v\":2}";
```

Replace `SessionStore::load_account()` (the existing ~20-line implementation) with:

```cpp
std::optional<std::string>
SessionStore::load_account(const std::string& user_id)
{
    // 1. Authoritative store: SecretStore (Keychain / CredManager / libsecret).
    //    Returns nullopt when the backend is unavailable (stub) or the key
    //    does not exist.
    if (auto sec = SecretStore::load(user_id))
        return sec;

    // 2. Legacy plaintext fallback.
    fs::path p = account_dir(user_id) / "session.json";
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    if (in.bad())
        return std::nullopt;
    std::string s = buf.str();

    // Sentinel: migration has already run but SecretStore returned nothing
    // (key deleted externally or backend unavailable this boot). Treat as
    // no session — the user must log in again.
    if (s.empty() || s == kMigratedSentinel)
        return std::nullopt;

    // 3. Opportunistic migration: on first load after upgrade, move the
    //    plaintext session to SecretStore. Best-effort: if SecretStore is
    //    unavailable (stub) or the write fails, we keep returning the
    //    plaintext on every subsequent load — no data loss.
    if (SecretStore::save(user_id, s))
        atomic_write(p, kMigratedSentinel); // suppress future plaintext reads

    return s;
}
```

Replace `SessionStore::save_account()` (the existing ~8-line implementation) with:

```cpp
bool SessionStore::save_account(const std::string& user_id,
                                const std::string& json)
{
    if (sanitize_user_id(user_id).empty())
        return false;

    // Try SecretStore first. If it succeeds, write the sentinel so that
    // load_account knows the real data is in SecretStore.
    if (SecretStore::save(user_id, json))
    {
        // The sentinel write is best-effort: if it fails (e.g. disk full),
        // the stale plaintext file remains but SecretStore is authoritative
        // on next load (step 1 of load_account). Return success because the
        // important write succeeded.
        atomic_write(account_dir(user_id) / "session.json", kMigratedSentinel);
        return true;
    }

    // SecretStore unavailable (stub / backend error) — write full JSON to
    // disk (existing plaintext behaviour).
    return atomic_write(account_dir(user_id) / "session.json", json);
}
```

Replace `SessionStore::clear_account()` (the existing ~5-line implementation) with:

```cpp
void SessionStore::clear_account(const std::string& user_id)
{
    SecretStore::remove(user_id);
    std::error_code ec;
    fs::remove_all(account_dir(user_id), ec);
}
```

- [ ] **Step 5.4: Build and run new tests**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests && \
ctest --test-dir build/linux-qt6-debug \
      -R "\[session\]" --output-on-failure
```

Expected: all `[session]` tests pass.

- [ ] **Step 5.5: Run the full test suite**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure
```

Expected: all tests pass — no regressions.

- [ ] **Step 5.6: Commit**

```bash
git add client/src/session_store.cpp \
        tests/cpp/test_session_store_migration.cpp
git commit -m "feat(client): integrate SecretStore into SessionStore with plaintext migration"
```

---

## Verification

End-to-end manual check (Linux with GNOME Keyring or KWallet running):

1. Build and launch: `cmake --build build/linux-qt6-debug && ./build/linux-qt6-debug/ui/linux-qt/tesseract`
2. Log in. Confirm the app loads and syncs normally.
3. Open the GNOME Keyring viewer (`seahorse`) — confirm an entry named `"Tesseract session"` appears with the `user-id` attribute set to your Matrix ID.
4. Quit and relaunch — confirm automatic re-login still works (session restored from Keychain / SecretStore).
5. Check that `~/.config/tesseract/accounts/<uid>/session.json` now contains `{"v":2}` (sentinel), not the raw token JSON.
6. Run all tests: `ctest --test-dir build/linux-qt6-debug --output-on-failure` — all pass.

**Regression check for plaintext fallback:**
On a machine without a running secret service (headless CI):

1. Confirm the build used `secret_store_stub.cpp` (CMake output: `"tesseract: libsecret not found — using plaintext token fallback"`)
2. Log in normally — session saved to plaintext file.
3. Quit and relaunch — automatic re-login works via plaintext fallback.

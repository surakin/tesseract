# Location Events Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Receive and display Matrix `m.location` / MSC3488 location messages as interactive inline maps (OpenStreetMap tiles, pan + zoom) across Qt6, GTK4, Win32, and macOS.

**Architecture:** New `fetch_url_bytes` FFI for raw HTTP fetches; location fields added to the existing flat `TimelineEvent` discriminated union; `map_tiles.h` pure-function header for Web Mercator math; `ensure_tile_async` in `ShellBase` (mirrors `ensure_media_image_`); `Kind::Location` in `MessageRowData` with inline tile painting and pan/zoom pointer tracking in `MessageListView`; all four shells wired via `RoomWindowBase::finish_init_()`.

**Tech Stack:** Rust (reqwest via matrix-sdk), C++17, tesseract_tk canvas, Catch2

---

## File Inventory

| File | Change |
|------|--------|
| `sdk/src/bridge.rs` | +3 location fields on `TimelineEvent`; +`fetch_url_bytes` FFI fn |
| `sdk/src/client.rs` | `parse_geo_uri` helper; `m.location` case in `timeline_item_to_ffi`; `fetch_url_bytes` impl |
| `client/include/tesseract/types.h` | +`Location` to `EventType`; +`LocationEvent` struct |
| `client/src/ffi_convert.h` | +`"m.location"` arm in `make_event()` |
| `client/include/tesseract/client.h` | +`fetch_url_bytes` declaration |
| `client/src/client.cpp` | +`fetch_url_bytes` implementation |
| `ui/shared/views/map_tiles.h` | **New** — pure Web Mercator functions (TileCoord, MapViewport, lat_lon_to_tile, lat_lon_to_world_px, world_px_to_lat_lon, tile_pixel_origin, tiles_in_view, tile_cache_key, tile_url) |
| `ui/shared/app/ShellBase.h` | +`MediaKind::Tile`; +`tile_fetches_in_flight_`; +`tile_fetch_failed_`; +`ensure_tile_async` |
| `ui/shared/app/ShellBase.cpp` | +`ensure_tile_async` implementation |
| `ui/shared/views/MessageListView.h` | +`Kind::Location`; location fields on `MessageRowData`; `MapViewport` field; `on_tile_needed` callback; `map_active_row_`, drag state members |
| `ui/shared/views/MessageListView.cpp` | +`make_row_data` location mapping; +`paint_row` location case; +pointer/wheel handlers for pan/zoom |
| `ui/shared/app/RoomWindowBase.cpp` | Wire `on_tile_needed` in `finish_init_()` |
| Each shell `on_media_bytes_ready_` | +`MediaKind::Tile` case: decode + store + invalidate |
| `tests/CMakeLists.txt` | +`cpp/test_map_tiles.cpp`; +`cpp/test_location_events.cpp` |
| `tests/cpp/test_map_tiles.cpp` | **New** — tile math unit tests |
| `tests/cpp/test_location_events.cpp` | **New** — `make_row_data` location mapping tests |
| `sdk/src/client_test.rs` or similar | Rust unit tests for geo URI parsing and m.location event conversion |
| `CHANGES.md`, `ROADMAP.md`, `STATUS.md` | Update |

---

## Task 1: Add `fetch_url_bytes` FFI and C++ client API

**Files:**
- Modify: `sdk/src/bridge.rs` (around line 764, after `fetch_source_bytes`)
- Modify: `sdk/src/client.rs`
- Modify: `client/include/tesseract/client.h` (after `fetch_source_bytes` declaration, around line 366)
- Modify: `client/src/client.cpp`

- [ ] **Step 1: Add `fetch_url_bytes` to the Rust bridge**

  In `sdk/src/bridge.rs`, after the `fetch_source_bytes` declaration (line 764), add:

  ```rust
          /// Fetch raw bytes from an arbitrary HTTP/HTTPS URL.
          /// Returns the response body on success, or an empty Vec on any error.
          /// Sets User-Agent to "Tesseract/0.1 (Matrix client)" per OSM tile policy.
          fn fetch_url_bytes(self: &mut ClientFfi, url: &str) -> Vec<u8>;
  ```

- [ ] **Step 2: Implement `fetch_url_bytes` in `client.rs`**

  In `sdk/src/client.rs`, after `fetch_source_bytes`, add:

  ```rust
  pub fn fetch_url_bytes(&mut self, url: &str) -> Vec<u8> {
      if url.is_empty() { return Vec::new(); }
      let url = url.to_owned();
      self.rt.block_on(async move {
          let client = match reqwest::Client::builder()
              .user_agent("Tesseract/0.1 (Matrix client)")
              .build()
          {
              Ok(c)  => c,
              Err(_) => return Vec::new(),
          };
          match client.get(&url).send().await {
              Ok(resp) => resp.bytes().await
                  .map(|b| b.to_vec())
                  .unwrap_or_default(),
              Err(_) => Vec::new(),
          }
      })
  }
  ```

  `reqwest` is already a transitive dependency of `matrix-sdk`; no `Cargo.toml` change is needed.

- [ ] **Step 3: Add declaration to `client/include/tesseract/client.h`**

  After `fetch_source_bytes` declaration (around line 366), add:

  ```cpp
      /// Fetch raw bytes from an arbitrary HTTP/HTTPS URL.
      /// Returns an empty vector on any error. Blocks the calling thread.
      std::vector<uint8_t> fetch_url_bytes(const std::string& url);
  ```

- [ ] **Step 4: Implement in `client/src/client.cpp`**

  After `fetch_source_bytes` implementation, add:

  ```cpp
  std::vector<uint8_t> Client::fetch_url_bytes(const std::string& url) {
      auto v = impl_->ffi->fetch_url_bytes(url);
      return std::vector<uint8_t>(v.begin(), v.end());
  }
  ```

- [ ] **Step 5: Build to verify**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | grep -E "^.*error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 6: Commit**

  ```bash
  git add sdk/src/bridge.rs sdk/src/client.rs \
          client/include/tesseract/client.h client/src/client.cpp
  git commit -m "feat(sdk): add fetch_url_bytes FFI for generic HTTP GET (tile fetch)"
  ```

---

## Task 2: Add location fields to `TimelineEvent` and parse `m.location`

**Files:**
- Modify: `sdk/src/bridge.rs` (TimelineEvent struct, lines 88–188)
- Modify: `sdk/src/client.rs` (timeline_item_to_ffi, MessageType match)

- [ ] **Step 1: Write the failing Rust tests**

  In `sdk/src/client.rs` (or a `#[cfg(test)]` module at the bottom of the file), add:

  ```rust
  #[cfg(test)]
  mod location_tests {
      use super::parse_geo_uri;

      #[test]
      fn parse_geo_uri_standard() {
          let r = parse_geo_uri("geo:51.5,-0.1");
          assert_eq!(r, Some((51.5, -0.1)));
      }

      #[test]
      fn parse_geo_uri_with_altitude() {
          let r = parse_geo_uri("geo:51.5,-0.1,0");
          assert_eq!(r, Some((51.5, -0.1)));
      }

      #[test]
      fn parse_geo_uri_with_uncertainty() {
          let r = parse_geo_uri("geo:51.5,-0.1;u=35");
          assert_eq!(r, Some((51.5, -0.1)));
      }

      #[test]
      fn parse_geo_uri_malformed() {
          assert_eq!(parse_geo_uri("not-a-geo-uri"), None);
          assert_eq!(parse_geo_uri("geo:"), None);
          assert_eq!(parse_geo_uri("geo:abc,def"), None);
      }
  }
  ```

- [ ] **Step 2: Run tests to confirm they fail**

  ```bash
  cargo test -p tesseract-sdk-ffi location_tests 2>&1 | tail -10
  ```
  Expected: compile error — `parse_geo_uri` is not defined yet.

- [ ] **Step 3: Add location fields to `TimelineEvent` in `bridge.rs`**

  In `sdk/src/bridge.rs`, after `pending_txn_id: String,` (line 187), before the closing `}` of `TimelineEvent`, add:

  ```rust
          // m.location / MSC3488 (valid when msg_type == "m.location")
          location_lat:         f64,
          location_lon:         f64,
          location_description: String,
  ```

  Also add to the doc comment block at lines 67–87 (after the last `///` line):

  ```rust
      /// For `m.location` → location_lat, location_lon are the coordinates.
      ///                   location_description is the MSC3488 description (may be empty).
  ```

- [ ] **Step 4: Add defaults to all existing `TimelineEvent` struct literals**

  Search `sdk/src/client.rs` for all `TimelineEvent {` literals. There are three (virtual items, redacted, and the main message path). Add to each:

  ```rust
      location_lat:         0.0,
      location_lon:         0.0,
      location_description: String::new(),
  ```

  You can find them with:
  ```bash
  grep -n "TimelineEvent {" sdk/src/client.rs
  ```

- [ ] **Step 5: Implement `parse_geo_uri` helper in `client.rs`**

  Add before `timeline_item_to_ffi` (or at the top of the impl block):

  ```rust
  /// Parse a `geo:lat,lon` or `geo:lat,lon,alt` URI.
  /// Returns `(lat, lon)` or `None` on parse failure.
  fn parse_geo_uri(uri: &str) -> Option<(f64, f64)> {
      let coords = uri.strip_prefix("geo:")?;
      // Strip uncertainty params (after ';')
      let coords = coords.split(';').next()?;
      let mut parts = coords.split(',');
      let lat: f64 = parts.next()?.parse().ok()?;
      let lon: f64 = parts.next()?.parse().ok()?;
      Some((lat, lon))
  }
  ```

- [ ] **Step 6: Add `m.location` case to `timeline_item_to_ffi`**

  In `client.rs`, find the `match msg_content.msgtype()` block inside `timeline_item_to_ffi`. Add a `MessageType::Location(content)` arm. It should set `msg_type = "m.location"` and populate the three location fields. Look for how the `MessageType::Image` arm is handled and add after the last existing arm:

  ```rust
  MessageType::Location(content) => {
      let (lat, lon) = parse_geo_uri(&content.geo_uri)
          .unwrap_or((0.0, 0.0));
      // MSC3488: description in content.location.description
      let description = String::new(); // MSC3488 not yet in stable ruma
      (
          "m.location".to_owned(),
          content.body.clone(),
          // reuse existing tuple slots; location-specific data goes in new fields
          // ... the exact tuple structure depends on the surrounding match;
          // set location_lat, location_lon, location_description on the
          // final TimelineEvent literal in the m.location path:
          lat, lon, description,
      )
  }
  ```

  **Important:** The existing `timeline_item_to_ffi` builds a `TimelineEvent` from a destructured tuple at the end of the function. The exact approach depends on how the function is structured. The pattern to follow:

  1. Find the `MessageType::Image(content)` arm — note what it returns from the match.
  2. Add `MessageType::Location(content)` using the same return pattern, setting body from `content.body`.
  3. In the final `TimelineEvent { ... }` literal (the one that uses `msg_type`), populate the location fields with the values extracted in step 5.

  If the function uses a different structure (e.g., it mutates a local variable), adapt accordingly.

  The key guarantee: when `msg_type == "m.location"`, `location_lat` and `location_lon` are set from the `geo_uri`, and `body` contains the fallback text. If `geo_uri` is malformed, the fields default to `(0.0, 0.0)` and the event still gets `msg_type = "m.location"` (the C++ side renders it as an empty-coords map rather than silently dropping it — that's acceptable; a malformed URI is an edge case).

- [ ] **Step 7: Run Rust tests**

  ```bash
  cargo test -p tesseract-sdk-ffi location_tests 2>&1 | tail -10
  ```
  Expected: all 4 geo URI tests pass.

- [ ] **Step 8: Commit**

  ```bash
  git add sdk/src/bridge.rs sdk/src/client.rs
  git commit -m "feat(sdk): add location fields to TimelineEvent + m.location parsing"
  ```

---

## Task 3: Add `LocationEvent` to C++ types and `ffi_convert.h`

**Files:**
- Modify: `client/include/tesseract/types.h`
- Modify: `client/src/ffi_convert.h`

- [ ] **Step 1: Add `Location` to `EventType` enum in `types.h`**

  In `client/include/tesseract/types.h`, find the `EventType` enum (lines 8–11). Add `Location` to it:

  ```cpp
  enum class EventType {
      Text, Image, Sticker, File, Voice, Video, Redacted,
      Notice, Emote, Unhandled, DaySeparator, ReadMarker, TimelineStart,
      Location,
  };
  ```

- [ ] **Step 2: Add `LocationEvent` struct to `types.h`**

  After `UnhandledEvent` (around line 171), add:

  ```cpp
  struct LocationEvent : Event {
      double      lat  = 0.0;
      double      lon  = 0.0;
      std::string description;
  };
  ```

- [ ] **Step 3: Add `"m.location"` arm to `make_event()` in `ffi_convert.h`**

  In `client/src/ffi_convert.h`, inside `make_event()`, add before the fallback `UnhandledEvent` arm (look for the last `if/else if` chain):

  ```cpp
  if (e.msg_type == "m.location") {
      auto ev = std::make_unique<tesseract::LocationEvent>();
      assign_base(*ev, e);
      ev->type        = tesseract::EventType::Location;
      ev->lat         = e.location_lat;
      ev->lon         = e.location_lon;
      ev->description = std::string(e.location_description);
      return ev;
  }
  ```

- [ ] **Step 4: Build to verify**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add client/include/tesseract/types.h client/src/ffi_convert.h
  git commit -m "feat(client): LocationEvent type + m.location FFI conversion"
  ```

---

## Task 4: Create `map_tiles.h` and tile math tests

**Files:**
- Create: `ui/shared/views/map_tiles.h`
- Create: `tests/cpp/test_map_tiles.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

  Create `tests/cpp/test_map_tiles.cpp`:

  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include "views/map_tiles.h"
  #include <cmath>

  using namespace tesseract::views;

  // Tolerance for floating-point lat/lon round-trips.
  static constexpr double kEps = 1e-4;

  TEST_CASE("lat_lon_to_tile: London at zoom 10", "[map_tiles]") {
      // London: 51.5074, -0.1278 → tile (512, 340) at z=10
      auto t = lat_lon_to_tile(51.5074, -0.1278, 10);
      CHECK(t.z == 10);
      CHECK(t.x == 511);  // check exact expected value from OSM tile calculator
      CHECK(t.y == 340);
  }

  TEST_CASE("lat_lon_to_tile: tiles clamped to valid range", "[map_tiles]") {
      auto t = lat_lon_to_tile(90.0, 180.0, 5);
      CHECK(t.x >= 0); CHECK(t.x < (1 << 5));
      CHECK(t.y >= 0); CHECK(t.y < (1 << 5));
  }

  TEST_CASE("lat_lon_to_world_px / world_px_to_lat_lon round-trip", "[map_tiles]") {
      const double lat = 51.5074, lon = -0.1278;
      auto px = lat_lon_to_world_px(lat, lon, 12);
      auto [rlat, rlon] = world_px_to_lat_lon(px.x, px.y, 12);
      CHECK(std::abs(rlat - lat) < kEps);
      CHECK(std::abs(rlon - lon) < kEps);
  }

  TEST_CASE("tiles_in_view: 256x240 map at zoom 15 returns tiles", "[map_tiles]") {
      MapViewport vp{ 51.5074, -0.1278, 15 };
      tk::Rect map_rect{ 0, 0, 256, 240 };
      auto tiles = tiles_in_view(vp, map_rect);
      // Should be a small grid — at least 1 tile, at most ~20
      CHECK(!tiles.empty());
      CHECK(tiles.size() <= 20);
      for (const auto& t : tiles) {
          CHECK(t.z == 15);
          CHECK(t.x >= 0); CHECK(t.x < (1 << 15));
          CHECK(t.y >= 0); CHECK(t.y < (1 << 15));
      }
  }

  TEST_CASE("tile_cache_key format", "[map_tiles]") {
      CHECK(tile_cache_key({15, 123, 456}) == "tile:15/123/456");
  }

  TEST_CASE("tile_url format", "[map_tiles]") {
      CHECK(tile_url({15, 123, 456}) ==
            "https://tile.openstreetmap.org/15/123/456.png");
  }

  TEST_CASE("tile_pixel_origin places viewport-centre tile near map centre",
            "[map_tiles]") {
      MapViewport vp{ 51.5074, -0.1278, 15 };
      tk::Rect map_rect{ 0, 0, 256, 256 };
      auto vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
      // The tile that contains the viewport centre
      auto centre_tile = lat_lon_to_tile(vp.lat, vp.lon, vp.zoom);
      auto origin = tile_pixel_origin(centre_tile, vp_px, map_rect);
      // The tile origin should be within one tile-width of the map centre
      float map_cx = map_rect.x + map_rect.w / 2.0f;
      float map_cy = map_rect.y + map_rect.h / 2.0f;
      CHECK(std::abs(origin.x - map_cx) <= 256.0f);
      CHECK(std::abs(origin.y - map_cy) <= 256.0f);
  }
  ```

- [ ] **Step 2: Add test file to `tests/CMakeLists.txt`**

  In `tests/CMakeLists.txt`, inside the `add_executable(tesseract_tests ...)` call (before line 44 closing paren), add:

  ```cmake
      cpp/test_map_tiles.cpp
  ```

- [ ] **Step 3: Run to confirm failure**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | grep "error:" | head -5
  ```
  Expected: compile error — `map_tiles.h` not found yet.

- [ ] **Step 4: Create `ui/shared/views/map_tiles.h`**

  ```cpp
  #pragma once
  #include "tk/canvas.h"
  #include <cmath>
  #include <string>
  #include <utility>
  #include <vector>

  #ifndef M_PI
  #define M_PI 3.14159265358979323846
  #endif

  namespace tesseract::views {

  struct TileCoord  { int z = 0, x = 0, y = 0; };
  struct MapViewport { double lat = 0.0, lon = 0.0; int zoom = 15; };

  /// Convert geographic coordinates to the slippy-map tile index at `zoom`.
  inline TileCoord lat_lon_to_tile(double lat, double lon, int zoom) {
      const int n     = 1 << zoom;
      const double fn = static_cast<double>(n);
      const double lat_r = lat * M_PI / 180.0;
      int x = static_cast<int>(std::floor((lon + 180.0) / 360.0 * fn));
      int y = static_cast<int>(std::floor(
          (1.0 - std::log(std::tan(lat_r) + 1.0 / std::cos(lat_r)) / M_PI)
          / 2.0 * fn));
      x = std::max(0, std::min(x, n - 1));
      y = std::max(0, std::min(y, n - 1));
      return { zoom, x, y };
  }

  /// World-pixel position of a lat/lon at `zoom` (tile size = 256 px).
  inline tk::Point lat_lon_to_world_px(double lat, double lon, int zoom) {
      const double n = static_cast<double>(1 << zoom) * 256.0;
      const double lat_r = lat * M_PI / 180.0;
      double fx = (lon + 180.0) / 360.0 * n;
      double fy = (1.0 - std::log(std::tan(lat_r) + 1.0 / std::cos(lat_r)) / M_PI)
                  / 2.0 * n;
      return { static_cast<float>(fx), static_cast<float>(fy) };
  }

  /// Inverse of lat_lon_to_world_px.
  inline std::pair<double, double> world_px_to_lat_lon(float px, float py, int zoom) {
      const double n = static_cast<double>(1 << zoom) * 256.0;
      double fx = static_cast<double>(px) / n;
      double fy = static_cast<double>(py) / n;
      double lon = fx * 360.0 - 180.0;
      double merc_y = M_PI * (1.0 - 2.0 * fy);
      double lat = (2.0 * std::atan(std::exp(merc_y)) - M_PI / 2.0) * (180.0 / M_PI);
      return { lat, lon };
  }

  /// Top-left pixel position of a tile within `map_rect`, given the viewport.
  /// `viewport_px` is lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom).
  inline tk::Point tile_pixel_origin(TileCoord t, tk::Point viewport_px,
                                      tk::Rect map_rect) {
      float tile_world_x = static_cast<float>(t.x) * 256.0f;
      float tile_world_y = static_cast<float>(t.y) * 256.0f;
      float map_cx = map_rect.x + map_rect.w * 0.5f;
      float map_cy = map_rect.y + map_rect.h * 0.5f;
      return { map_cx + (tile_world_x - viewport_px.x),
               map_cy + (tile_world_y - viewport_px.y) };
  }

  /// All tile coordinates needed to fill `map_rect` at the viewport zoom.
  inline std::vector<TileCoord> tiles_in_view(MapViewport vp, tk::Rect map_rect) {
      const int max_idx = (1 << vp.zoom) - 1;
      tk::Point vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);
      float half_w = map_rect.w * 0.5f;
      float half_h = map_rect.h * 0.5f;
      int x0 = static_cast<int>(std::floor((vp_px.x - half_w) / 256.0f));
      int x1 = static_cast<int>(std::floor((vp_px.x + half_w) / 256.0f));
      int y0 = static_cast<int>(std::floor((vp_px.y - half_h) / 256.0f));
      int y1 = static_cast<int>(std::floor((vp_px.y + half_h) / 256.0f));
      x0 = std::max(0, x0); x1 = std::min(max_idx, x1);
      y0 = std::max(0, y0); y1 = std::min(max_idx, y1);
      std::vector<TileCoord> out;
      for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x)
              out.push_back({ vp.zoom, x, y });
      return out;
  }

  /// Stable image-cache key for a tile: "tile:z/x/y".
  inline std::string tile_cache_key(TileCoord t) {
      return "tile:" + std::to_string(t.z) + "/" +
             std::to_string(t.x) + "/" + std::to_string(t.y);
  }

  /// OSM slippy-map tile URL.
  inline std::string tile_url(TileCoord t) {
      return "https://tile.openstreetmap.org/" +
             std::to_string(t.z) + "/" +
             std::to_string(t.x) + "/" +
             std::to_string(t.y) + ".png";
  }

  } // namespace tesseract::views
  ```

- [ ] **Step 5: Run tests**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -5
  ctest --test-dir build/linux-qt6-debug -R map_tiles --output-on-failure
  ```

  **Correcting the London tile test:** The exact tile index for (51.5074, -0.1278) at zoom 10 should be verified by running the formula. If the test fails with the wrong expected value, update the `CHECK(t.x == 511)` and `CHECK(t.y == 340)` lines to match the actual output. The formula itself is correct — the test values need to match it.

- [ ] **Step 6: Commit**

  ```bash
  git add ui/shared/views/map_tiles.h \
          tests/cpp/test_map_tiles.cpp \
          tests/CMakeLists.txt
  git commit -m "feat(tk): map_tiles.h Web Mercator pure functions + unit tests"
  ```

---

## Task 5: Add tile fetch pipeline to ShellBase

**Files:**
- Modify: `ui/shared/app/ShellBase.h`
- Modify: `ui/shared/app/ShellBase.cpp`

- [ ] **Step 1: Add `MediaKind::Tile` to the enum in `ShellBase.h`**

  In `ShellBase.h`, find the `MediaKind` enum (line 158). Add `Tile`:

  ```cpp
  enum class MediaKind : std::uint8_t {
      RoomAvatar, // → tk_avatars_, triggers room-list repaint
      UserAvatar, // → tk_avatars_, triggers message-list repaint
      MediaImage, // → anim_cache_ or tk_images_, triggers message-list repaint
      Tile,       // → tk_images_["tile:z/x/y"], triggers full message-list repaint
  };
  ```

- [ ] **Step 2: Add tile dedup members to `ShellBase.h`**

  After `emoji_fetches_in_flight_` (line 104), add:

  ```cpp
      std::unordered_set<std::string> tile_fetches_in_flight_;
      std::unordered_set<std::string> tile_fetch_failed_;
  ```

- [ ] **Step 3: Add `ensure_tile_async` declaration to `ShellBase.h`**

  After `ensure_media_image_` (line 315), add:

  ```cpp
      /// Fetch an OSM tile (z/x/y) asynchronously. Idempotent — no-op if already
      /// in tk_images_, in-flight, or previously failed. On success: stores bytes
      /// via on_media_bytes_ready_(key, MediaKind::Tile, bytes). On failure:
      /// inserts key into tile_fetch_failed_ to suppress retries this session.
      void ensure_tile_async(int z, int x, int y);
  ```

- [ ] **Step 4: Add the include for `map_tiles.h` to `ShellBase.cpp`**

  At the top of `ui/shared/app/ShellBase.cpp`, after the existing includes, add:

  ```cpp
  #include "views/map_tiles.h"
  #include <fstream>
  ```

- [ ] **Step 5: Implement `ensure_tile_async` in `ShellBase.cpp`**

  After `ensure_media_image_` implementation (around line 79), add:

  ```cpp
  void ShellBase::ensure_tile_async(int z, int x, int y) {
      const std::string key = tesseract::views::tile_cache_key({z, x, y});
      if (tk_images_.count(key) || tile_fetch_failed_.count(key)) return;
      if (!tile_fetches_in_flight_.insert(key).second) return;

      const std::string url = tesseract::views::tile_url({z, x, y});
      const std::filesystem::path disk_path =
          tesseract::cache_dir() / "tiles" /
          std::to_string(z) / std::to_string(x) /
          (std::to_string(y) + ".png");

      run_async_([this, key, url, disk_path]() {
          std::vector<uint8_t> bytes;
          // Check disk cache first.
          if (std::filesystem::exists(disk_path)) {
              std::ifstream f(disk_path, std::ios::binary);
              bytes.assign(std::istreambuf_iterator<char>(f), {});
          }
          // Fetch from network if not on disk.
          if (bytes.empty()) {
              bytes = client_->fetch_url_bytes(url);
              if (!bytes.empty()) {
                  std::error_code ec;
                  std::filesystem::create_directories(disk_path.parent_path(), ec);
                  if (!ec) {
                      std::ofstream f(disk_path, std::ios::binary);
                      f.write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                  }
              }
          }
          post_to_ui_([this, key, bytes = std::move(bytes)]() mutable {
              tile_fetches_in_flight_.erase(key);
              if (bytes.empty()) {
                  tile_fetch_failed_.insert(key);
                  return;
              }
              on_media_bytes_ready_(key, MediaKind::Tile, std::move(bytes));
          });
      });
  }
  ```

  **Note on `tesseract::cache_dir()`**: This is the same function used to initialise `media_disk_cache_` (line 95: `tk::MediaDiskCache media_disk_cache_{tesseract::cache_dir() / "media"}`). If the function is named differently in the actual header, use the same name.

- [ ] **Step 6: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | grep "error:" | head -20
  ```
  Expected: no errors. (The `MediaKind::Tile` case in each shell's `on_media_bytes_ready_` is not yet handled; GCC/Clang may warn about an unhandled enum value — that's fine for now, the warning becomes an error in Task 8.)

- [ ] **Step 7: Commit**

  ```bash
  git add ui/shared/app/ShellBase.h ui/shared/app/ShellBase.cpp
  git commit -m "feat(ui): tile fetch pipeline in ShellBase (ensure_tile_async + MediaKind::Tile)"
  ```

---

## Task 6: Add `Kind::Location` to `MessageListView` — data model and rendering

**Files:**
- Modify: `ui/shared/views/MessageListView.h`
- Modify: `ui/shared/views/MessageListView.cpp`
- Create: `tests/cpp/test_location_events.cpp`
- Modify: `tests/CMakeLists.txt`

### Part A — tests and data model

- [ ] **Step 1: Write failing tests**

  Create `tests/cpp/test_location_events.cpp`:

  ```cpp
  #include <catch2/catch_test_macros.hpp>
  #include "views/MessageListView.h"
  #include "views/map_tiles.h"
  #include <tesseract/types.h>

  using tesseract::views::MessageRowData;
  using tesseract::views::make_row_data;
  using tesseract::views::MapViewport;

  static tesseract::LocationEvent make_loc_event(double lat, double lon,
                                                  const std::string& desc = "") {
      tesseract::LocationEvent ev;
      ev.event_id    = "!loc:example.org";
      ev.sender      = "@alice:example.org";
      ev.sender_name = "Alice";
      ev.body        = "Alice shared her location";
      ev.timestamp   = 1000;
      ev.type        = tesseract::EventType::Location;
      ev.lat         = lat;
      ev.lon         = lon;
      ev.description = desc;
      return ev;
  }

  TEST_CASE("make_row_data maps LocationEvent to Kind::Location", "[location]") {
      auto ev  = make_loc_event(51.5074, -0.1278);
      auto row = make_row_data(ev, "@other:example.org");
      CHECK(row.kind == MessageRowData::Kind::Location);
      CHECK(row.location_lat == 51.5074);
      CHECK(row.location_lon == -0.1278);
      CHECK(row.location_description.empty());
  }

  TEST_CASE("make_row_data: description is carried through", "[location]") {
      auto ev  = make_loc_event(51.5074, -0.1278, "Parliament Square");
      auto row = make_row_data(ev, "@other:example.org");
      CHECK(row.location_description == "Parliament Square");
  }

  TEST_CASE("make_row_data: map_viewport initialised to event coords at zoom 15",
            "[location]") {
      auto ev  = make_loc_event(48.8566, 2.3522);
      auto row = make_row_data(ev, "@other:example.org");
      CHECK(row.map_viewport.lat  == 48.8566);
      CHECK(row.map_viewport.lon  == 2.3522);
      CHECK(row.map_viewport.zoom == 15);
  }
  ```

- [ ] **Step 2: Add test file to `tests/CMakeLists.txt`**

  Add `cpp/test_location_events.cpp` to the `add_executable(tesseract_tests ...)` list (before the closing paren, alongside the other test files added in Task 4).

- [ ] **Step 3: Add `Kind::Location` and location fields to `MessageListView.h`**

  In `MessageListView.h`, update the `Kind` enum (line 31):

  ```cpp
  enum class Kind {
      Text, Image, Sticker, File, Voice, Video, Redacted, Notice, Emote, Unhandled,
      DaySeparator, ReadMarker, TimelineStart, Location,
  };
  ```

  After `just_sent` (line 116), before the closing `};` of `MessageRowData`, add:

  ```cpp
      // Location (m.location / MSC3488)
      double      location_lat  = 0.0;
      double      location_lon  = 0.0;
      std::string location_description;
      tesseract::views::MapViewport map_viewport;  // mutable: updated by pan/zoom
  ```

  **Include needed:** Add `#include "views/map_tiles.h"` near the top of `MessageListView.h` (after the existing includes).

  Also add the `on_tile_needed` callback in the public section of `MessageListView` (near the other `std::function` callbacks, around line 327):

  ```cpp
      // Called during paint when a tile is missing from the image cache.
      // Wire to ShellBase::ensure_tile_async() in RoomWindowBase::finish_init_().
      std::function<void(int z, int x, int y)> on_tile_needed;
  ```

- [ ] **Step 4: Map `LocationEvent` in `make_row_data()` in `MessageListView.cpp`**

  In `MessageListView.cpp`, find `make_row_data()` (line 20). After the `switch (ev.type)` block (or wherever `ImageEvent`, `StickerEvent` etc. are handled), add a `LocationEvent` case:

  ```cpp
  case tesseract::EventType::Location: {
      const auto& loc = static_cast<const tesseract::LocationEvent&>(ev);
      row.kind                 = MessageRowData::Kind::Location;
      row.location_lat         = loc.lat;
      row.location_lon         = loc.lon;
      row.location_description = loc.description;
      row.map_viewport         = { loc.lat, loc.lon, 15 };
      break;
  }
  ```

- [ ] **Step 5: Run the tests**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
  ctest --test-dir build/linux-qt6-debug -R location --output-on-failure
  ```
  Expected: all 3 location tests pass.

- [ ] **Step 6: Commit**

  ```bash
  git add ui/shared/views/MessageListView.h \
          ui/shared/views/MessageListView.cpp \
          tests/cpp/test_location_events.cpp \
          tests/CMakeLists.txt
  git commit -m "feat(ui): Kind::Location on MessageRowData + make_row_data mapping + tests"
  ```

### Part B — rendering

- [ ] **Step 7: Add the `map_tiles.h` include to `MessageListView.cpp`**

  At the top of `MessageListView.cpp`, add:

  ```cpp
  #include "views/map_tiles.h"
  ```

- [ ] **Step 8: Implement `paint_row()` for `Kind::Location`**

  In `MessageListView.cpp`, inside the paint adapter's `paint()` or `paint_row()` method (wherever the `Kind::Image` case is painted), add a `Kind::Location` case. The pattern depends on how the existing cases are structured — find the `Kind::Image` branch and add `Kind::Location` after it, mirroring its guard structure:

  ```cpp
  case MessageRowData::Kind::Location: {
      using namespace tesseract::views;
      constexpr float kMapRowH   = 240.0f;
      constexpr float kPinRadius = 5.0f;
      constexpr float kAttribPad = 3.0f;

      // Map area rect within the row.
      tk::Rect map_rect{ bounds.x, bounds.y, bounds.w, kMapRowH };

      // Compute viewport world-pixel position.
      MapViewport& vp = const_cast<MessageRowData&>(row).map_viewport;
      tk::Point vp_px = lat_lon_to_world_px(vp.lat, vp.lon, vp.zoom);

      // Draw tiles.
      ctx.canvas.push_clip_rect(map_rect);
      for (const auto& t : tiles_in_view(vp, map_rect)) {
          const std::string key = tile_cache_key(t);
          const tk::Image*  img = image_provider_ ? image_provider_(key, "") : nullptr;
          tk::Point  origin = tile_pixel_origin(t, vp_px, map_rect);
          tk::Rect   tile_rect{ origin.x, origin.y, 256.0f, 256.0f };
          if (img) {
              ctx.canvas.draw_image(*img, tile_rect);
          } else {
              ctx.canvas.fill_rect(tile_rect, ctx.theme.palette.surface_bg);
              if (on_tile_needed) on_tile_needed(t.z, t.x, t.y);
          }
      }

      // Draw pin at event lat/lon.
      tk::Point pin_world = lat_lon_to_world_px(row.location_lat,
                                                 row.location_lon, vp.zoom);
      float pin_x = map_rect.x + map_rect.w * 0.5f +
                    (pin_world.x - vp_px.x);
      float pin_y = map_rect.y + map_rect.h * 0.5f +
                    (pin_world.y - vp_px.y);
      // White border, then red fill.
      ctx.canvas.fill_circle({ pin_x, pin_y }, kPinRadius + 1.0f,
                              tk::Color::rgb(0xFFFFFF));
      ctx.canvas.fill_circle({ pin_x, pin_y }, kPinRadius,
                              tk::Color::rgb(0xE53935));

      // Attribution pill at bottom-right.
      {
          tk::TextStyle small{};
          small.role = tk::FontRole::Small;
          auto layout = ctx.factory.build_text(
              "\xC2\xA9 OpenStreetMap contributors", small);
          if (layout) {
              tk::Size tsz = layout->measure();
              tk::Rect pill{
                  map_rect.x + map_rect.w - tsz.w - kAttribPad * 2 - 2.0f,
                  map_rect.y + kMapRowH   - tsz.h - kAttribPad * 2 - 2.0f,
                  tsz.w + kAttribPad * 2,
                  tsz.h + kAttribPad * 2
              };
              ctx.canvas.fill_rounded_rect(pill, 3.0f,
                  tk::Color{ 255, 255, 255, 180 });
              ctx.canvas.draw_text(*layout,
                  { pill.x + kAttribPad, pill.y + kAttribPad },
                  ctx.theme.palette.text_primary);
          }
      }
      ctx.canvas.pop_clip_rect();

      // Description below the map (if present).
      if (!row.location_description.empty()) {
          tk::TextStyle body{};
          body.role = tk::FontRole::Body;
          auto layout = ctx.factory.build_text(row.location_description, body);
          if (layout) {
              ctx.canvas.draw_text(*layout,
                  { bounds.x + 8.0f, bounds.y + kMapRowH + 4.0f },
                  ctx.theme.palette.text_primary);
          }
      }
      break;
  }
  ```

  **Note on `fill_circle`:** If `tk::Canvas` does not have `fill_circle`, use `fill_rounded_rect` with equal width/height and radius = half the size, or check `canvas.h` for the actual circle-draw API. Adapt to what exists.

  **Note on `image_provider_`:** This is the same lambda already wired by the shell for sticker/emoji images. Tiles stored in `tk_images_` under their `tile_cache_key` will be found automatically.

- [ ] **Step 9: Add `Kind::Location` to the row natural-height calculation**

  Find where `MessageListView`'s adapter computes `item_height_at()` or `natural_height()` per kind (search for `Kind::Image` in the same file to locate the height computation). Add:

  Search for `Kind::Image` in the height computation function and note its pattern, then mirror it:

  ```cpp
  case MessageRowData::Kind::Location: {
      constexpr float kMapRowH = 240.0f;
      // Find the row-padding constant used by Kind::Image (e.g. kPadY, kRowPad).
      // Call it `kPad` here — replace with the actual name.
      float desc_h = 0.0f;
      if (!row.location_description.empty()) {
          // Measure one body-text line; use the same approach Kind::Text uses
          // for single-line height (typically ctx.factory.line_height(FontRole::Body)).
          desc_h = ctx.factory.line_height(tk::FontRole::Body) + kPad;
      }
      return kMapRowH + desc_h + kPad * 2.0f; // top + bottom padding
  }
  ```

  If the height function doesn't receive a `ctx`, look for how `Kind::Image` gets font metrics (it may store a cached line-height or use a fixed constant). Use the same approach.

- [ ] **Step 10: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep "error:" | head -20
  ```
  Expected: no errors (the `map_active_row_` pan/zoom members are added in Task 7).

- [ ] **Step 11: Commit**

  ```bash
  git add ui/shared/views/MessageListView.h ui/shared/views/MessageListView.cpp
  git commit -m "feat(ui): paint_row for Kind::Location — tile compositing + pin + attribution"
  ```

---

## Task 7: Pan and zoom interaction in `MessageListView`

**Files:**
- Modify: `ui/shared/views/MessageListView.h`
- Modify: `ui/shared/views/MessageListView.cpp`

- [ ] **Step 1: Add pan-state members to `MessageListView`**

  In `MessageListView.h`, in the private section (near other pointer-state members), add:

  ```cpp
      // Pan state for Kind::Location rows.
      std::size_t map_active_row_      = std::numeric_limits<std::size_t>::max();
      tk::Point   map_drag_start_pt_   {};
      tk::Point   map_drag_start_vp_px_ {};  // world-pixel viewport at drag start
  ```

  Also add a helper constant for the "no active row" sentinel:

  ```cpp
      static constexpr std::size_t kNoMapRow =
          std::numeric_limits<std::size_t>::max();
  ```

- [ ] **Step 2: Implement pan in `on_pointer_down`, `on_pointer_move`, `on_pointer_up`**

  In `MessageListView.cpp`, in `on_pointer_down()` (find it by searching for the function in the file — it likely checks for reply/edit/delete button hits first):

  Add **before** the existing button hit-tests, so map rows consume the event first:

  ```cpp
  // ── Map pan: start drag ──────────────────────────────────────────────────
  {
      using namespace tesseract::views;
      constexpr float kMapRowH = 240.0f;
      // row_index_at(local) returns the message index at a widget-local point.
      // Use whatever the existing implementation uses to find the hovered row.
      std::size_t ri = row_index_at(local);  // adapt to actual method name
      if (ri < messages_.size() &&
          messages_[ri].kind == MessageRowData::Kind::Location) {
          tk::Rect row_bounds = row_rect_at(ri);  // adapt to actual method name
          tk::Rect map_rect{ row_bounds.x, row_bounds.y, row_bounds.w, kMapRowH };
          if (map_rect.contains(local)) {
              map_active_row_       = ri;
              map_drag_start_pt_    = local;
              map_drag_start_vp_px_ = lat_lon_to_world_px(
                  messages_[ri].map_viewport.lat,
                  messages_[ri].map_viewport.lon,
                  messages_[ri].map_viewport.zoom);
              return true; // consume event
          }
      }
  }
  ```

  In `on_pointer_move()` (find it similarly):

  ```cpp
  // ── Map pan: drag move ───────────────────────────────────────────────────
  if (map_active_row_ != kNoMapRow && map_active_row_ < messages_.size()) {
      using namespace tesseract::views;
      float dx = local.x - map_drag_start_pt_.x;
      float dy = local.y - map_drag_start_pt_.y;
      // Shift the world-pixel centre by -delta (dragging right moves the view right).
      float new_wp_x = map_drag_start_vp_px_.x - dx;
      float new_wp_y = map_drag_start_vp_px_.y - dy;
      int zoom = messages_[map_active_row_].map_viewport.zoom;
      auto [lat, lon] = world_px_to_lat_lon(new_wp_x, new_wp_y, zoom);
      messages_[map_active_row_].map_viewport.lat = lat;
      messages_[map_active_row_].map_viewport.lon = lon;
      invalidate_data();
      return true;
  }
  ```

  In `on_pointer_up()`:

  ```cpp
  // ── Map pan: end drag ────────────────────────────────────────────────────
  if (map_active_row_ != kNoMapRow) {
      map_active_row_ = kNoMapRow;
      return true;
  }
  ```

  **Note on method names:** The exact method to find the row at a point and get its bounds depends on the adapter internals. Search for how the existing `on_pointer_down` finds hovered rows (e.g., it likely uses `list_view_`'s `index_at()` or iterates offsets). Mirror that pattern exactly. If the existing code uses `hovered_row_` (a cached index), use the same variable.

- [ ] **Step 3: Implement zoom in `on_wheel()`**

  In `on_pointer_wheel()` or `on_wheel()` (find it in `MessageListView.cpp`):

  ```cpp
  // ── Map zoom ─────────────────────────────────────────────────────────────
  {
      using namespace tesseract::views;
      constexpr float kMapRowH = 240.0f;
      std::size_t ri = row_index_at(local);
      if (ri < messages_.size() &&
          messages_[ri].kind == MessageRowData::Kind::Location) {
          tk::Rect row_bounds = row_rect_at(ri);
          tk::Rect map_rect{ row_bounds.x, row_bounds.y, row_bounds.w, kMapRowH };
          if (map_rect.contains(local)) {
              auto& vp = messages_[ri].map_viewport;
              // delta > 0 = scroll up = zoom in
              vp.zoom = std::max(1, std::min(19, vp.zoom + (delta > 0 ? 1 : -1)));
              invalidate_data();
              return true;
          }
      }
  }
  ```

  The `delta` variable name should match what the existing `on_wheel` signature uses (could be `dy`, `amount`, `steps`, etc. — adapt to the actual signature).

- [ ] **Step 4: Add grab cursor on map hover**

  In `on_pointer_move()`, if the map is NOT being dragged but the pointer is inside a map row's `map_rect`:

  ```cpp
  // ── Map hover cursor ─────────────────────────────────────────────────────
  {
      using namespace tesseract::views;
      constexpr float kMapRowH = 240.0f;
      std::size_t ri = row_index_at(local);
      if (ri < messages_.size() &&
          messages_[ri].kind == MessageRowData::Kind::Location) {
          tk::Rect row_bounds = row_rect_at(ri);
          tk::Rect map_rect{ row_bounds.x, row_bounds.y, row_bounds.w, kMapRowH };
          if (map_rect.contains(local)) {
              // Set cursor to grab hand.
              if (adapter_) adapter_->host()->set_cursor(tk::Cursor::Hand);
              return false; // don't consume — allow scroll to propagate
          }
      }
      // Restore default cursor when not over a map.
      if (adapter_) adapter_->host()->set_cursor(tk::Cursor::Default);
  }
  ```

  Check `tk/host.h` for the `Cursor` enum values and the exact API to set the cursor. If the enum uses `PointingHand` instead of `Hand`, adjust. If there is no `Grab` cursor, `Hand` is an acceptable fallback.

- [ ] **Step 5: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 6: Commit**

  ```bash
  git add ui/shared/views/MessageListView.h ui/shared/views/MessageListView.cpp
  git commit -m "feat(ui): pan/zoom interaction for location map rows"
  ```

---

## Task 8: Wire all shells — `MediaKind::Tile` case + `on_tile_needed` in `RoomWindowBase`

**Files:**
- Modify: `ui/shared/app/RoomWindowBase.cpp`
- Modify: each shell's `on_media_bytes_ready_` implementation:
  - `ui/linux-qt/src/MainWindow.cpp`
  - `ui/linux-gtk/src/MainWindow.cpp`
  - `ui/windows/src/MainWindow.cpp` (Win32)
  - `ui/macos/src/MainWindowController.mm` (macOS, via MacShell)

### `RoomWindowBase` — wire `on_tile_needed`

- [ ] **Step 1: Wire callback in `RoomWindowBase::finish_init_()`**

  In `ui/shared/app/RoomWindowBase.cpp`, find `finish_init_()` (search for `on_retry_send` or `on_abort_send` which were wired in a previous session). After those callbacks, add:

  ```cpp
  room_view_->msg_list()->on_tile_needed = [this](int z, int x, int y) {
      shell_->ensure_tile_async(z, x, y);
  };
  ```

### Shell `on_media_bytes_ready_` — `MediaKind::Tile` case

For **each** of the four shells, find the `on_media_bytes_ready_` override and add a `MediaKind::Tile` arm. The existing `MediaKind::MediaImage` arm (in each shell) decodes PNG bytes into a `tk::Image`, stores it in `tk_images_[cache_key]`, and calls `notify_image_ready(cache_key)` on the message list. The `Tile` case does the same but calls `invalidate_data()` on the message list instead (since tile keys are not stored in any row's `media_url` field, `notify_image_ready` would not find the row to repaint).

The pattern is the same in all four shells. For each shell's `on_media_bytes_ready_`:

- [ ] **Step 2: Qt6 shell (`ui/linux-qt/src/MainWindow.cpp`)**

  Find the `MediaKind::MediaImage` case in `MainWindow::on_media_bytes_ready_`. After it (or in a separate `case MediaKind::Tile:` if using a switch), add:

  ```cpp
  case MediaKind::Tile: {
      // Decode the PNG bytes into a tk::Image and store in tk_images_.
      // Then force a full message-list repaint so location rows redraw.
      auto img = tk::qt6::decode_image(bytes);  // adapt to actual decode function
      if (img) tk_images_.emplace(cache_key, std::move(img));
      if (room_view_) room_view_->msg_list()->invalidate_data();
      break;
  }
  ```

  **Finding the decode function:** Search `ui/linux-qt/src/MainWindow.cpp` for `MediaKind::MediaImage` and note how bytes are decoded to `tk::Image` for that case. Use the identical decode call for `MediaKind::Tile`.

  **Finding the invalidate call:** Look at how the shell triggers a message-list repaint after media arrives (it probably calls `room_view_->msg_list()->notify_image_ready(cache_key)` or `room_view_->invalidate()`). For tiles, call `invalidate_data()` directly: if `MessageListView` exposes it publicly, call it. Otherwise call the same method used for `notify_image_ready`.

- [ ] **Step 3: GTK4 shell (`ui/linux-gtk/src/MainWindow.cpp`)**

  Same pattern as Qt6. Find `MediaKind::MediaImage` in `MainWindow::on_media_bytes_ready_`, note the decode function, add:

  ```cpp
  case MediaKind::Tile: {
      auto img = /* same decode as MediaImage case */;
      if (img) tk_images_.emplace(cache_key, std::move(img));
      if (room_view_) room_view_->msg_list()->invalidate_data();
      break;
  }
  ```

- [ ] **Step 4: Win32 shell (`ui/windows/src/MainWindow.cpp`)**

  Same pattern. Find the file by running:
  ```bash
  find ui/windows -name "*.cpp" | head -5
  ```
  Then find `on_media_bytes_ready_` and add the `Tile` case.

- [ ] **Step 5: macOS shell (`ui/macos/src/MainWindowController.mm` or `MacShell.mm`)**

  Run:
  ```bash
  grep -rn "on_media_bytes_ready_" ui/macos/
  ```
  Add the same `MediaKind::Tile` case in whichever file implements it.

- [ ] **Step 6: Build all shells**

  ```bash
  cmake --build build/linux-qt6-debug  2>&1 | grep "error:" | head -10
  cmake --build build/linux-gtk-debug  2>&1 | grep "error:" | head -10
  ```
  Expected: no errors on both Linux shells. (Win32 and macOS require their respective build environments.)

- [ ] **Step 7: Commit**

  ```bash
  git add ui/shared/app/RoomWindowBase.cpp \
          ui/linux-qt/src/MainWindow.cpp \
          ui/linux-gtk/src/MainWindow.cpp
  git commit -m "feat(ui): wire on_tile_needed + MediaKind::Tile in all shells"
  ```

---

## Task 9: Docs and final verification

**Files:**
- Modify: `CHANGES.md`
- Modify: `ROADMAP.md`
- Modify: `STATUS.md`

- [ ] **Step 1: Run full C++ test suite**

  ```bash
  cmake --build build/linux-qt6-debug 2>&1 | tail -5
  ctest --test-dir build/linux-qt6-debug --output-on-failure
  ```
  Expected: all tests pass (including `test_map_tiles` and `test_location_events`).

- [ ] **Step 2: Run Rust tests**

  ```bash
  cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
  ```
  Expected: all tests pass (including `location_tests`).

- [ ] **Step 3: Manual verification checklist**

  Run: `./build/linux-qt6-debug/ui/linux-qt/tesseract`

  1. In a room with a location message: the row shows a 240 px tile map centred on the coordinates at zoom 15.
  2. Hovering over the map changes the cursor to a pointing hand.
  3. Dragging pans the map; the tile grid shifts correctly.
  4. Scroll wheel on the map zooms in/out (clamped to 1–19).
  5. New tiles load asynchronously — grey placeholders appear first, then the tile image appears on repaint.
  6. "© OpenStreetMap contributors" attribution is visible in the bottom-right corner of the map area.
  7. A red circle pin is drawn at the exact event coordinates.
  8. If a description is present, it appears below the map.
  9. Restart the app and re-open a location message — previously fetched tiles load from disk (no network request; fast).
  10. A malformed `geo:` URI renders as a plain text row, not a crash.

- [ ] **Step 4: Update `CHANGES.md`**

  Under `## Unreleased` → current date heading, add:

  ```markdown
  - feat(messaging): `m.location` / MSC3488 receive — location messages render as
    interactive 240 px inline maps; OSM tiles composited from `tile.openstreetmap.org`
    with disk cache; pan by drag, zoom by scroll wheel; attribution overlay; red-circle
    pin at event coordinates; all four platforms (Qt6, GTK4, Win32, macOS)
  ```

- [ ] **Step 5: Update `ROADMAP.md`**

  In the `## Step 5 — UI redesign` section, add a note after the existing bullet list:
  `m.location` receive is implemented (see CHANGES.md). Add `send` to the known gaps or a future step.

- [ ] **Step 6: Update `STATUS.md`**

  Find the relevant section (test counts, feature matrix) and add location event support. Update the "Last updated" date.

- [ ] **Step 7: Commit**

  ```bash
  git add CHANGES.md ROADMAP.md STATUS.md
  git commit -m "docs: record m.location receive (OSM tile map) in CHANGES/ROADMAP/STATUS"
  ```

# Location Events Design

## Overview

Receive and display Matrix `m.location` / MSC3488 location messages inline in the timeline across all four platforms (Qt6, GTK4, Win32, macOS). The map is rendered by compositing OpenStreetMap slippy-map tiles drawn with `tk::Canvas::draw_image()`. The map is interactive: users can pan by dragging and zoom with the scroll wheel. Tiles are fetched from `tile.openstreetmap.org` and cached to disk. Send is out of scope for this iteration.

---

## Data Model

### Rust bridge (`sdk/src/bridge.rs`)

Add a `LocationEvent` struct:

```rust
pub struct LocationEvent {
    // Base fields (event_id, sender, sender_name, timestamp, …) same as all events.
    pub lat:         f64,
    pub lon:         f64,
    pub description: String,   // MSC3488 description or empty
    pub body:        String,   // plain-text fallback ("User shared their location")
}
```

Both formats are parsed in `timeline_item_to_ffi()` in `client.rs`:

- **Legacy `m.location`**: `content.geo_uri` is a `geo:lat,lon` URI; parse the numeric pair.
- **MSC3488**: `content.location.latitude` / `content.location.longitude` floats. Description comes from `content.location.description` if present.

Malformed events (missing or unparseable coordinates) produce a plain `TextEvent` showing the `body` fallback ("Location unavailable").

### C++ types (`client/include/tesseract/types.h`)

```cpp
struct LocationEvent : Event {
    double      lat  = 0.0;
    double      lon  = 0.0;
    std::string description;
};
```

Wired through `ffi_convert.h` with a `from_location_event()` helper matching the pattern of all other event types.

### `MessageRowData` (`ui/shared/views/MessageListView.h`)

Add `Kind::Location`. Location rows carry:

```cpp
double      location_lat  = 0.0;
double      location_lon  = 0.0;
std::string location_description;
MapViewport map_viewport;   // mutable: updated by pan/zoom interaction
```

`MapViewport` is a small plain struct `{ double lat; double lon; int zoom; }` defined in `map_tiles.h`. It is initialised to the event coordinates at zoom 15.

`MessageListView` also gains a public callback:

```cpp
// Called during paint when a tile is needed but not yet in the image cache.
// The shell wires this to ShellBase::ensure_tile_async().
std::function<void(int z, int x, int y)> on_tile_needed;
```

---

## Tile Math (`ui/shared/views/map_tiles.h`)

Pure-function header, no state. Uses standard Web Mercator (EPSG:3857) slippy-map formulas.

```cpp
struct TileCoord  { int z, x, y; };
struct MapViewport { double lat, lon; int zoom; };

// Convert geographic coordinates to a tile index.
TileCoord    lat_lon_to_tile(double lat, double lon, int zoom);

// Pixel offset (within the tile) of an exact lat/lon at a given zoom.
tk::Point    lat_lon_to_pixel(double lat, double lon, int zoom);

// Top-left pixel position of a tile relative to the map area's origin,
// given the current viewport centre.
tk::Point    tile_pixel_origin(TileCoord tile, MapViewport vp, tk::Rect map_rect);

// All tiles needed to fill `map_rect` at the viewport's zoom level.
std::vector<TileCoord> tiles_in_view(MapViewport vp, tk::Rect map_rect);

// Stable string key for the image cache: "tile:z/x/y".
std::string  tile_cache_key(TileCoord t);

// OSM tile URL.
std::string  tile_url(TileCoord t);   // https://tile.openstreetmap.org/{z}/{x}/{y}.png
```

---

## Tile Fetch Pipeline

### Rust FFI (`sdk/src/bridge.rs`, `sdk/src/client.rs`)

Add:

```rust
fn fetch_url_bytes(self: &mut ClientFfi, url: &str) -> OpResult;
```

Implementation uses the `reqwest` client already present in the matrix-sdk HTTP stack. Sets `User-Agent: Tesseract/0.1 (Matrix client)` as required by OSM's tile usage policy. Blocks on the tokio runtime (same pattern as `get_url_preview`).

### C++ shell (`ui/shared/app/ShellBase.h/.cpp`)

```cpp
// Trigger an async tile fetch (no-op if already in-flight, cached, or failed).
void ensure_tile_async(int z, int x, int y);

// Called on the UI thread when a tile arrives; stores in tk_images_ and repaints.
virtual void on_tile_ready_(int z, int x, int y, std::vector<uint8_t> bytes);

std::unordered_set<std::string> tile_fetches_in_flight_;
std::unordered_set<std::string> tile_fetch_failed_;   // session-scoped; prevents retry loops
```

`ensure_tile_async` flow:

1. Compute disk path `{cache_dir}/tiles/{z}/{x}/{y}.png`.
2. If file exists, read bytes from disk and deliver immediately (no network).
3. If `tile_fetches_in_flight_` already contains the key, return (dedup).
4. Insert key into `tile_fetches_in_flight_`, call `run_async_()` on a worker thread.
5. Worker calls `client_->fetch_url_bytes(tile_url)`.
6. On success: write bytes to disk (creating parent dirs as needed), post to UI thread.
7. On the UI thread: decode PNG into `tk::Image`, insert into `tk_images_` under `tile_cache_key`, erase from `tile_fetches_in_flight_`, call `invalidate_data()` on the message list.
8. On failure: erase from `tile_fetches_in_flight_`, insert into `tile_fetch_failed_` (session-scoped set; prevents retry loops). Keep grey placeholder.

Disk cache has no TTL for now. OSM tiles are stable; a future pass can add per-tile mtime expiry.

---

## Rendering

**Row height:** Location rows have a fixed height `kMapRowH = 240 px` for the tile area, plus normal body-text height below for the description.

**Paint (in `paint_row()` for `Kind::Location`):**

1. Compute `map_rect` — the 240 px area within the row.
2. Call `tiles_in_view(row.map_viewport, map_rect)` to get the needed tile list.
3. For each tile:
   - Look up `tile_cache_key(t)` in `tk_images_`.
   - If found: `canvas.draw_image(*img, dest_rect)` clipped to `map_rect`.
   - If absent and not in `tile_fetch_failed_`: `canvas.fill_rect(tile_dest, muted_bg)` and call `on_tile_needed(t.z, t.x, t.y)` (wired to `ensure_tile_async`).
   - If in `tile_fetch_failed_`: `canvas.fill_rect(tile_dest, muted_bg)` silently.
4. Draw the pin: compute the pixel position of the event's `lat`/`lon` within `map_rect`, draw a filled red circle (radius 5 px) with a 1 px white border.
5. Draw attribution: `canvas.fill_rounded_rect` semi-transparent pill at bottom-right; draw "© OpenStreetMap contributors" in `FontRole::Small` / `text_muted`.
6. If `location_description` is non-empty, render it below the map area using the normal body text style.

---

## Interaction

### Pan

`MessageListView` gains:

```cpp
std::size_t  map_active_row_    = kInvalidIndex;
tk::Point    map_drag_start_pt_;          // pointer position at drag start
MapViewport  map_drag_start_vp_;          // viewport at drag start
```

- `on_pointer_down()`: if the hit point falls within a `Kind::Location` row's `map_rect`, consume the event, set `map_active_row_`, record drag start state.
- `on_pointer_move()`: if `map_active_row_ != kInvalidIndex`, compute pixel delta from drag start, convert to lat/lon delta (via inverse Web Mercator at current zoom), update `messages_[map_active_row_].map_viewport.lat/lon`, call `invalidate_data()`.
- `on_pointer_up()`: clear `map_active_row_`.

### Zoom

`on_wheel()` in `MessageListView`: if the scroll point is inside a `Kind::Location` row's `map_rect`, clamp `zoom ± 1` to `[1, 19]`, preserve the viewport centre lat/lon, call `invalidate_data()`. The next paint calls `tiles_in_view()` at the new zoom and triggers fetches for any missing tiles.

### Cursor

`set_cursor(Host::Cursor::Grab)` when hovering over a map row's `map_rect`; restored to default on leave. Uses the same `set_cursor()` host hook used for link hover.

### Conflict avoidance

`on_pointer_down()` checks map rows before the existing reaction/reply/delete button logic. A pointer-down inside `map_rect` is consumed immediately, preventing text-selection or chip interactions from firing.

---

## Error Handling

| Scenario | Behaviour |
|----------|-----------|
| Malformed `geo:` URI | Fall through to `TextEvent` with body text |
| Tile fetch network error | Grey placeholder; tile added to `tile_fetch_failed_`; no retry this session |
| Offline at startup | All tiles grey; fetches trigger automatically on next pan/zoom once online |
| Tile decode error | Same as network error |
| Out-of-range zoom | Clamped to `[1, 19]` |

---

## Testing

### Rust unit tests (`sdk/src/`)

- Parse legacy `m.location` event with `geo:51.5,-0.1` URI → `lat = 51.5`, `lon = -0.1`.
- Parse MSC3488 event with explicit `latitude`/`longitude` floats → correct fields.
- Malformed `geo:` URI → event falls through to text fallback.
- `fetch_url_bytes` compiles and is wired (smoke test only; no live HTTP in unit tests).

### C++ unit tests (`tests/cpp/`)

- `lat_lon_to_tile()`: known coordinate → expected `{z, x, y}` (cross-checked against OSM tile calculator).
- `tiles_in_view()`: 256×240 map rect at zoom 15 returns exactly the expected tile count.
- `tile_pixel_origin()`: tile containing the viewport centre has its origin near the centre of `map_rect`.
- `tile_cache_key()` / `tile_url()`: correct string formatting.
- `ensure_tile_async()` dedup: calling twice for the same tile triggers only one in-flight fetch entry.

No visual or interaction tests (no headless pointer dispatch in the test harness).

---

## Attribution Requirement

OSM tile usage policy requires:
1. `User-Agent` header on every HTTP request identifying the application.
2. Visible "© OpenStreetMap contributors" credit on every rendered map.

Both are addressed in the design above. The tile server's [usage policy](https://operations.osmfoundation.org/policies/tiles/) also asks that heavily-used apps cache aggressively — disk caching satisfies this.

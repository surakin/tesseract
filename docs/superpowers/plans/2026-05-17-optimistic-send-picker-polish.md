# Optimistic Send + Picker Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add SDK-driven local echo for sent messages with clock/checkmark/error indicators, fix GridView hover tracking, and add shortcode tooltips to emoji and sticker pickers.

**Architecture:** Two independent features. (1) Optimistic send: `sdk/src/client.rs` switches `send_message()` to `timeline.send()`, populates four new `pending_*` fields on `TimelineEvent`, and adds `retry_send`/`abort_send` FFI fns. These flow through `types.h → ffi_convert.h → MessageRowData → paint_row()` where own-message rows gain a clock/checkmark/error indicator. (2) Picker polish: `GridView` gains `on_pointer_move`/`on_pointer_leave` overrides (it already has `hovered_index_` and `index_at()`), and both pickers draw an inline tooltip label when `grid_->hovered_index() >= 0`.

**Tech Stack:** Rust (matrix-sdk-ui 0.17 `Timeline::send()`, `EventSendState`), C++ (tesseract_tk, Catch2 tests)

---

## File Inventory

| File | Change |
|------|--------|
| `sdk/src/bridge.rs` | +4 pending fields on `TimelineEvent`; +`retry_send`, `abort_send` FFI fns |
| `sdk/src/client.rs` | `send_message()` → `timeline.send()`; pending state extraction; `retry_send`, `abort_send` impl |
| `client/include/tesseract/types.h` | +4 pending fields on `Event` base struct |
| `client/src/ffi_convert.h` | `assign_base()` copies new pending fields |
| `client/include/tesseract/client.h` | +`retry_send`, `abort_send` declarations |
| `client/src/client.cpp` | Implement `retry_send`, `abort_send` |
| `ui/shared/views/MessageListView.h` | `PendingState` enum + fields on `MessageRowData`; `RowChipGeom` retry/abort rects; `HoverTarget` extensions; new callbacks + `clear_just_sent()` |
| `ui/shared/views/MessageListView.cpp` | `make_row_data()` pending mapping; `update_message()` just-sent detection + 2s timer; `paint_row()` pending indicators; `on_pointer_down()` retry/abort hit-test |
| `ui/shared/app/RoomWindowBase.h/.cpp` | Wire `on_retry_send`, `on_abort_send`, `on_just_sent` callbacks |
| `ui/shared/tk/list_view.h` | `GridView`: +`on_pointer_move`, `on_pointer_leave`, `hovered_index()`, `rect_at()` |
| `ui/shared/tk/list_view.cpp` | `GridView`: implement the four new methods |
| `ui/shared/views/EmojiPicker.h` | +`on_pointer_move`, `on_pointer_leave`, `hovered_grid_cell_`, `current_shortcodes_` |
| `ui/shared/views/EmojiPicker.cpp` | `rebuild_current_items()` populates shortcodes; `paint()` inline tooltip |
| `ui/shared/views/StickerPicker.h` | Same as EmojiPicker |
| `ui/shared/views/StickerPicker.cpp` | Same as EmojiPicker |
| `tests/cpp/test_tk_lists.cpp` | GridView hover tests |
| `tests/cpp/test_pending_send.cpp` | New file: `make_row_data` pending mapping, `update_message` just-sent |

---

## Task 1: Add pending fields + FFI functions to bridge.rs

**Files:**
- Modify: `sdk/src/bridge.rs` (lines 179–180, after the `TimelineEvent` struct; lines 503–505 for new fns)

- [ ] **Step 1: Add 4 pending fields to `TimelineEvent`**

  In `sdk/src/bridge.rs`, after line 179 (`image_animated: bool,`), add:

  ```rust
        /// Local-echo send state: "sending" | "failed" | "" (server event).
        pending_state:        String,
        /// Human-readable error message when `pending_state == "failed"`.
        pending_error:        String,
        /// True when the failure is recoverable (retry re-enables the queue).
        pending_recoverable:  bool,
        /// Transaction ID of the pending local echo (for abort_send).
        pending_txn_id:       String,
  ```

- [ ] **Step 2: Add `retry_send` and `abort_send` FFI function signatures**

  In `sdk/src/bridge.rs`, after line 505 (`fn send_message(...) -> OpResult;`), add:

  ```rust
          /// Re-enable the send queue for `room_id` after a recoverable failure.
          /// The SDK automatically retries all pending sends.
          fn retry_send(self: &mut ClientFfi, room_id: &str) -> OpResult;

          /// Abort a pending local echo identified by `txn_id` in `room_id`.
          fn abort_send(self: &mut ClientFfi, room_id: &str, txn_id: &str) -> OpResult;
  ```

- [ ] **Step 3: Run Rust tests to verify bridge compiles**

  ```bash
  cargo test -p tesseract-sdk-ffi 2>&1 | tail -20
  ```
  Expected: compilation error about missing struct fields in `TimelineEvent` literals — that's expected at this stage. Fix by adding the 4 fields with defaults to every existing `TimelineEvent { .. }` literal in `client.rs` (done in Task 2).

  Actually at bridge-only stage the Rust tests should compile since `bridge.rs` is behind `#[cfg(not(test))]`. Run:
  ```bash
  cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
  ```
  Expected: tests pass (bridge is excluded from test build).

- [ ] **Step 4: Commit**

  ```bash
  git add sdk/src/bridge.rs
  git commit -m "feat(sdk/bridge): add pending_* fields and retry/abort_send FFI stubs"
  ```

---

## Task 2: Implement pending-state logic in client.rs

**Files:**
- Modify: `sdk/src/client.rs`

- [ ] **Step 1: Add EventSendState import**

  Near the top of `client.rs` (around line 29, with the other `matrix_sdk_ui` imports), the `use matrix_sdk_ui { ... }` block already exists. Inside it or below it, add:

  ```rust
  use matrix_sdk_ui::timeline::EventSendState;
  ```

- [ ] **Step 2: Switch `send_message()` to use `timeline.send()`**

  Replace the `send_message` function body (lines 1797–1813) with:

  ```rust
  pub fn send_message(&mut self, room_id: &str, body: &str, formatted_body: &str) -> OpResult {
      let Some(client) = self.client.clone() else { return err("not logged in") };
      let room_id = match matrix_sdk::ruma::RoomId::parse(room_id) {
          Ok(id) => id,
          Err(e) => return err(format!("invalid room id: {e}")),
      };
      let content = if formatted_body.is_empty() {
          RoomMessageEventContent::text_plain(body)
      } else {
          RoomMessageEventContent::text_html(body, formatted_body)
      };
      // Use the live timeline if subscribed — local echo fires immediately.
      if let Some(handle) = self.timelines.get(&room_id) {
          let timeline = handle.timeline.clone();
          return match self.rt.block_on(async move { timeline.send(content.into()).await }) {
              Ok(_)  => ok(""),
              Err(e) => err(e.to_string()),
          };
      }
      // Fallback: no timeline subscribed for this room.
      let Some(room) = client.get_room(&room_id) else { return err("room not found") };
      match self.rt.block_on(async move { room.send(content).await }) {
          Ok(_)  => ok(""),
          Err(e) => err(e.to_string()),
      }
  }
  ```

- [ ] **Step 3: Add `retry_send()` implementation**

  After `send_message`, add:

  ```rust
  pub fn retry_send(&mut self, room_id: &str) -> OpResult {
      let Some(client) = self.client.clone() else { return err("not logged in") };
      let room_id: OwnedRoomId = match room_id.parse() {
          Ok(id) => id,
          Err(e) => return err(format!("invalid room id: {e}")),
      };
      let Some(room) = client.get_room(&room_id) else { return err("room not found") };
      room.send_queue().set_enabled(true);
      ok("")
  }
  ```

- [ ] **Step 4: Add `abort_send()` implementation**

  After `retry_send`, add:

  ```rust
  pub fn abort_send(&mut self, room_id: &str, txn_id: &str) -> OpResult {
      use matrix_sdk_ui::timeline::TimelineEventItemId;
      let room_id: OwnedRoomId = match room_id.parse() {
          Ok(id) => id,
          Err(e) => return err(format!("invalid room id: {e}")),
      };
      let txn_id: matrix_sdk::ruma::OwnedTransactionId = txn_id.into();
      let Some(handle) = self.timelines.get(&room_id) else {
          return err("no timeline for room");
      };
      let timeline = handle.timeline.clone();
      let item_id = TimelineEventItemId::TransactionId(txn_id);
      match self.rt.block_on(async move { timeline.redact(&item_id, None).await }) {
          Ok(_)  => ok(""),
          Err(e) => err(e.to_string()),
      }
  }
  ```

- [ ] **Step 5: Extract pending state in `timeline_item_to_ffi()`**

  In `timeline_item_to_ffi` (line 4073), before the `if let TimelineItemContent::MsgLike ... Redacted` block (line 4138), add a helper that computes pending state for any `event_item`:

  ```rust
  // Compute pending fields once for all non-virtual event paths.
  let (pending_state, pending_error, pending_recoverable, pending_txn_id) =
      if event_item.is_local_echo() {
          let txn = event_item.transaction_id()
              .map(|t| t.to_string())
              .unwrap_or_default();
          match event_item.send_state() {
              Some(EventSendState::NotSentYet) =>
                  ("sending".to_owned(), String::new(), false, txn),
              Some(EventSendState::SendingFailed { error, is_recoverable }) =>
                  ("failed".to_owned(), error.to_string(), *is_recoverable, txn),
              _ => (String::new(), String::new(), false, txn),
          }
      } else {
          (String::new(), String::new(), false, String::new())
      };
  ```

- [ ] **Step 6: Add pending fields to all `TimelineEvent` struct literals in `timeline_item_to_ffi`**

  There are three `TimelineEvent { ... }` literals in the function:
  1. Virtual items path (line ~4092): add `pending_state: String::new(), pending_error: String::new(), pending_recoverable: false, pending_txn_id: String::new(),`
  2. Redacted path (line ~4150): same defaults (redacted = never a local echo)
  3. Sticker path (line ~4225): use the computed variables: `pending_state: pending_state.clone(), pending_error: pending_error.clone(), pending_recoverable, pending_txn_id: pending_txn_id.clone(),`
  4. Final message path (the `return Some(TimelineEvent { ... })` that follows the big tuple destructuring): `pending_state, pending_error, pending_recoverable, pending_txn_id,`

  Find the final TimelineEvent literal by searching for the one that follows the `match msg_content.msgtype()` block. It's the only one that consumes `body`, `msg_type`, `source_json`, etc. from the tuple.

- [ ] **Step 7: Run Rust tests**

  ```bash
  cargo test -p tesseract-sdk-ffi 2>&1 | tail -10
  ```
  Expected: all tests pass.

- [ ] **Step 8: Commit**

  ```bash
  git add sdk/src/client.rs
  git commit -m "feat(sdk): switch send_message to timeline.send() with local-echo pending state"
  ```

---

## Task 3: Add pending fields to C++ types and FFI conversion

**Files:**
- Modify: `client/include/tesseract/types.h` (line 65, after `bool is_edited = false;`)
- Modify: `client/src/ffi_convert.h` (line 127, after `ev.is_edited = e.is_edited;`)

- [ ] **Step 1: Add pending fields to `Event` base struct in `types.h`**

  In `client/include/tesseract/types.h`, after `bool is_edited = false;` (line 64), add:

  ```cpp
      /// Local-echo send state. "" = server event; "sending" = in-flight;
      /// "failed" = delivery failed.
      std::string pending_state;
      std::string pending_error;
      bool        pending_recoverable = false;
      std::string pending_txn_id;
  ```

- [ ] **Step 2: Copy pending fields in `assign_base()` in `ffi_convert.h`**

  In `client/src/ffi_convert.h`, after `ev.is_edited = e.is_edited;` (line 126), add:

  ```cpp
      ev.pending_state       = std::string(e.pending_state);
      ev.pending_error       = std::string(e.pending_error);
      ev.pending_recoverable = e.pending_recoverable;
      ev.pending_txn_id      = std::string(e.pending_txn_id);
  ```

- [ ] **Step 3: Build to verify**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | tail -15
  ```
  Expected: compiles without errors.

- [ ] **Step 4: Commit**

  ```bash
  git add client/include/tesseract/types.h client/src/ffi_convert.h
  git commit -m "feat(client): propagate pending_* fields from FFI bridge through C++ Event"
  ```

---

## Task 4: Add `retry_send` / `abort_send` to C++ client API

**Files:**
- Modify: `client/include/tesseract/client.h` (after `send_message` around line 181)
- Modify: `client/src/client.cpp` (after `send_message` impl at line 185)

- [ ] **Step 1: Add declarations to `client.h`**

  In `client/include/tesseract/client.h`, after the `send_message` declaration (line 181), add:

  ```cpp
      /// Re-enable the send queue for `room_id` after a recoverable failure.
      Result retry_send(const std::string& room_id);

      /// Abort the pending local echo with `txn_id` in `room_id`.
      Result abort_send(const std::string& room_id, const std::string& txn_id);
  ```

- [ ] **Step 2: Implement in `client.cpp`**

  In `client/src/client.cpp`, after the `send_message` implementation (line 185), add:

  ```cpp
  Result Client::retry_send(const std::string& room_id) {
      return from_ffi(impl_->ffi->retry_send(room_id));
  }

  Result Client::abort_send(const std::string& room_id, const std::string& txn_id) {
      return from_ffi(impl_->ffi->abort_send(room_id, txn_id));
  }
  ```

- [ ] **Step 3: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_client 2>&1 | tail -10
  ```
  Expected: compiles without errors.

- [ ] **Step 4: Commit**

  ```bash
  git add client/include/tesseract/client.h client/src/client.cpp
  git commit -m "feat(client): add retry_send and abort_send to Client API"
  ```

---

## Task 5: Add `PendingState` to `MessageRowData` and update `MessageListView` interface

**Files:**
- Modify: `ui/shared/views/MessageListView.h`

- [ ] **Step 1: Add `PendingState` enum and fields to `MessageRowData`**

  In `ui/shared/views/MessageListView.h`, after `std::string blurhash;` (line 107, last field of `MessageRowData`), before the closing `};`, add:

  ```cpp
      // Optimistic send state (own messages only).
      enum class PendingState { None, Sending, Failed };
      PendingState pending_state      = PendingState::None;
      std::string  pending_txn_id;
      std::string  pending_error;
      bool         pending_recoverable = false;
      // Set for ~2 s after a Sending → None transition to show ✓.
      bool         just_sent           = false;
  ```

- [ ] **Step 2: Add `retry_button` and `abort_button` to `RowChipGeom`**

  In `RowChipGeom` (around line 352), after `tk::Rect delete_button{};`, add:

  ```cpp
          tk::Rect          retry_button{};  // 0-area when not painted
          tk::Rect          abort_button{};  // 0-area when not painted
  ```

- [ ] **Step 3: Extend `HoverTarget` enum**

  Change `enum class HoverTarget { None, Chip, AddButton, Receipt };` (line 364) to:

  ```cpp
      enum class HoverTarget { None, Chip, AddButton, Receipt, RetryButton, AbortButton };
  ```

- [ ] **Step 4: Add public callbacks and `clear_just_sent()`**

  In `MessageListView`'s public interface (after the existing `on_delete_requested` callback around line 277), add:

  ```cpp
      // Fired when a local echo transitions Sending → None (message confirmed
      // by the server). `event_id` is the new server-assigned event ID.
      std::function<void(const std::string& event_id)> on_just_sent;

      // Fired when the user clicks the Retry button on a failed own message.
      std::function<void(const std::string& txn_id)> on_retry_send;

      // Fired when the user clicks the ✕ (abort) button on an unrecoverable
      // failed own message.
      std::function<void(const std::string& txn_id)> on_abort_send;

      // Clear the just_sent flag on the row matching `event_id` and repaint.
      // Called 2 s after on_just_sent fires (via post_delayed_).
      void clear_just_sent(const std::string& event_id);
  ```

- [ ] **Step 5: Build to verify header compiles**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:|warning:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 6: Commit**

  ```bash
  git add ui/shared/views/MessageListView.h
  git commit -m "feat(ui): PendingState enum + fields on MessageRowData; retry/abort callbacks"
  ```

---

## Task 6: Implement pending mapping in `make_row_data()` and just-sent detection in `update_message()`

**Files:**
- Modify: `ui/shared/views/MessageListView.cpp`

- [ ] **Step 1: Map pending fields in `make_row_data()`**

  In `MessageListView.cpp`, in `make_row_data()` (line 20), after `row.is_edited = ev.is_edited;` (line 37) and before the `switch (ev.type)` block, add:

  ```cpp
      using PendingState = MessageRowData::PendingState;
      if (ev.pending_state == "sending") {
          row.pending_state = PendingState::Sending;
      } else if (ev.pending_state == "failed") {
          row.pending_state = PendingState::Failed;
      } else {
          row.pending_state = PendingState::None;
      }
      row.pending_txn_id      = ev.pending_txn_id;
      row.pending_error       = ev.pending_error;
      row.pending_recoverable = ev.pending_recoverable;
  ```

- [ ] **Step 2: Add just-sent detection and 2 s timer in `update_message()`**

  In `update_message()` (line 2258), replace the current assignment + invalidate block:

  Current code (lines 2276–2278):
  ```cpp
      adapter_->invalidate_layout_cache_at(index);
      messages_[index] = std::move(msg);
      invalidate_data();
  ```

  Replace with:
  ```cpp
      // Detect Sending → None transition for own messages (local echo confirmed).
      const bool just_sent_transition =
          messages_[index].is_own &&
          messages_[index].pending_state == MessageRowData::PendingState::Sending &&
          msg.pending_state == MessageRowData::PendingState::None;

      adapter_->invalidate_layout_cache_at(index);
      messages_[index] = std::move(msg);
      if (just_sent_transition) {
          messages_[index].just_sent = true;
          const std::string eid = messages_[index].event_id;
          if (on_just_sent) on_just_sent(eid);
          // Clear the ✓ indicator after 2 s.
          if (post_delayed_) {
              post_delayed_(2000, [this, eid]{ clear_just_sent(eid); });
          }
      }
      invalidate_data();
  ```

- [ ] **Step 3: Implement `clear_just_sent()`**

  Add the implementation after `update_message`:
  ```cpp
  void MessageListView::clear_just_sent(const std::string& event_id) {
      for (auto& row : messages_) {
          if (row.event_id == event_id && row.just_sent) {
              row.just_sent = false;
              invalidate_data();
              return;
          }
      }
  }
  ```

- [ ] **Step 4: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add ui/shared/views/MessageListView.cpp
  git commit -m "feat(ui): map pending fields; just-sent detection with 2s checkmark timer"
  ```

---

## Task 7: Paint pending indicators and hit-test retry/abort in `paint_row()` / `on_pointer_down()`

**Files:**
- Modify: `ui/shared/views/MessageListView.cpp`

The spec requires a small glyph indicator at bottom-right of own-message rows, non-overlapping with read receipts. This task adds indicator drawing to `paint_row()` and hit-testing to `on_pointer_down()`.

- [ ] **Step 1: Draw pending indicator in `paint_row()`**

  In `MessageListView.cpp`, locate `paint_row()` inside the `Adapter` class. Find the section that draws read receipts / reaction chips at the bottom-right of own-message rows. After those are drawn (and after `RowChipGeom` is populated), add a pending-indicator section gated on `row.is_own`:

  The indicator draws right-aligned in the row, below the message body and above or beside read receipts. Store the button rects in `hovered_row_geom_` so `on_pointer_down` can test them.

  ```cpp
  // ── Pending send indicator (own messages only) ──────────────────────
  if (row.is_own &&
      (row.pending_state != MessageRowData::PendingState::None ||
       row.just_sent)) {
      using PS = MessageRowData::PendingState;
      constexpr float kIndPadR = 6.0f;   // right inset from row edge
      constexpr float kIndPadB = 4.0f;   // bottom inset from row edge
      tk::TextStyle small{};
      small.role = tk::FontRole::Small;

      if (row.just_sent || row.pending_state == PS::None) {
          // ✓ — briefly shown after confirmation
          auto layout = ctx.factory.build_text("\xE2\x9C\x93", small);
          if (layout) {
              tk::Size sz = layout->measure();
              tk::Point pos{
                  bounds.x + bounds.w - sz.w - kIndPadR,
                  bounds.y + bounds.h - sz.h - kIndPadB
              };
              ctx.canvas.draw_text(*layout, pos, ctx.theme.palette.accent);
          }
      } else if (row.pending_state == PS::Sending) {
          // ◷ clock
          auto layout = ctx.factory.build_text("\xE2\x97\xB7", small);
          if (layout) {
              tk::Size sz = layout->measure();
              tk::Point pos{
                  bounds.x + bounds.w - sz.w - kIndPadR,
                  bounds.y + bounds.h - sz.h - kIndPadB
              };
              ctx.canvas.draw_text(*layout, pos, ctx.theme.palette.text_muted);
          }
      } else if (row.pending_state == PS::Failed) {
          // ⚠ + Retry or ⚠ + ✕
          const tk::Color red = tk::Color::rgb(0xE53935);
          auto warn = ctx.factory.build_text("\xE2\x9A\xA0", small);
          float right = bounds.x + bounds.w - kIndPadR;
          float bot   = bounds.y + bounds.h - kIndPadB;
          if (row.pending_recoverable) {
              // ⚠ "Retry"
              tk::TextStyle acc{};
              acc.role = tk::FontRole::Small;
              auto retry_lbl = ctx.factory.build_text("Retry", acc);
              if (warn && retry_lbl) {
                  tk::Size wsz = warn->measure();
                  tk::Size rsz = retry_lbl->measure();
                  constexpr float kGap = 3.0f;
                  float total_w = wsz.w + kGap + rsz.w;
                  float x = right - total_w;
                  float y = bot - std::max(wsz.h, rsz.h);
                  ctx.canvas.draw_text(*warn, {x, y}, red);
                  tk::Rect retry_rect{ x + wsz.w + kGap, y, rsz.w, rsz.h };
                  ctx.canvas.draw_text(*retry_lbl, {retry_rect.x, retry_rect.y},
                                        ctx.theme.palette.accent);
                  // Store in world coords for hit-testing.
                  if (row_index == geom_.row_index)
                      geom_.retry_button = retry_rect;
              }
          } else {
              // ⚠ ✕
              auto x_lbl = ctx.factory.build_text("\xE2\x9C\x95", small);
              if (warn && x_lbl) {
                  tk::Size wsz = warn->measure();
                  tk::Size xsz = x_lbl->measure();
                  constexpr float kGap = 3.0f;
                  float total_w = wsz.w + kGap + xsz.w;
                  float x = right - total_w;
                  float y = bot - std::max(wsz.h, xsz.h);
                  ctx.canvas.draw_text(*warn, {x, y}, red);
                  tk::Rect abort_rect{ x + wsz.w + kGap, y, xsz.w, xsz.h };
                  ctx.canvas.draw_text(*x_lbl, {abort_rect.x, abort_rect.y}, red);
                  if (row_index == geom_.row_index)
                      geom_.abort_button = abort_rect;
              }
          }
      }
  }
  ```

  Note: `geom_` is the `RowChipGeom` member of `MessageListView` updated during paint. Look for how `reply_button`, `edit_button`, `delete_button` are stored in the existing `paint_row()` code to understand the `geom_` reference pattern, then use the same approach.

- [ ] **Step 2: Add hit-testing for retry/abort in `on_pointer_down()`**

  In `MessageListView::on_pointer_down()`, after the existing checks for `edit_button`, `delete_button` clicks (look for `on_delete_requested` call), add:

  ```cpp
  // Retry/abort pending send buttons.
  const auto& rgeom = hovered_row_geom_;
  if (rgeom.retry_button.w > 0 &&
      rgeom.retry_button.contains(world_point)) {
      std::size_t ri = rgeom.row_index;
      if (ri < messages_.size() && on_retry_send)
          on_retry_send(messages_[ri].pending_txn_id);
      return true;
  }
  if (rgeom.abort_button.w > 0 &&
      rgeom.abort_button.contains(world_point)) {
      std::size_t ri = rgeom.row_index;
      if (ri < messages_.size() && on_abort_send)
          on_abort_send(messages_[ri].pending_txn_id);
      return true;
  }
  ```

- [ ] **Step 3: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors. Fix any that arise from the `geom_` access pattern — look at how `reply_button` etc. are written in the existing `paint_row` adapter code and mirror it exactly.

- [ ] **Step 4: Commit**

  ```bash
  git add ui/shared/views/MessageListView.cpp
  git commit -m "feat(ui): paint pending send indicators (clock/checkmark/retry/abort)"
  ```

---

## Task 8: Write C++ tests for pending send mapping and just-sent detection

**Files:**
- Create: `tests/cpp/test_pending_send.cpp`

- [ ] **Step 1: Write the failing tests**

  Create `tests/cpp/test_pending_send.cpp`:

  ```cpp
  #include <catch2/catch_test_macros.hpp>

  #include "views/MessageListView.h"
  #include "tk_test_surface.h"

  #include <tesseract/types.h>

  using tesseract::views::MessageRowData;
  using tesseract::views::make_row_data;

  // Helper: build a minimal TextEvent with pending fields set.
  static tesseract::TextEvent make_text_event(const std::string& sender,
                                               const std::string& pending_state,
                                               bool recoverable = false,
                                               const std::string& txn_id = "") {
      tesseract::TextEvent ev;
      ev.event_id       = "!test:example.org";
      ev.sender         = sender;
      ev.sender_name    = "Test";
      ev.body           = "hello";
      ev.timestamp      = 1000;
      ev.type           = tesseract::EventType::Text;
      ev.pending_state  = pending_state;
      ev.pending_recoverable = recoverable;
      ev.pending_txn_id = txn_id;
      return ev;
  }

  TEST_CASE("make_row_data maps 'sending' pending state", "[pending]") {
      auto ev = make_text_event("@me:example.org", "sending", false, "txn1");
      auto row = make_row_data(ev, "@me:example.org");
      CHECK(row.pending_state == MessageRowData::PendingState::Sending);
      CHECK(row.pending_txn_id == "txn1");
      CHECK(row.is_own == true);
  }

  TEST_CASE("make_row_data maps 'failed' recoverable", "[pending]") {
      auto ev = make_text_event("@me:example.org", "failed", true, "txn2");
      ev.pending_error = "network error";
      auto row = make_row_data(ev, "@me:example.org");
      CHECK(row.pending_state == MessageRowData::PendingState::Failed);
      CHECK(row.pending_recoverable == true);
      CHECK(row.pending_error == "network error");
  }

  TEST_CASE("make_row_data maps 'failed' unrecoverable", "[pending]") {
      auto ev = make_text_event("@me:example.org", "failed", false, "txn3");
      auto row = make_row_data(ev, "@me:example.org");
      CHECK(row.pending_state == MessageRowData::PendingState::Failed);
      CHECK(row.pending_recoverable == false);
  }

  TEST_CASE("make_row_data: empty pending_state → PendingState::None", "[pending]") {
      auto ev = make_text_event("@me:example.org", "");
      auto row = make_row_data(ev, "@me:example.org");
      CHECK(row.pending_state == MessageRowData::PendingState::None);
  }

  TEST_CASE("update_message fires on_just_sent on Sending→None for own messages",
            "[pending]") {
      auto surface = TestSurface::create(400, 600);
      tesseract::views::MessageListView list;

      // Seed with one Sending message.
      tesseract::TextEvent ev;
      ev.event_id    = "!ev1:example.org";
      ev.sender      = "@me:example.org";
      ev.sender_name = "Me";
      ev.body        = "hello";
      ev.timestamp   = 1000;
      ev.type        = tesseract::EventType::Text;
      ev.pending_state = "sending";
      ev.pending_txn_id = "txn1";
      auto row = make_row_data(ev, "@me:example.org");
      list.set_messages({ row });

      std::string fired_id;
      list.on_just_sent = [&](const std::string& eid) { fired_id = eid; };

      // Now update with PendingState::None (server confirmed).
      ev.pending_state  = "";
      ev.pending_txn_id = "";
      ev.event_id       = "!ev1:example.org";   // same or updated event_id
      auto updated = make_row_data(ev, "@me:example.org");
      list.update_message(0, std::move(updated));

      CHECK(fired_id == "!ev1:example.org");
      CHECK(list.messages()[0].just_sent == true);
  }

  TEST_CASE("update_message does NOT fire on_just_sent for non-own messages",
            "[pending]") {
      auto surface = TestSurface::create(400, 600);
      tesseract::views::MessageListView list;

      tesseract::TextEvent ev;
      ev.event_id = "!ev2:example.org";
      ev.sender   = "@other:example.org";
      ev.sender_name = "Other";
      ev.body     = "hey";
      ev.timestamp = 1000;
      ev.type = tesseract::EventType::Text;
      ev.pending_state = "sending";
      auto row = make_row_data(ev, "@me:example.org");  // not own
      list.set_messages({ row });

      bool fired = false;
      list.on_just_sent = [&](const std::string&) { fired = true; };

      ev.pending_state = "";
      auto updated = make_row_data(ev, "@me:example.org");
      list.update_message(0, std::move(updated));

      CHECK(fired == false);
  }
  ```

- [ ] **Step 2: Register the test file in CMakeLists.txt**

  Find how other test files are registered (look for `add_executable(tesseract_tests` or `target_sources(tesseract_tests`). Add `test_pending_send.cpp` to that list.

- [ ] **Step 3: Run the tests to verify they pass**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
  ctest --test-dir build/linux-qt6-debug -R pending --output-on-failure
  ```
  Expected: all 5 pending tests pass.

- [ ] **Step 4: Commit**

  ```bash
  git add tests/cpp/test_pending_send.cpp CMakeLists.txt
  git commit -m "test(ui): add pending-send mapping and just-sent detection tests"
  ```

---

## Task 9: Wire retry/abort/just_sent in RoomWindowBase

**Files:**
- Modify: `ui/shared/app/RoomWindowBase.h`
- Modify: `ui/shared/app/RoomWindowBase.cpp`

- [ ] **Step 1: Add helper declarations to `RoomWindowBase.h`**

  In `RoomWindowBase.h`, after `void send_typing_notice_(bool typing);` (line 61), add:

  ```cpp
      void retry_send_     (const std::string& txn_id);
      void abort_send_     (const std::string& txn_id);
  ```

- [ ] **Step 2: Implement helpers in `RoomWindowBase.cpp`**

  Find the `RoomWindowBase.cpp` file. Implement:

  ```cpp
  void RoomWindowBase::retry_send_(const std::string& /*txn_id*/) {
      // The SDK retries all pending sends for the room when the queue is
      // re-enabled — txn_id is unused here but kept for symmetry.
      shell_->client_->retry_send(room_id_);
  }

  void RoomWindowBase::abort_send_(const std::string& txn_id) {
      shell_->client_->abort_send(room_id_, txn_id);
  }
  ```

  Note: `shell_->client_` is accessible because `ShellBase` declares `RoomWindowBase` as a friend. Verify this is the correct access pattern by looking at how `send_message_()` is implemented and mirroring it.

- [ ] **Step 3: Wire callbacks in `finish_init_()`**

  In `RoomWindowBase::finish_init_()` (find it in `RoomWindowBase.cpp`), after the existing callback wiring (reply, edit, delete, etc.), add:

  ```cpp
  room_view_->msg_list()->on_retry_send = [this](const std::string& txn_id) {
      retry_send_(txn_id);
  };
  room_view_->msg_list()->on_abort_send = [this](const std::string& txn_id) {
      abort_send_(txn_id);
  };
  ```

  Note: `room_view_->msg_list()` returns the `MessageListView*`. Check `RoomView.h` to confirm the method name.

- [ ] **Step 4: Build**

  ```bash
  cmake --build build/linux-qt6-debug 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add ui/shared/app/RoomWindowBase.h ui/shared/app/RoomWindowBase.cpp
  git commit -m "feat(ui): wire retry_send/abort_send in RoomWindowBase"
  ```

---

## Task 10: Add GridView hover tracking

**Files:**
- Modify: `ui/shared/tk/list_view.h`
- Modify: `ui/shared/tk/list_view.cpp`

- [ ] **Step 1: Add declarations to `GridView` in `list_view.h`**

  In `list_view.h`, inside the `GridView` class (after `void on_pointer_drag(Point local) override;` around line 93), add:

  ```cpp
      void on_pointer_move (Point local) override;
      void on_pointer_leave()            override;

      // Index of the currently hovered cell (-1 when none).
      int  hovered_index()  const { return hovered_index_; }

      // Widget-local rect of cell `idx`, or a zero-area rect when out of bounds.
      tk::Rect rect_at(int idx) const;
  ```

- [ ] **Step 2: Implement `on_pointer_move` and `on_pointer_leave` in `list_view.cpp`**

  In `list_view.cpp`, after the existing `GridView::index_at()` implementation (around line 458), add:

  ```cpp
  void GridView::on_pointer_move(Point local) {
      int idx = index_at(local);
      if (idx == hovered_index_) return;
      hovered_index_ = idx;
      invalidate_data();
  }

  void GridView::on_pointer_leave() {
      if (hovered_index_ == kInvalidIndex) return;
      hovered_index_ = kInvalidIndex;
      invalidate_data();
  }
  ```

- [ ] **Step 3: Implement `rect_at()`**

  ```cpp
  tk::Rect GridView::rect_at(int idx) const {
      if (!adapter_ || idx < 0 ||
          static_cast<std::size_t>(idx) >= adapter_->count()) return {};
      int c = cols(bounds_.w);
      if (c <= 0) return {};
      int row = idx / c;
      int col = idx % c;
      float x = bounds_.x + padding_.left + col * (cell_w_ + h_spacing_);
      float y = bounds_.y + padding_.top  + row * (cell_h_ + v_spacing_) - scroll_y_;
      return { x, y, cell_w_, cell_h_ };
  }
  ```

- [ ] **Step 4: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add ui/shared/tk/list_view.h ui/shared/tk/list_view.cpp
  git commit -m "feat(tk): GridView on_pointer_move/leave hover tracking + rect_at()"
  ```

---

## Task 11: Write GridView hover tests

**Files:**
- Modify: `tests/cpp/test_tk_lists.cpp`

- [ ] **Step 1: Write failing tests**

  In `tests/cpp/test_tk_lists.cpp`, add a minimal `GridAdapter` and tests after the existing `TEST_CASE` blocks:

  ```cpp
  namespace {

  struct FixedGridAdapter : tk::GridAdapter {
      std::size_t n = 20;
      std::size_t count() const override { return n; }
      void paint_cell(std::size_t, tk::PaintCtx& ctx, tk::Rect bounds,
                      bool, bool hovered) override {
          ctx.canvas.fill_rect(bounds,
              hovered ? tk::Color::rgb(0xFF0000) : tk::Color::rgb(0xCCCCCC));
      }
  };

  } // namespace

  TEST_CASE("GridView hover: on_pointer_move updates hovered_index", "[tk][gridview]") {
      Stage st;
      tk::GridView grid;
      FixedGridAdapter ad;
      grid.set_adapter(&ad);
      grid.set_cell_size(32, 32);
      grid.set_spacing(2, 2);

      auto lc = st.layout_ctx();
      // 400 px wide → cols = (400 + 2) / (32 + 2) = 11
      grid.arrange(lc, { 0, 0, 400, 300 });

      CHECK(grid.hovered_index() == -1);

      // Pointer over cell 0 (top-left, col=0 row=0).
      grid.on_pointer_move({ 16, 16 });  // centre of first cell
      CHECK(grid.hovered_index() == 0);

      // Move to cell 1 (col=1, row=0): x = 32+2+16 = 50.
      grid.on_pointer_move({ 50, 16 });
      CHECK(grid.hovered_index() == 1);
  }

  TEST_CASE("GridView hover: on_pointer_leave clears hovered_index", "[tk][gridview]") {
      Stage st;
      tk::GridView grid;
      FixedGridAdapter ad;
      grid.set_adapter(&ad);
      grid.set_cell_size(32, 32);
      grid.set_spacing(2, 2);

      auto lc = st.layout_ctx();
      grid.arrange(lc, { 0, 0, 400, 300 });

      grid.on_pointer_move({ 16, 16 });
      REQUIRE(grid.hovered_index() == 0);

      grid.on_pointer_leave();
      CHECK(grid.hovered_index() == -1);
  }

  TEST_CASE("GridView hover: pointer in inter-cell gap → -1", "[tk][gridview]") {
      Stage st;
      tk::GridView grid;
      FixedGridAdapter ad;
      grid.set_adapter(&ad);
      grid.set_cell_size(32, 32);
      grid.set_spacing(2, 2);

      auto lc = st.layout_ctx();
      grid.arrange(lc, { 0, 0, 400, 300 });

      // x=33 is in the 2-px gap between cells 0 and 1 (cell 0 is 0..31, gap 32..33).
      grid.on_pointer_move({ 33, 16 });
      CHECK(grid.hovered_index() == -1);
  }

  TEST_CASE("GridView rect_at returns correct cell rect", "[tk][gridview]") {
      Stage st;
      tk::GridView grid;
      FixedGridAdapter ad;
      ad.n = 4;
      grid.set_adapter(&ad);
      grid.set_cell_size(32, 32);
      grid.set_spacing(2, 2);

      auto lc = st.layout_ctx();
      // arrange at origin: cell 0 = {0,0,32,32}, cell 1 = {34,0,32,32}
      grid.arrange(lc, { 0, 0, 400, 300 });

      auto r0 = grid.rect_at(0);
      CHECK(r0.x == 0.0f);
      CHECK(r0.y == 0.0f);
      CHECK(r0.w == 32.0f);
      CHECK(r0.h == 32.0f);

      auto r1 = grid.rect_at(1);
      CHECK(r1.x == 34.0f);  // 0 + 32 + 2
  }
  ```

- [ ] **Step 2: Build and run**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
  ctest --test-dir build/linux-qt6-debug -R gridview --output-on-failure
  ```
  Expected: all 4 GridView hover tests pass.

- [ ] **Step 3: Commit**

  ```bash
  git add tests/cpp/test_tk_lists.cpp
  git commit -m "test(tk): GridView hover and rect_at tests"
  ```

---

## Task 12: EmojiPicker — shortcode tracking and inline tooltip

**Files:**
- Modify: `ui/shared/views/EmojiPicker.h`
- Modify: `ui/shared/views/EmojiPicker.cpp`

- [ ] **Step 1: Add hover state and shortcode list to `EmojiPicker.h`**

  In `EmojiPicker.h`, in the `private:` section after `std::vector<tesseract::ImagePackImage> current_emoticons_;`, add:

  ```cpp
      std::vector<std::string>             current_shortcodes_; // parallel to current_glyphs_
      int                                  hovered_grid_cell_ = -1;
  ```

  In the public Widget overrides block (after `bool on_wheel(...) override;`), add:

  ```cpp
      void on_pointer_move (tk::Point local) override;
      void on_pointer_leave()                override;
  ```

- [ ] **Step 2: Populate `current_shortcodes_` in `rebuild_current_items()`**

  In `EmojiPicker.cpp`, in `rebuild_current_items()` (line 250):

  After `current_glyphs_.clear();`, also add `current_shortcodes_.clear();`

  In each branch that populates `current_glyphs_`:

  **Frequents branch** — after `current_glyphs_ = frequents_glyphs_;`, add:
  ```cpp
  // Look up shortcodes from the static emoji table.
  current_shortcodes_.reserve(current_glyphs_.size());
  for (const auto& glyph : current_glyphs_) {
      std::string sc;
      for (const auto& e : tesseract::emoji::all()) {
          if (e.glyph == glyph && !e.shortcodes.empty()) {
              auto end = e.shortcodes.find(' ');
              sc = ":" + std::string(e.shortcodes.substr(0, end)) + ":";
              break;
          }
      }
      current_shortcodes_.push_back(std::move(sc));
  }
  ```

  **Category branch** — change the loop from:
  ```cpp
  for (const auto* e : entries) current_glyphs_.emplace_back(e->glyph);
  ```
  to:
  ```cpp
  current_shortcodes_.reserve(entries.size());
  for (const auto* e : entries) {
      current_glyphs_.emplace_back(e->glyph);
      auto end = e->shortcodes.find(' ');
      current_shortcodes_.emplace_back(
          ":" + std::string(e->shortcodes.substr(0, end)) + ":");
  }
  ```

  **CustomPack branch** — after pushing to `current_emoticons_`, add a parallel shortcode:
  ```cpp
  // Emoticons use their shortcode field directly.
  current_shortcodes_.emplace_back(":" + img.shortcode + ":");
  ```
  (Push before `current_emoticons_.push_back(std::move(img));` to keep indices aligned.)

  **Search branch** — same as Category branch.

  Also reset `hovered_grid_cell_ = -1;` at the top of `rebuild_current_items()`:
  ```cpp
  hovered_grid_cell_ = -1;
  ```

- [ ] **Step 3: Implement `on_pointer_move()` and `on_pointer_leave()`**

  In `EmojiPicker.cpp`, add after `rebuild_current_items()`:

  ```cpp
  void EmojiPicker::on_pointer_move(tk::Point local) {
      int cell = -1;
      if (grid_ && grid_rect_.contains(local)) {
          // Convert widget-local to grid-local.
          tk::Point grid_local{ local.x - grid_rect_.x, local.y - grid_rect_.y };
          cell = grid_->index_at(grid_local);
      }
      if (cell == hovered_grid_cell_) return;
      hovered_grid_cell_ = cell;
      invalidate();
  }

  void EmojiPicker::on_pointer_leave() {
      if (hovered_grid_cell_ == -1) return;
      hovered_grid_cell_ = -1;
      invalidate();
  }
  ```

  Note: `invalidate()` is the `Widget` base method that requests a repaint. Verify the method name by checking `widget.h`; if it's named differently (e.g., `request_repaint()` or `mark_dirty()`) use that instead.

- [ ] **Step 4: Draw inline tooltip in `EmojiPicker::paint()`**

  In `EmojiPicker.cpp`, in `paint()` (line 328), after `if (grid_) grid_->paint(ctx);` (line 340), add:

  ```cpp
  // ── Shortcode tooltip ──────────────────────────────────────────────
  if (hovered_grid_cell_ >= 0 &&
      static_cast<std::size_t>(hovered_grid_cell_) < current_shortcodes_.size()) {
      const std::string& sc = current_shortcodes_[hovered_grid_cell_];
      if (!sc.empty()) {
          tk::TextStyle small{};
          small.role = tk::FontRole::Small;
          auto layout = ctx.factory.build_text(sc, small);
          if (layout) {
              tk::Size tsz = layout->measure();
              constexpr float kPad = 4.0f;
              constexpr float kRadius = 4.0f;
              // Cell rect (widget-local).
              tk::Rect cell_r = grid_->rect_at(hovered_grid_cell_);
              // Position tooltip centred above cell; flip below if too close to top.
              float tx = cell_r.x + (cell_r.w - tsz.w) / 2.0f - kPad;
              float ty = cell_r.y - tsz.h - kPad * 2 - 2.0f;
              if (ty < bounds_.y) ty = cell_r.y + cell_r.h + 2.0f;
              // Clamp horizontally to picker bounds.
              tx = std::max(bounds_.x + kPad, std::min(tx, bounds_.x + bounds_.w - tsz.w - kPad * 2 - kPad));
              tk::Rect bg{ tx, ty, tsz.w + kPad * 2, tsz.h + kPad * 2 };
              ctx.canvas.push_clip_rect(bounds_);
              ctx.canvas.fill_rounded_rect(bg, kRadius, ctx.theme.palette.chrome_bg);
              ctx.canvas.stroke_rounded_rect(bg, kRadius, ctx.theme.palette.popup_border, 1.0f);
              ctx.canvas.draw_text(*layout, { bg.x + kPad, bg.y + kPad },
                                    ctx.theme.palette.text_primary);
              ctx.canvas.pop_clip_rect();
          }
      }
  }
  ```

  Note: `grid_->rect_at(idx)` returns a rect in grid-widget-local coords. Since `grid_` is arranged at `grid_rect_`, the rect is already positioned relative to the picker's `bounds_` origin. Verify this by checking how `arrange()` sets `grid_rect_` and how `GridView::rect_at()` uses `bounds_`.

- [ ] **Step 5: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 6: Commit**

  ```cpp
  git add ui/shared/views/EmojiPicker.h ui/shared/views/EmojiPicker.cpp
  git commit -m "feat(ui): EmojiPicker shortcode tooltip on hover"
  ```

---

## Task 13: StickerPicker — shortcode tracking and inline tooltip

**Files:**
- Modify: `ui/shared/views/StickerPicker.h`
- Modify: `ui/shared/views/StickerPicker.cpp`

Follow the same pattern as Task 12 for EmojiPicker. Differences:
- Shortcode source: `current_items_[idx].shortcode` (wrapped in `:...:`)
- No `emoji::all()` lookup needed — pack items carry their shortcode directly.

- [ ] **Step 1: Add hover state to `StickerPicker.h`**

  In `StickerPicker.h`, in `private:`, after `int hovered_tab_idx_ = -1;`, add:

  ```cpp
      int hovered_grid_cell_ = -1;
  ```

  In the public Widget overrides block (after `bool on_wheel(...) override;`), add:

  ```cpp
      void on_pointer_move (tk::Point local) override;
      void on_pointer_leave()                override;
  ```

- [ ] **Step 2: Implement `on_pointer_move()` and `on_pointer_leave()`**

  In `StickerPicker.cpp`, add after `rebuild_current_items()`:

  ```cpp
  void StickerPicker::on_pointer_move(tk::Point local) {
      int cell = -1;
      if (grid_ && grid_rect_.contains(local)) {
          tk::Point grid_local{ local.x - grid_rect_.x, local.y - grid_rect_.y };
          cell = grid_->index_at(grid_local);
      }
      if (cell == hovered_grid_cell_) return;
      hovered_grid_cell_ = cell;
      invalidate();
  }

  void StickerPicker::on_pointer_leave() {
      if (hovered_grid_cell_ == -1) return;
      hovered_grid_cell_ = -1;
      invalidate();
  }
  ```

  Also reset `hovered_grid_cell_ = -1;` at the start of `rebuild_current_items()`.

- [ ] **Step 3: Draw inline tooltip in `StickerPicker::paint()`**

  In `StickerPicker.cpp`, in `paint()`, after `if (grid_) grid_->paint(ctx);`, add the same tooltip block as EmojiPicker, but source the shortcode from `current_items_`:

  ```cpp
  // ── Shortcode tooltip ──────────────────────────────────────────────
  if (hovered_grid_cell_ >= 0 &&
      static_cast<std::size_t>(hovered_grid_cell_) < current_items_.size()) {
      const std::string& raw_sc = current_items_[hovered_grid_cell_].shortcode;
      if (!raw_sc.empty()) {
          const std::string sc = ":" + raw_sc + ":";
          tk::TextStyle small{};
          small.role = tk::FontRole::Small;
          auto layout = ctx.factory.build_text(sc, small);
          if (layout) {
              tk::Size tsz = layout->measure();
              constexpr float kPad = 4.0f;
              constexpr float kRadius = 4.0f;
              tk::Rect cell_r = grid_->rect_at(hovered_grid_cell_);
              float tx = cell_r.x + (cell_r.w - tsz.w) / 2.0f - kPad;
              float ty = cell_r.y - tsz.h - kPad * 2 - 2.0f;
              if (ty < bounds_.y) ty = cell_r.y + cell_r.h + 2.0f;
              tx = std::max(bounds_.x + kPad,
                            std::min(tx, bounds_.x + bounds_.w - tsz.w - kPad * 2 - kPad));
              tk::Rect bg{ tx, ty, tsz.w + kPad * 2, tsz.h + kPad * 2 };
              ctx.canvas.push_clip_rect(bounds_);
              ctx.canvas.fill_rounded_rect(bg, kRadius, ctx.theme.palette.chrome_bg);
              ctx.canvas.stroke_rounded_rect(bg, kRadius, ctx.theme.palette.popup_border, 1.0f);
              ctx.canvas.draw_text(*layout, { bg.x + kPad, bg.y + kPad },
                                    ctx.theme.palette.text_primary);
              ctx.canvas.pop_clip_rect();
          }
      }
  }
  ```

- [ ] **Step 4: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add ui/shared/views/StickerPicker.h ui/shared/views/StickerPicker.cpp
  git commit -m "feat(ui): StickerPicker shortcode tooltip on hover"
  ```

---

## Task 14: Final build, full test suite, and verification

- [ ] **Step 1: Full C++ build**

  ```bash
  cmake --build build/linux-qt6-debug 2>&1 | tail -10
  ```
  Expected: build succeeds.

- [ ] **Step 2: Run Rust tests**

  ```bash
  cargo test -p tesseract-sdk-ffi 2>&1 | tail -10
  ```
  Expected: all tests pass.

- [ ] **Step 3: Run C++ tests**

  ```bash
  ctest --test-dir build/linux-qt6-debug --output-on-failure
  ```
  Expected: all tests pass including `test_pending_send` and `test_tk_lists` GridView hover cases.

- [ ] **Step 4: Manual verification checklist**

  Run the app: `./build/linux-qt6-debug/ui/linux-qt/tesseract`

  1. **Optimistic send (happy path):** Send a message → it appears immediately with ◷ indicator → ◷ disappears and ✓ appears → ✓ fades after ~2 s.
  2. **Optimistic send (recoverable failure):** Disconnect network, send → ◷ appears → transitions to ⚠ Retry → reconnect, click Retry → message sends normally.
  3. **Optimistic send (unrecoverable):** Simulate an unrecoverable error → ⚠ ✕ appears → click ✕ → message row disappears.
  4. **Dark theme:** Switch OS to dark mode → open emoji picker → all surfaces use palette colors (bg, chrome_bg, text_primary etc. — no light-mode hardcoded values).
  5. **GridView hover:** Hover over emoji cells → each cell highlights (was broken before; `hovered_index_` now tracks the pointer).
  6. **Emoji shortcode tooltip:** Hover over any emoji → `:shortcode:` label appears centered above the cell; moving away clears it immediately.
  7. **Sticker shortcode tooltip:** Same as emoji tooltip, using `shortcode` from `ImagePackImage`.

- [ ] **Step 5: Commit plan document to repo**

  ```bash
  cp /home/rayden/.claude/plans/sent-messages-should-show-glimmering-glade.md \
     docs/superpowers/plans/2026-05-17-optimistic-send-picker-polish.md
  git add docs/superpowers/plans/2026-05-17-optimistic-send-picker-polish.md
  git commit -m "docs: add optimistic-send + picker-polish implementation plan"
  ```

---

## Task 15: Full code review pass

After all implementation tasks are committed, do a complete review of every changed file before declaring the work done.

- [ ] **Step 1: Review Rust changes**

  Read `sdk/src/bridge.rs` and `sdk/src/client.rs` in full at the changed sections. Verify:
  - All four `pending_*` fields are present in **every** `TimelineEvent { ... }` struct literal (virtual, redacted, sticker, and the final message path). A missing field will be a compile error, but double-check none were accidentally omitted.
  - `send_message()` tries `self.timelines.get(&room_id)` before falling back to `room.send()`. The borrow of `self.timelines` is released before `self.rt.block_on()` is called (via `clone()` of the Arc).
  - `retry_send()` calls `room.send_queue().set_enabled(true)` — no `await` needed (it's synchronous).
  - `abort_send()` uses `TimelineEventItemId::TransactionId(txn_id)` and calls `timeline.redact(...)`.
  - The `EventSendState::NotSentYet` match arm does not accidentally match `Sent { event_id }` items.

- [ ] **Step 2: Review C++ type changes**

  Read `client/include/tesseract/types.h` and `client/src/ffi_convert.h`. Verify:
  - All four `pending_*` fields exist on `Event` (base struct, line ~64).
  - `assign_base()` copies all four fields. No field typo (e.g., `pending_recoverable` vs `pendingRecoverable`).
  - No derived `Event` subclass overrides or hides any pending field (they shouldn't — the fields are on the base).

- [ ] **Step 3: Review `MessageRowData` and `MessageListView` changes**

  Read `ui/shared/views/MessageListView.h` and `.cpp`. Verify:
  - `PendingState` enum is inside `MessageRowData` (not a free enum), and its default is `None`.
  - `just_sent` is never persisted across `set_messages()` (it's a transient in-memory flag, not mapped from `Event`).
  - `update_message()`: the just-sent detection checks `is_own` before firing `on_just_sent` — a message from someone else should never trigger it.
  - `clear_just_sent()` iterates `messages_` by reference, sets `just_sent = false`, and calls `invalidate_data()` exactly once (returns after first match — no double-invalidate).
  - `post_delayed_` is guarded (`if (post_delayed_)`) before use — the timer is optional (tests and shells without a timer don't crash).
  - `RowChipGeom::retry_button` and `abort_button` are reset to zero-area in every paint pass for the hovered row before the new geometry is computed. If they are not explicitly zeroed at the start of each paint pass, stale rects from a previous state will ghost.
  - `on_pointer_down()` checks `retry_button.w > 0` (or similar non-zero-area guard) before treating it as a hit target.

- [ ] **Step 4: Review GridView hover**

  Read `ui/shared/tk/list_view.h` and `.cpp` at the new methods. Verify:
  - `on_pointer_move` calls `invalidate_data()` only when `hovered_index_` actually changes (the early return on `idx == hovered_index_` prevents unnecessary repaints).
  - `on_pointer_leave` similarly guards on `hovered_index_ == kInvalidIndex`.
  - `rect_at()` uses `bounds_` (world coords) for `x`/`y`, correctly accounting for `scroll_y_` and `padding_`. Double-check: if `bounds_` is in world coords and `index_at()` takes widget-local coords, the tooltip position math in EmojiPicker/StickerPicker needs to decide whether `rect_at()` returns world or local coords — and use it consistently.

- [ ] **Step 5: Review EmojiPicker and StickerPicker tooltip logic**

  Read `EmojiPicker.cpp` and `StickerPicker.cpp`. Verify:
  - `current_shortcodes_` is always the same length as `current_glyphs_` (for unicode pages) or `current_emoticons_` (for custom packs). A length mismatch between the cell index and the shortcode vector would cause an out-of-bounds access. Check that `current_shortcodes_.clear()` is called at the top of `rebuild_current_items()`, alongside `current_glyphs_.clear()`.
  - For the **CustomPack** branch in EmojiPicker: the shortcode is pushed to `current_shortcodes_` *before* `current_emoticons_.push_back(std::move(img))` so indices remain aligned. The index used to look up the tooltip is `hovered_grid_cell_` into `current_shortcodes_` — it must match the cell index the GridAdapter uses for `current_emoticons_`.
  - `on_pointer_move` converts from picker-widget-local coords to grid-widget-local coords correctly: `grid_local = { local.x - grid_rect_.x, local.y - grid_rect_.y }`. Verify `grid_rect_` is in picker-widget-local coords (it is, since `arrange()` uses `bounds_.x + offset`).
  - The tooltip's `push_clip_rect(bounds_)` / `pop_clip_rect()` are balanced.
  - `invalidate()` is the correct Widget method to request a repaint (check `widget.h` for the actual method name).

- [ ] **Step 6: Fix any issues found and re-run tests**

  ```bash
  cmake --build build/linux-qt6-debug 2>&1 | grep "error:" | head -20
  ctest --test-dir build/linux-qt6-debug --output-on-failure
  cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
  ```
  Expected: all pass. Fix any issues found during review, then commit the fixes.

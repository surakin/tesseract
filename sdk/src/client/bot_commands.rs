//! MSC4391 in-room bot command discovery FFI surface.
//!
//! Split out of `client/mod.rs`'s general FFI impl block, same convention as
//! `client/image_packs.rs`. The state-event fetch/parse/merge/join-filter
//! itself lives in `super::fetch_room_bot_commands` (mod.rs, alongside
//! `fetch_all_room_pack_contents`, which it mirrors) and in the pure
//! `crate::bot_commands` module; this file only owns the cache read and the
//! FFI struct conversion. Invocation (`send_bot_command`) lives in
//! `send.rs` alongside the other message-sending methods, not here.

use super::ClientFfi;

impl ClientFfi {
    /// Snapshot of every MSC4391 bot command description currently cached
    /// for `room_id` — valid and invalid entries alike (see
    /// `CommandDescriptionFfi::valid`'s doc comment in bridge.rs) from
    /// senders currently joined to the room. Reads the in-memory cache
    /// only — no network roundtrip. Populated by `set_active_room`'s
    /// newly-active-room fetch; empty for a room not yet visited this
    /// session (same "fetch on first visit" tradeoff as the image-pack
    /// cache — see `bot_commands` field's doc comment in mod.rs).
    #[cfg(not(test))]
    pub fn list_room_bot_commands(&self, room_id: &str) -> Vec<crate::ffi::CommandDescriptionFfi> {
        let Ok(rid) = room_id.parse::<matrix_sdk::ruma::OwnedRoomId>() else {
            return Vec::new();
        };
        let cache = self.bot_commands.lock();
        let Some(descriptions) = cache.get(&rid) else {
            return Vec::new();
        };
        descriptions.iter().map(description_to_ffi).collect()
    }

    #[cfg(test)]
    pub fn list_room_bot_commands(&self, _room_id: &str) -> Vec<crate::ffi::CommandDescriptionFfi> {
        Vec::new()
    }
}

#[cfg(not(test))]
fn description_to_ffi(
    d: &crate::bot_commands::CommandDescription,
) -> crate::ffi::CommandDescriptionFfi {
    crate::ffi::CommandDescriptionFfi {
        command: d.command.clone(),
        sender: d.sender.clone(),
        sender_display_name: d.sender_display_name.clone(),
        state_key: d.state_key.clone(),
        room_id: d.room_id.clone(),
        parameters: d.parameters.iter().map(parameter_to_ffi).collect(),
        description_text: d.description.clone(),
        valid: d.valid,
    }
}

#[cfg(not(test))]
fn parameter_to_ffi(p: &crate::bot_commands::CommandParameter) -> crate::ffi::CommandParameterFfi {
    crate::ffi::CommandParameterFfi {
        key: p.key.clone(),
        schema_json: serde_json::to_string(&p.schema).unwrap_or_else(|_| "null".to_owned()),
        description_text: p.description.plain_text().to_owned(),
        optional: p.optional,
    }
}

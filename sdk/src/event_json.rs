//! Helpers over raw event JSON, for code that works on an event as fetched
//! (the full envelope with `type`, `content`, `sender`, `unsigned`).

use matrix_sdk::deserialized_responses::EncryptionInfo;
use serde_json::Value;

/// Removes the server-bundled edit (`unsigned.m.relations.m.replace`) from
/// `envelope` unless it is a valid replacement of the envelope's event.
///
/// The bundled edit is not covered by end-to-end encryption, so the homeserver
/// can put anything there. It is kept only if it passes the spec's
/// replacement-validity rules, the same ones the timeline applies; in
/// particular a plaintext edit of an encrypted event is dropped. Other
/// relations such as `m.thread` stay in place.
///
/// `original_encryption` is the decryption info of the event itself and
/// `edit_encryption` that of the bundled edit (present only if the SDK
/// decrypted an `m.room.encrypted` edit).
pub(crate) fn drop_invalid_bundled_edit(
    envelope: &mut Value,
    original_encryption: Option<&EncryptionInfo>,
    edit_encryption: Option<&EncryptionInfo>,
) {
    let Some(edit) = envelope
        .get("unsigned")
        .and_then(|u| u.get("m.relations"))
        .and_then(|r| r.get("m.replace"))
    else {
        return;
    };
    let to_raw =
        |v: &Value| serde_json::value::to_raw_value(v).map(matrix_sdk::ruma::serde::Raw::from_json);
    let valid = match (to_raw(&*envelope), to_raw(edit)) {
        (Ok(original), Ok(edit)) => matrix_sdk::check_validity_of_replacement_events(
            &original,
            original_encryption,
            &edit,
            edit_encryption,
        )
        .is_ok(),
        _ => false,
    };
    if valid {
        return;
    }
    if let Some(relations) = envelope
        .get_mut("unsigned")
        .and_then(|u| u.get_mut("m.relations"))
        .and_then(Value::as_object_mut)
    {
        relations.remove("m.replace");
    }
}

/// The content of `envelope` as the timeline would display it: the newest
/// edit's `m.new_content` when there is one, else the event's own content.
///
/// The server bundles the edit under `unsigned.m.relations.m.replace`; it is
/// used only when it names the original's own sender, since a replacement by
/// anyone else is not an edit and showing it would put someone else's words
/// in the original's mouth. `m.relates_to` and `m.mentions` are left in
/// place: the caller decides what to drop. For an event fetched from the
/// server, callers must first run [`drop_invalid_bundled_edit`], because this
/// function trusts the bundled edit's JSON.
pub(crate) fn latest_content(envelope: &Value) -> Value {
    let own = &envelope["content"];
    let replace = &envelope["unsigned"]["m.relations"]["m.replace"];
    let same_sender = matches!(
        (replace["sender"].as_str(), envelope["sender"].as_str()),
        (Some(a), Some(b)) if a == b
    );
    let bundled_edit = if same_sender {
        &replace["content"]["m.new_content"]
    } else {
        &Value::Null
    };
    let own_is_edit = own["m.relates_to"]["rel_type"].as_str() == Some("m.replace");
    if bundled_edit.is_object() {
        bundled_edit.clone()
    } else if own_is_edit && own["m.new_content"].is_object() {
        own["m.new_content"].clone()
    } else {
        own.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::deserialized_responses::{
        AlgorithmInfo, DeviceLinkProblem, VerificationLevel, VerificationState,
    };
    use serde_json::json;

    fn megolm_info(sender: &str) -> EncryptionInfo {
        EncryptionInfo {
            sender: sender.try_into().unwrap(),
            sender_device: None,
            forwarder: None,
            algorithm_info: AlgorithmInfo::MegolmV1AesSha2 {
                curve25519_key: "K".into(),
                sender_claimed_keys: Default::default(),
                session_id: None,
            },
            verification_state: VerificationState::Unverified(VerificationLevel::None(
                DeviceLinkProblem::MissingDevice,
            )),
        }
    }

    fn edited(edit: Value) -> Value {
        json!({
            "type": "m.room.message",
            "event_id": "$o",
            "sender": "@a:x",
            "content": { "msgtype": "m.text", "body": "old" },
            "unsigned": { "m.relations": { "m.replace": edit, "m.thread": { "count": 1 } } },
        })
    }

    fn valid_edit() -> Value {
        json!({
            "type": "m.room.message",
            "event_id": "$e",
            "sender": "@a:x",
            "content": {
                "msgtype": "m.text",
                "body": "* new",
                "m.new_content": { "msgtype": "m.text", "body": "new" },
                "m.relates_to": { "rel_type": "m.replace", "event_id": "$o" },
            },
        })
    }

    fn has_edit(envelope: &Value) -> bool {
        envelope["unsigned"]["m.relations"]
            .get("m.replace")
            .is_some()
    }

    #[test]
    fn a_valid_bundled_edit_in_an_unencrypted_room_is_kept() {
        let mut ev = edited(valid_edit());
        drop_invalid_bundled_edit(&mut ev, None, None);
        assert!(has_edit(&ev));
        assert_eq!(latest_content(&ev)["body"], "new");
    }

    #[test]
    fn a_plaintext_edit_of_an_encrypted_event_is_dropped() {
        let mut ev = edited(valid_edit());
        let info = megolm_info("@a:x");
        drop_invalid_bundled_edit(&mut ev, Some(&info), None);
        assert!(!has_edit(&ev));
        assert_eq!(ev["unsigned"]["m.relations"]["m.thread"]["count"], 1);
        assert_eq!(latest_content(&ev)["body"], "old");
    }

    #[test]
    fn a_decrypted_edit_of_an_encrypted_event_is_kept() {
        let mut ev = edited(valid_edit());
        let info = megolm_info("@a:x");
        drop_invalid_bundled_edit(&mut ev, Some(&info), Some(&info));
        assert!(has_edit(&ev));
        assert_eq!(latest_content(&ev)["body"], "new");
    }

    #[test]
    fn an_edit_of_a_different_event_is_dropped() {
        let mut edit = valid_edit();
        edit["content"]["m.relates_to"]["event_id"] = json!("$other");
        let mut ev = edited(edit);
        drop_invalid_bundled_edit(&mut ev, None, None);
        assert!(!has_edit(&ev));
        assert_eq!(latest_content(&ev)["body"], "old");
    }

    #[test]
    fn an_edit_by_someone_else_is_dropped() {
        let mut edit = valid_edit();
        edit["sender"] = json!("@evil:x");
        let mut ev = edited(edit);
        drop_invalid_bundled_edit(&mut ev, None, None);
        assert!(!has_edit(&ev));
    }

    #[test]
    fn an_edit_changing_the_event_type_is_dropped() {
        let mut edit = valid_edit();
        edit["type"] = json!("m.sticker");
        let mut ev = edited(edit);
        drop_invalid_bundled_edit(&mut ev, None, None);
        assert!(!has_edit(&ev));
    }

    #[test]
    fn a_malformed_bundled_edit_is_dropped() {
        let mut no_id = valid_edit();
        no_id.as_object_mut().unwrap().remove("event_id");
        let mut ev = edited(no_id);
        drop_invalid_bundled_edit(&mut ev, None, None);
        assert!(!has_edit(&ev));

        let mut no_content = valid_edit();
        no_content.as_object_mut().unwrap().remove("content");
        let mut ev = edited(no_content);
        drop_invalid_bundled_edit(&mut ev, None, None);
        assert!(!has_edit(&ev));
    }

    #[test]
    fn an_event_without_a_bundled_edit_is_unchanged() {
        let plain = json!({
            "type": "m.room.message",
            "event_id": "$o",
            "sender": "@a:x",
            "content": { "msgtype": "m.text", "body": "old" },
        });
        let mut ev = plain.clone();
        drop_invalid_bundled_edit(&mut ev, None, None);
        assert_eq!(ev, plain);

        let mut with_unsigned = plain.clone();
        with_unsigned["unsigned"] = json!({ "age": 5 });
        let before = with_unsigned.clone();
        drop_invalid_bundled_edit(&mut with_unsigned, None, None);
        assert_eq!(with_unsigned, before);
    }
}

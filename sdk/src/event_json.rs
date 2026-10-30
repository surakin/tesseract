//! Helpers over raw event JSON, for code that works on an event as fetched
//! (the full envelope with `type`, `content`, `sender`, `unsigned`).

use serde_json::Value;

/// The content of `envelope` as the timeline would display it: the newest
/// edit's `m.new_content` when there is one, else the event's own content.
///
/// The server bundles the edit under `unsigned.m.relations.m.replace`; it is
/// used only when it names the original's own sender, since a replacement by
/// anyone else is not an edit and showing it would put someone else's words
/// in the original's mouth. `m.relates_to` and `m.mentions` are left in
/// place: the caller decides what to drop.
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

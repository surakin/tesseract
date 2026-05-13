//! MSC4356 recently-used emoji.
//!
//! Wire format (per [MSC4356]):
//!
//! ```json
//! {
//!     "recent_emoji": [
//!         { "emoji": "😀", "total": 5 },
//!         { "emoji": "🎉", "total": 2 }
//!     ]
//! }
//! ```
//!
//! Array is ordered most-recent first; clients SHOULD cap to 100 entries.
//! Tesseract dual-writes the same content under both the stable
//! [`TYPE_STABLE`] and the unstable [`TYPE_UNSTABLE`] account-data types and
//! reads through the same precedence with a final fallback to Element's
//! historical [`TYPE_LEGACY`] blob (so existing users don't lose their picker
//! rank on the first MSC4356 run).
//!
//! [MSC4356]: https://github.com/matrix-org/matrix-spec-proposals/pull/4356
//!
//! This module is self-contained: it only depends on `serde_json::Value`, so
//! parse/bump/serialize can be unit-tested without a matrix-sdk client.

use serde_json::{json, Map, Value};

pub const TYPE_STABLE:   &str = "m.recent_emoji";
pub const TYPE_UNSTABLE: &str = "io.github.johennes.msc4356.recent_emoji";
pub const TYPE_LEGACY:   &str = "io.element.recent_emoji";

/// MSC4356 RECOMMENDS clients trim to 100 entries. The picker only ever
/// reads the top-N (N << 100) so the cap exists purely to keep the
/// account-data blob bounded for cross-client compatibility.
pub const MAX_ENTRIES: usize = 100;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub emoji: String,
    pub total: u64,
}

/// Parse the MSC4356 content object — i.e. the value of
/// `account_data["content"]`, not the full event envelope. Returns an empty
/// vec for any structural error (null root, missing `recent_emoji`, etc.) so
/// a malformed blob never stalls the picker. Entries missing `emoji` or
/// `total` are dropped. Duplicates by `emoji` are deduped, keeping the
/// first occurrence (which the wire format guarantees is the most recent).
pub fn parse_msc4356(raw: &Value) -> Vec<Entry> {
    let arr = match raw.get("recent_emoji").and_then(Value::as_array) {
        Some(a) => a,
        None => return Vec::new(),
    };
    let mut out: Vec<Entry> = Vec::with_capacity(arr.len());
    let mut seen = std::collections::HashSet::<String>::new();
    for item in arr {
        let Some(obj) = item.as_object() else { continue };
        let Some(emoji) = obj.get("emoji").and_then(Value::as_str) else { continue };
        if emoji.is_empty() { continue }
        let total = obj.get("total").and_then(Value::as_u64).unwrap_or(0);
        if !seen.insert(emoji.to_owned()) { continue }
        out.push(Entry { emoji: emoji.to_owned(), total });
    }
    out
}

/// Parse Element's legacy `io.element.recent_emoji` blob. Element has
/// shipped this in two shapes over time:
///
/// 1. Documented nested-array: `{ "recent_emoji": [["😀", 5], ["🎉", 3], ...] }`
/// 2. Object map (used by some forks): `{ "recent_emoji": { "😀": 5, "🎉": 3 } }`
///
/// Both yield an `Entry` list. For the array form we preserve input order
/// (Element stores most-recent first); for the object form we count-sort
/// desc (no ordering signal). Returns empty for any structural error.
pub fn parse_legacy_element(raw: &Value) -> Vec<Entry> {
    let inner = match raw.get("recent_emoji") {
        Some(v) => v,
        None => return Vec::new(),
    };

    // Nested-array form.
    if let Some(arr) = inner.as_array() {
        let mut out: Vec<Entry> = Vec::with_capacity(arr.len());
        let mut seen = std::collections::HashSet::<String>::new();
        for item in arr {
            let Some(pair) = item.as_array() else { continue };
            if pair.len() < 2 { continue }
            let Some(emoji) = pair[0].as_str() else { continue };
            if emoji.is_empty() { continue }
            let total = pair[1].as_u64().unwrap_or(0);
            if !seen.insert(emoji.to_owned()) { continue }
            out.push(Entry { emoji: emoji.to_owned(), total });
        }
        return out;
    }

    // Object-map form.
    if let Some(obj) = inner.as_object() {
        let mut out: Vec<Entry> = obj.iter()
            .filter_map(|(k, v)| {
                if k.is_empty() { return None }
                Some(Entry { emoji: k.clone(), total: v.as_u64().unwrap_or(0) })
            })
            .collect();
        // Object iteration order is unspecified; sort by count desc for a
        // sensible "most used first" approximation.
        out.sort_by(|a, b| b.total.cmp(&a.total));
        return out;
    }

    Vec::new()
}

/// Apply one MSC4356 bump: move `glyph` to the front and increment `total`,
/// or insert a fresh `{ emoji: glyph, total: 1 }` at the front. Truncates
/// to [`MAX_ENTRIES`] from the tail. No-op when `glyph` is empty.
pub fn bump(mut entries: Vec<Entry>, glyph: &str) -> Vec<Entry> {
    if glyph.is_empty() { return entries; }

    let existing = entries.iter().position(|e| e.emoji == glyph);
    match existing {
        Some(idx) => {
            let mut e = entries.remove(idx);
            e.total = e.total.saturating_add(1);
            entries.insert(0, e);
        }
        None => {
            entries.insert(0, Entry { emoji: glyph.to_owned(), total: 1 });
        }
    }
    if entries.len() > MAX_ENTRIES {
        entries.truncate(MAX_ENTRIES);
    }
    entries
}

/// Stable-sort by `total` descending and return the first `n` glyphs. The
/// picker's "Frequently used" tab uses this — MSC4356 leaves ranking open
/// and most-used-first matches the existing UX. Ties preserve input order
/// (so MSC4356's most-recent-first ordering breaks ties naturally).
pub fn top_by_count(entries: &[Entry], n: usize) -> Vec<String> {
    if n == 0 || entries.is_empty() { return Vec::new(); }
    let mut indexed: Vec<(usize, &Entry)> = entries.iter().enumerate().collect();
    // Stable sort: by total desc, tie-break by original index asc.
    indexed.sort_by(|a, b| b.1.total.cmp(&a.1.total).then(a.0.cmp(&b.0)));
    indexed.into_iter()
        .take(n)
        .map(|(_, e)| e.emoji.clone())
        .collect()
}

/// Produce the MSC4356 content object (suitable for `Raw::new` followed by
/// `set_account_data_raw`). The order of `entries` is preserved on the wire
/// — callers are responsible for handing in most-recent-first order.
pub fn serialize_msc4356(entries: &[Entry]) -> Value {
    let mut arr: Vec<Value> = Vec::with_capacity(entries.len());
    for e in entries {
        let mut obj = Map::new();
        obj.insert("emoji".into(), Value::String(e.emoji.clone()));
        obj.insert("total".into(), json!(e.total));
        arr.push(Value::Object(obj));
    }
    let mut content = Map::new();
    content.insert("recent_emoji".into(), Value::Array(arr));
    Value::Object(content)
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parse_msc4356_well_formed() {
        let raw = json!({
            "recent_emoji": [
                { "emoji": "😀", "total": 5 },
                { "emoji": "🎉", "total": 2 },
            ]
        });
        let entries = parse_msc4356(&raw);
        assert_eq!(entries.len(), 2);
        assert_eq!(entries[0], Entry { emoji: "😀".into(), total: 5 });
        assert_eq!(entries[1], Entry { emoji: "🎉".into(), total: 2 });
    }

    #[test]
    fn parse_msc4356_round_trips_through_serialize() {
        let entries = vec![
            Entry { emoji: "🚀".into(), total: 7 },
            Entry { emoji: "👋".into(), total: 1 },
        ];
        let raw = serialize_msc4356(&entries);
        let parsed = parse_msc4356(&raw);
        assert_eq!(parsed, entries);
    }

    #[test]
    fn parse_msc4356_skips_malformed_entries() {
        let raw = json!({
            "recent_emoji": [
                { "emoji": "😀", "total": 3 },
                { "total": 2 },                    // missing emoji
                { "emoji": "" },                   // empty emoji
                { "emoji": "🎉" },                 // missing total → 0
                "not even an object",
                { "emoji": "😀", "total": 99 },    // duplicate
            ]
        });
        let entries = parse_msc4356(&raw);
        assert_eq!(entries, vec![
            Entry { emoji: "😀".into(), total: 3 },
            Entry { emoji: "🎉".into(), total: 0 },
        ]);
    }

    #[test]
    fn parse_msc4356_rejects_unrelated_shapes() {
        assert!(parse_msc4356(&Value::Null).is_empty());
        assert!(parse_msc4356(&json!({})).is_empty());
        assert!(parse_msc4356(&json!({ "recent_emoji": "nope" })).is_empty());
        assert!(parse_msc4356(&json!({ "recent_emoji": 42 })).is_empty());
    }

    #[test]
    fn parse_legacy_nested_array_preserves_order() {
        let raw = json!({
            "recent_emoji": [["😀", 5], ["🎉", 3], ["🔥", 1]]
        });
        let entries = parse_legacy_element(&raw);
        assert_eq!(entries.len(), 3);
        assert_eq!(entries[0].emoji, "😀");
        assert_eq!(entries[0].total, 5);
        assert_eq!(entries[1].emoji, "🎉");
        assert_eq!(entries[2].emoji, "🔥");
    }

    #[test]
    fn parse_legacy_nested_array_tolerates_garbage() {
        let raw = json!({
            "recent_emoji": [
                ["😀", 5],
                "not a pair",
                ["🎉"],                  // wrong arity
                ["", 4],                 // empty glyph
                ["🚀", 7],
                ["😀", 999],             // duplicate
            ]
        });
        let entries = parse_legacy_element(&raw);
        assert_eq!(entries, vec![
            Entry { emoji: "😀".into(), total: 5 },
            Entry { emoji: "🚀".into(), total: 7 },
        ]);
    }

    #[test]
    fn parse_legacy_object_map_sorts_by_count_desc() {
        let raw = json!({
            "recent_emoji": { "🎉": 3, "😀": 10, "🔥": 7 }
        });
        let entries = parse_legacy_element(&raw);
        assert_eq!(entries.len(), 3);
        // Object iteration order is undefined in JSON; the parser sorts
        // count desc so we can rely on this even though the input map
        // could come in any order.
        assert_eq!(entries[0], Entry { emoji: "😀".into(), total: 10 });
        assert_eq!(entries[1], Entry { emoji: "🔥".into(), total: 7 });
        assert_eq!(entries[2], Entry { emoji: "🎉".into(), total: 3 });
    }

    #[test]
    fn parse_legacy_rejects_unrelated_shapes() {
        assert!(parse_legacy_element(&Value::Null).is_empty());
        assert!(parse_legacy_element(&json!({})).is_empty());
        assert!(parse_legacy_element(&json!({ "recent_emoji": 7 })).is_empty());
    }

    #[test]
    fn bump_inserts_new_glyph_at_front() {
        let entries = vec![Entry { emoji: "😀".into(), total: 5 }];
        let out = bump(entries, "🎉");
        assert_eq!(out, vec![
            Entry { emoji: "🎉".into(), total: 1 },
            Entry { emoji: "😀".into(), total: 5 },
        ]);
    }

    #[test]
    fn bump_promotes_existing_glyph_and_increments() {
        let entries = vec![
            Entry { emoji: "😀".into(), total: 5 },
            Entry { emoji: "🎉".into(), total: 2 },
            Entry { emoji: "🔥".into(), total: 7 },
        ];
        let out = bump(entries, "🎉");
        assert_eq!(out, vec![
            Entry { emoji: "🎉".into(), total: 3 },
            Entry { emoji: "😀".into(), total: 5 },
            Entry { emoji: "🔥".into(), total: 7 },
        ]);
    }

    #[test]
    fn bump_truncates_to_max_entries() {
        let mut entries: Vec<Entry> = (0..MAX_ENTRIES)
            .map(|i| Entry { emoji: format!("e{}", i), total: 1 })
            .collect();
        // Insert a fresh glyph — the oldest tail entry should fall off.
        entries = bump(entries, "🆕");
        assert_eq!(entries.len(), MAX_ENTRIES);
        assert_eq!(entries[0].emoji, "🆕");
        // The original last entry ("e99") is now at index MAX-2; the
        // tail of the input ("e99") used to be there before insertion.
        assert_eq!(entries.last().unwrap().emoji, format!("e{}", MAX_ENTRIES - 2));
    }

    #[test]
    fn bump_is_noop_on_empty_glyph() {
        let entries = vec![Entry { emoji: "😀".into(), total: 3 }];
        let out = bump(entries.clone(), "");
        assert_eq!(out, entries);
    }

    #[test]
    fn bump_saturates_at_u64_max() {
        let entries = vec![Entry { emoji: "😀".into(), total: u64::MAX }];
        let out = bump(entries, "😀");
        assert_eq!(out[0].total, u64::MAX);
    }

    #[test]
    fn top_by_count_stable_sorts_desc() {
        let entries = vec![
            Entry { emoji: "a".into(), total: 1 },
            Entry { emoji: "b".into(), total: 5 },
            Entry { emoji: "c".into(), total: 3 },
            Entry { emoji: "d".into(), total: 5 },   // tie with b — keep b first
        ];
        assert_eq!(top_by_count(&entries, 3),
                   vec!["b".to_owned(), "d".to_owned(), "c".to_owned()]);
    }

    #[test]
    fn top_by_count_clamps_n() {
        let entries = vec![Entry { emoji: "a".into(), total: 1 }];
        assert_eq!(top_by_count(&entries, 0), Vec::<String>::new());
        assert_eq!(top_by_count(&entries, 99), vec!["a".to_owned()]);
        assert_eq!(top_by_count(&[], 5),    Vec::<String>::new());
    }

    #[test]
    fn serialize_emits_canonical_shape() {
        let entries = vec![
            Entry { emoji: "😀".into(), total: 5 },
            Entry { emoji: "🎉".into(), total: 2 },
        ];
        let v = serialize_msc4356(&entries);
        assert_eq!(v, json!({
            "recent_emoji": [
                { "emoji": "😀", "total": 5 },
                { "emoji": "🎉", "total": 2 },
            ]
        }));
    }
}

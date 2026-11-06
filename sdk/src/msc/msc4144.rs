//! MSC4144: per-message profiles.
//! <https://github.com/matrix-org/matrix-spec-proposals/pull/4144>
//!
//! A bridge or relay bot attaches a `m.per_message_profile` object (unstable
//! `com.beeper.per_message_profile`) to an `m.room.message` or `m.sticker`: an
//! `id`, an optional `displayname` and an optional avatar (`avatar_url`, or
//! an encrypted `avatar_file`). With `has_fallback` the body also starts with
//! a `"{displayname}: "` prefix (and the HTML with a
//! `<strong data-mx-profile-fallback>` element) for clients that don't know
//! the MSC.
//!
//! Pure functions over `serde_json::Value`: no client or room access.

use ruma::events::room::EncryptedFile;
use serde_json::{Map, Value};

pub(crate) const UNSTABLE_KEY: &str = "com.beeper.per_message_profile";
pub(crate) const STABLE_KEY: &str = "m.per_message_profile";

/// The longest `id` or `displayname`, in bytes.
const MAX_FIELD_BYTES: usize = 255;
/// The marker attribute of the HTML fallback element.
const FALLBACK_ATTR: &str = "data-mx-profile-fallback";

/// The avatar a profile asks for.
#[derive(Debug, Clone)]
pub(crate) enum ProfileAvatar {
    /// None given (or an unusable one): show the member's avatar.
    Inherit,
    /// `avatar_url: ""`: no avatar, so initials of the profile name.
    Clear,
    Mxc(String),
    Encrypted(Box<EncryptedFile>),
}

/// A parsed, valid profile.
#[derive(Debug, Clone)]
pub(crate) struct Profile {
    pub id: String,
    /// `None` when omitted, null or empty: show the member's name.
    pub displayname: Option<String>,
    pub avatar: ProfileAvatar,
    pub has_fallback: bool,
}

/// Whether `raw_json` (an event's JSON text) mentions a profile key at all.
/// A cheap check to skip parsing the JSON of the ordinary message.
pub(crate) fn might_have_profile(raw_json: &str) -> bool {
    raw_json.contains(STABLE_KEY) || raw_json.contains(UNSTABLE_KEY)
}

fn valid_field(s: &str) -> bool {
    s.len() <= MAX_FIELD_BYTES && !s.contains('\0')
}

/// The profile of an event's `content` (the latest content, so an edit's
/// `m.new_content`). The stable key is read first, then the unstable one;
/// the first that is an object is used. `None` when there is none, or when
/// its `id` or `displayname` is invalid (the event then renders normally).
pub(crate) fn parse(content: &Value) -> Option<Profile> {
    let obj = [STABLE_KEY, UNSTABLE_KEY]
        .into_iter()
        .filter_map(|key| content.get(key))
        .find_map(Value::as_object)?;

    let id = obj.get("id")?.as_str()?;
    if id.is_empty() || !valid_field(id) {
        return None;
    }
    let displayname = match obj.get("displayname").and_then(Value::as_str) {
        Some(name) if !valid_field(name) => return None,
        Some("") | None => None,
        Some(name) => Some(name.to_owned()),
    };

    Some(Profile {
        id: id.to_owned(),
        displayname,
        avatar: parse_avatar(obj),
        has_fallback: obj.get("has_fallback").and_then(Value::as_bool) == Some(true),
    })
}

fn parse_avatar(obj: &Map<String, Value>) -> ProfileAvatar {
    let file = obj
        .get("avatar_file")
        .filter(|f| f.is_object())
        .and_then(|f| serde_json::from_value::<EncryptedFile>(f.clone()).ok())
        .filter(|f| f.url.is_valid());
    if let Some(file) = file {
        return ProfileAvatar::Encrypted(Box::new(file));
    }
    match obj.get("avatar_url").and_then(Value::as_str) {
        Some("") => ProfileAvatar::Clear,
        Some(url) if url.starts_with("mxc://") && url.len() > "mxc://".len() => {
            ProfileAvatar::Mxc(url.to_owned())
        }
        _ => ProfileAvatar::Inherit,
    }
}

/// Whether the fallback prefix of `profile` is to be cut from `content` (the
/// latest content): it says it has one, has a name to match it by, and is a
/// text, notice or emote message or a captioned media message. A sticker (no
/// `msgtype`) or other message type is left alone. The caller also leaves a
/// forward alone.
pub(crate) fn strips_fallback(content: &Value, profile: &Profile) -> bool {
    if !profile.has_fallback || profile.displayname.is_none() {
        return false;
    }
    match content["msgtype"].as_str() {
        Some("m.text" | "m.notice" | "m.emote") => true,
        Some("m.image" | "m.file" | "m.audio" | "m.video") => content["filename"]
            .as_str()
            .is_some_and(|f| Some(f) != content["body"].as_str()),
        _ => false,
    }
}

/// `body` without the exact `"{name}: "` prefix; `None` when it lacks it.
pub(crate) fn strip_plain_fallback<'a>(body: &'a str, name: &str) -> Option<&'a str> {
    body.strip_prefix(name)?.strip_prefix(": ")
}

/// `html` without its profile fallback element: the first
/// `<strong data-mx-profile-fallback>name: </strong>` after any
/// `</mx-reply>`. `None` when there is none. The sanitizer drops the marker
/// attribute, so this has to run on the raw HTML, before sanitizing.
pub(crate) fn strip_html_fallback(html: &str) -> Option<String> {
    // ASCII lowercasing keeps byte offsets, so positions carry over to `html`.
    let lower = html.to_ascii_lowercase();
    let start = lower
        .rfind("</mx-reply>")
        .map_or(0, |i| i + "</mx-reply>".len());
    let mut from = start;
    while let Some(rel) = lower[from..].find("<strong") {
        let open = from + rel;
        let after_name = open + "<strong".len();
        from = after_name;
        // `<strong>` itself or `<strongx>` is not a candidate.
        if !lower[after_name..].starts_with(|c: char| c.is_ascii_whitespace()) {
            continue;
        }
        let gt = lower[after_name..].find('>')?;
        let attrs: String = lower[after_name..after_name + gt]
            .chars()
            .filter(|c| !c.is_ascii_whitespace())
            .collect();
        let marked = attrs == FALLBACK_ATTR
            || attrs == format!("{FALLBACK_ATTR}=\"\"")
            || attrs == format!("{FALLBACK_ATTR}=''");
        if !marked {
            continue;
        }
        let inner_start = after_name + gt + 1;
        let close_rel = lower[inner_start..].find("</strong")?;
        let inner_end = inner_start + close_rel;
        let inner = &html[inner_start..inner_end];
        let tail = lower[inner_end + "</strong".len()..].trim_start();
        let tail_ws = lower[inner_end + "</strong".len()..].len() - tail.len();
        if inner.contains('<') || !inner.trim_end().ends_with(':') || !tail.starts_with('>') {
            return None;
        }
        let end = inner_end + "</strong".len() + tail_ws + 1;
        return Some(format!("{}{}", &html[..open], &html[end..]));
    }
    None
}

/// Remove both profile keys from `obj`, e.g. when content is copied into a
/// forward, which must not look as if the persona sent it.
pub(crate) fn remove_keys(obj: &mut Map<String, Value>) {
    obj.remove(STABLE_KEY);
    obj.remove(UNSTABLE_KEY);
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn content(profile: Value) -> Value {
        json!({ "msgtype": "m.text", "body": "cat: hi", STABLE_KEY: profile })
    }

    fn avatar_of(profile: Value) -> ProfileAvatar {
        parse(&content(profile)).unwrap().avatar
    }

    fn encrypted(url: &str) -> Value {
        json!({
            "url": url,
            "key": { "kty": "oct", "key_ops": ["encrypt", "decrypt"], "alg": "A256CTR",
                     "k": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "ext": true },
            "iv": "AAAAAAAAAAAAAAAAAAAAAA",
            "hashes": { "sha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
            "v": "v2"
        })
    }

    fn profile(name: Option<&str>, has_fallback: bool) -> Profile {
        Profile {
            id: "cat".into(),
            displayname: name.map(str::to_owned),
            avatar: ProfileAvatar::Inherit,
            has_fallback,
        }
    }

    // ---- parse: keys ----

    #[test]
    fn parses_the_stable_key() {
        let p = parse(&content(json!({ "id": "cat", "displayname": "Cat" }))).unwrap();
        assert_eq!(p.id, "cat");
        assert_eq!(p.displayname.as_deref(), Some("Cat"));
        assert!(!p.has_fallback);
    }

    #[test]
    fn parses_the_unstable_key() {
        let c =
            json!({ UNSTABLE_KEY: { "id": "cat", "displayname": "Cat", "has_fallback": true } });
        let p = parse(&c).unwrap();
        assert_eq!(p.id, "cat");
        assert!(p.has_fallback);
    }

    #[test]
    fn stable_key_wins_over_unstable() {
        let c = json!({
            STABLE_KEY: { "id": "new" },
            UNSTABLE_KEY: { "id": "old" }
        });
        assert_eq!(parse(&c).unwrap().id, "new");
    }

    #[test]
    fn a_stable_key_that_is_not_an_object_falls_through() {
        let c = json!({ STABLE_KEY: "nope", UNSTABLE_KEY: { "id": "old" } });
        assert_eq!(parse(&c).unwrap().id, "old");
        assert!(parse(&json!({ STABLE_KEY: 5 })).is_none());
        assert!(parse(&json!({ "body": "x" })).is_none());
    }

    #[test]
    fn an_invalid_stable_profile_is_not_replaced_by_the_unstable_one() {
        let c = json!({ STABLE_KEY: { "id": "" }, UNSTABLE_KEY: { "id": "old" } });
        assert!(parse(&c).is_none());
    }

    #[test]
    fn might_have_profile_spots_either_key() {
        assert!(might_have_profile(&format!(
            r#"{{"content":{{"{STABLE_KEY}":{{}}}}}}"#
        )));
        assert!(might_have_profile(&format!(
            r#"{{"content":{{"{UNSTABLE_KEY}":{{}}}}}}"#
        )));
        assert!(!might_have_profile(r#"{"content":{"body":"hi"}}"#));
    }

    // ---- parse: id ----

    #[test]
    fn invalid_ids_ignore_the_whole_profile() {
        let bad = |id: Value| parse(&content(json!({ "id": id, "displayname": "Cat" }))).is_none();
        assert!(bad(json!("")));
        assert!(bad(json!(5)));
        assert!(bad(json!(null)));
        assert!(bad(json!("a\0b")));
        assert!(bad(json!("x".repeat(256))));
        assert!(parse(&content(json!({ "displayname": "Cat" }))).is_none());
        assert!(parse(&content(json!({ "id": "x".repeat(255) }))).is_some());
    }

    // ---- parse: displayname ----

    #[test]
    fn omitted_null_and_empty_names_mean_the_member_name() {
        for name in [json!(null), json!("")] {
            let p = parse(&content(json!({ "id": "c", "displayname": name }))).unwrap();
            assert!(p.displayname.is_none());
        }
        assert!(parse(&content(json!({ "id": "c" })))
            .unwrap()
            .displayname
            .is_none());
    }

    #[test]
    fn invalid_names_ignore_the_whole_profile() {
        let long = "n".repeat(256);
        assert!(parse(&content(json!({ "id": "c", "displayname": long }))).is_none());
        assert!(parse(&content(json!({ "id": "c", "displayname": "a\0b" }))).is_none());
        let ok = "n".repeat(255);
        assert!(parse(&content(json!({ "id": "c", "displayname": ok }))).is_some());
    }

    #[test]
    fn name_length_is_counted_in_bytes() {
        // 128 two-byte characters are 256 bytes.
        let long = "é".repeat(128);
        assert!(parse(&content(json!({ "id": "c", "displayname": long }))).is_none());
    }

    #[test]
    fn has_fallback_needs_a_true_boolean() {
        let fb = |v: Value| {
            parse(&content(json!({ "id": "c", "has_fallback": v })))
                .unwrap()
                .has_fallback
        };
        assert!(fb(json!(true)));
        assert!(!fb(json!(false)));
        assert!(!fb(json!("true")));
        assert!(!fb(json!(1)));
    }

    // ---- parse: avatar ----

    #[test]
    fn avatar_url_mxc() {
        assert!(matches!(
            avatar_of(json!({ "id": "c", "avatar_url": "mxc://x/y" })),
            ProfileAvatar::Mxc(u) if u == "mxc://x/y"
        ));
    }

    #[test]
    fn empty_avatar_url_clears_the_avatar() {
        assert!(matches!(
            avatar_of(json!({ "id": "c", "avatar_url": "" })),
            ProfileAvatar::Clear
        ));
    }

    #[test]
    fn unusable_or_missing_avatars_inherit() {
        for avatar in [
            json!({ "id": "c" }),
            json!({ "id": "c", "avatar_url": "https://evil/x.png" }),
            json!({ "id": "c", "avatar_url": "mxc://" }),
            json!({ "id": "c", "avatar_url": 5 }),
            json!({ "id": "c", "avatar_url": null }),
        ] {
            assert!(matches!(avatar_of(avatar), ProfileAvatar::Inherit));
        }
    }

    #[test]
    fn avatar_file_wins_over_avatar_url() {
        let a = avatar_of(json!({
            "id": "c", "avatar_url": "mxc://x/plain", "avatar_file": encrypted("mxc://x/enc")
        }));
        match a {
            ProfileAvatar::Encrypted(f) => assert_eq!(f.url.as_str(), "mxc://x/enc"),
            other => panic!("expected Encrypted, got {other:?}"),
        }
    }

    #[test]
    fn an_invalid_avatar_file_falls_back_to_the_url() {
        // Not an mxc url.
        let a = avatar_of(json!({
            "id": "c", "avatar_url": "mxc://x/plain", "avatar_file": encrypted("https://evil/x")
        }));
        assert!(matches!(a, ProfileAvatar::Mxc(u) if u == "mxc://x/plain"));
        // Not a file at all.
        let a = avatar_of(json!({
            "id": "c", "avatar_url": "mxc://x/plain", "avatar_file": { "url": "mxc://x/y" }
        }));
        assert!(matches!(a, ProfileAvatar::Mxc(_)));
        let a = avatar_of(json!({ "id": "c", "avatar_file": "nope" }));
        assert!(matches!(a, ProfileAvatar::Inherit));
        // Invalid file and an empty url: cleared.
        let a = avatar_of(json!({
            "id": "c", "avatar_url": "", "avatar_file": encrypted("https://evil/x")
        }));
        assert!(matches!(a, ProfileAvatar::Clear));
    }

    // ---- strips_fallback ----

    #[test]
    fn strips_for_text_notice_and_emote() {
        let p = profile(Some("cat"), true);
        for msgtype in ["m.text", "m.notice", "m.emote"] {
            assert!(strips_fallback(
                &json!({ "msgtype": msgtype, "body": "cat: hi" }),
                &p
            ));
        }
    }

    #[test]
    fn strips_a_media_caption_only() {
        let p = profile(Some("cat"), true);
        for msgtype in ["m.image", "m.file", "m.audio", "m.video"] {
            // A caption: `filename` differs from `body`.
            assert!(strips_fallback(
                &json!({ "msgtype": msgtype, "body": "cat: look", "filename": "a.png" }),
                &p
            ));
            // The body is the file name.
            assert!(!strips_fallback(
                &json!({ "msgtype": msgtype, "body": "a.png", "filename": "a.png" }),
                &p
            ));
            assert!(!strips_fallback(
                &json!({ "msgtype": msgtype, "body": "a.png" }),
                &p
            ));
        }
    }

    #[test]
    fn does_not_strip_stickers_or_other_types() {
        let p = profile(Some("cat"), true);
        assert!(!strips_fallback(
            &json!({ "body": "cat: hi", "url": "mxc://x/y" }),
            &p
        ));
        assert!(!strips_fallback(
            &json!({ "msgtype": "m.location", "body": "cat: here" }),
            &p
        ));
        assert!(!strips_fallback(&json!({}), &p));
    }

    #[test]
    fn does_not_strip_without_a_fallback_or_a_name() {
        let c = json!({ "msgtype": "m.text", "body": "cat: hi" });
        assert!(!strips_fallback(&c, &profile(Some("cat"), false)));
        assert!(!strips_fallback(&c, &profile(None, true)));
    }

    // ---- strip_plain_fallback ----

    #[test]
    fn strips_the_exact_prefix() {
        assert_eq!(strip_plain_fallback("cat: hi", "cat"), Some("hi"));
        assert_eq!(strip_plain_fallback("cat: ", "cat"), Some(""));
        assert_eq!(strip_plain_fallback("cat: cat: hi", "cat"), Some("cat: hi"));
        assert_eq!(strip_plain_fallback("Zoë 🐱: hi", "Zoë 🐱"), Some("hi"));
    }

    #[test]
    fn near_misses_are_not_stripped() {
        assert_eq!(strip_plain_fallback("Cat: hi", "cat"), None);
        assert_eq!(strip_plain_fallback("cat:hi", "cat"), None);
        assert_eq!(strip_plain_fallback("cat :hi", "cat"), None);
        assert_eq!(strip_plain_fallback(" cat: hi", "cat"), None);
        assert_eq!(strip_plain_fallback("cats: hi", "cat"), None);
        assert_eq!(strip_plain_fallback("hi", "cat"), None);
    }

    // ---- strip_html_fallback ----

    #[test]
    fn strips_the_marked_element() {
        assert_eq!(
            strip_html_fallback("<strong data-mx-profile-fallback>cat: </strong>hello").as_deref(),
            Some("hello")
        );
    }

    #[test]
    fn strips_with_an_empty_attribute_value() {
        for open in [
            r#"<strong data-mx-profile-fallback="">"#,
            "<strong data-mx-profile-fallback=''>",
        ] {
            assert_eq!(
                strip_html_fallback(&format!("{open}cat: </strong>hello")).as_deref(),
                Some("hello")
            );
        }
    }

    #[test]
    fn strips_inside_a_paragraph() {
        assert_eq!(
            strip_html_fallback("<p><strong data-mx-profile-fallback>cat: </strong>hello</p>")
                .as_deref(),
            Some("<p>hello</p>")
        );
    }

    #[test]
    fn tolerates_extra_whitespace_and_a_spaced_close_tag() {
        assert_eq!(
            strip_html_fallback("<strong   data-mx-profile-fallback = \"\" >cat: </strong >hello")
                .as_deref(),
            Some("hello")
        );
        assert_eq!(
            strip_html_fallback("<STRONG DATA-MX-PROFILE-FALLBACK>cat: </STRONG>hello").as_deref(),
            Some("hello")
        );
    }

    #[test]
    fn strips_an_escaped_name() {
        assert_eq!(
            strip_html_fallback("<strong data-mx-profile-fallback>a&lt;b&amp;c: </strong><b>x</b>")
                .as_deref(),
            Some("<b>x</b>")
        );
    }

    #[test]
    fn keeps_other_markup_around_the_element() {
        assert_eq!(
            strip_html_fallback(
                "<em>a</em><strong>bold</strong><strong data-mx-profile-fallback>cat: </strong>b"
            )
            .as_deref(),
            Some("<em>a</em><strong>bold</strong>b")
        );
    }

    #[test]
    fn leaves_a_fallback_inside_the_reply_quote_alone() {
        let html = "<mx-reply><blockquote><strong data-mx-profile-fallback>cat: </strong>old\
                    </blockquote></mx-reply>new";
        assert_eq!(strip_html_fallback(html), None);
        // The one after the quote is still stripped.
        let both = "<mx-reply><blockquote><strong data-mx-profile-fallback>dog: </strong>old\
                    </blockquote></mx-reply><strong data-mx-profile-fallback>cat: </strong>new";
        assert_eq!(
            strip_html_fallback(both).as_deref(),
            Some(
                "<mx-reply><blockquote><strong data-mx-profile-fallback>dog: </strong>old\
                  </blockquote></mx-reply>new"
            )
        );
    }

    #[test]
    fn no_match_is_none() {
        assert_eq!(strip_html_fallback(""), None);
        assert_eq!(strip_html_fallback("hello"), None);
        assert_eq!(strip_html_fallback("<strong>cat: </strong>hello"), None);
        // Another attribute.
        assert_eq!(
            strip_html_fallback("<strong class=x data-mx-profile-fallback>cat: </strong>hi"),
            None
        );
        // Not name-colon shaped, nested markup or unterminated.
        assert_eq!(
            strip_html_fallback("<strong data-mx-profile-fallback>cat</strong>hi"),
            None
        );
        assert_eq!(
            strip_html_fallback("<strong data-mx-profile-fallback><b>cat</b>: </strong>hi"),
            None
        );
        assert_eq!(
            strip_html_fallback("<strong data-mx-profile-fallback>cat: hi"),
            None
        );
        assert_eq!(
            strip_html_fallback("<strong data-mx-profile-fallback"),
            None
        );
    }

    // ---- remove_keys ----

    #[test]
    fn remove_keys_removes_both() {
        let mut v = json!({ "body": "x", STABLE_KEY: {}, UNSTABLE_KEY: {} });
        remove_keys(v.as_object_mut().unwrap());
        assert_eq!(v, json!({ "body": "x" }));
    }
}

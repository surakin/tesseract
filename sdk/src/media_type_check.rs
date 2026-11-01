//! Safety check run before a received attachment is handed to the OS viewer.
//!
//! Compares three independent claims about a file — its extension, the
//! `mimetype` the sender declared, and the format its leading bytes actually
//! look like — and classifies the result. Executables and scripts are refused
//! outright, whatever the claims say.

/// Outcome of [`check`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    /// Extension, declared mime and (when recognisable) content agree.
    Ok,
    /// The extension does not belong to the declared mime type.
    ExtMimeMismatch,
    /// The recognised content format contradicts the declared mime or the extension.
    ContentMismatch,
    /// The file can run code; never launched.
    Executable,
    /// Consistent, but not a known-safe media type (image, audio, video, PDF,
    /// plain text): other formats can carry macros or links, so ask first.
    NotMedia,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MediaTypeCheck {
    pub verdict: Verdict,
    /// Lower-case extension of the sanitized name, without the dot.
    pub ext: String,
    /// Declared mime reduced to `type/subtype`, lower-case, aliases folded.
    pub declared_mime: String,
    /// Mime recognised from the file contents; empty when unrecognised.
    pub sniffed_mime: String,
    /// Extension (no dot) that would match the file best; empty when unknown.
    pub suggested_ext: String,
    /// Filesystem-safe version of the sender-supplied name.
    pub safe_filename: String,
    /// `safe_filename` with its extension replaced by `suggested_ext`.
    pub renamed_filename: String,
    /// Best guess at the file's type (contents, else declared, else extension);
    /// empty when nothing is known.
    pub effective_mime: String,
}

const EXEC_EXTS: &[&str] = &[
    "exe",
    "com",
    "scr",
    "bat",
    "cmd",
    "msi",
    "msp",
    "mst",
    "lnk",
    "url",
    "pif",
    "cpl",
    "dll",
    "sys",
    "ocx",
    "ps1",
    "ps1xml",
    "psm1",
    "psd1",
    "vbs",
    "vbe",
    "vb",
    "js",
    "jse",
    "wsf",
    "wsh",
    "hta",
    "msc",
    "reg",
    "inf",
    "scf",
    "chm",
    "jar",
    "jnlp",
    "appx",
    "msix",
    "gadget",
    "sh",
    "bash",
    "zsh",
    "fish",
    "csh",
    "ksh",
    "command",
    "run",
    "bin",
    "elf",
    "appimage",
    "desktop",
    "deb",
    "rpm",
    "app",
    "pkg",
    "dmg",
    "workflow",
    "action",
    "scpt",
    "applescript",
    "terminal",
    "py",
    "pyw",
    "pl",
    "rb",
    "php",
    "settingcontent-ms",
    // Documents the OS opens with scripting enabled.
    "html",
    "htm",
    "xhtml",
    "xht",
    "mht",
    "mhtml",
    "shtml",
    "svg",
    "svgz",
    "xml",
    "xsl",
    "xslt",
    "docm",
    "dotm",
    "xlsm",
    "xltm",
    "xlam",
    "pptm",
    "potm",
    "ppam",
    "ppsm",
    "sldm",
    "xll",
    "iqy",
    "slk",
    "application",
    "appref-ms",
    "diagcab",
    "library-ms",
    "search-ms",
    "theme",
    "themepack",
    "wsc",
    "sct",
    "ps2",
    "ps2xml",
    "msh",
    "msh1",
    "msh2",
    "mshxml",
    "webloc",
    "inetloc",
    "fileloc",
    "tool",
    "ade",
    "adp",
    "mda",
    "mdb",
    "ws",
    "vbscript",
    // Types the OS mounts, installs or configures when opened.
    "rdp",
    "mobileconfig",
    "iso",
    "img",
    "vhd",
    "vhdx",
    "cab",
    "one",
    "onetoc2",
    "crx",
    "xpi",
    "flatpakref",
    "flatpakrepo",
    "ovpn",
    "ics",
    "eml",
    "torrent",
    "website",
    "hlp",
    "pyc",
    "kdelnk",
];

const EXEC_MIMES: &[&str] = &[
    "application/x-msdownload",
    "application/x-msdos-program",
    "application/x-dosexec",
    "application/vnd.microsoft.portable-executable",
    "application/x-executable",
    "application/x-elf",
    "application/x-sharedlib",
    "application/x-mach-binary",
    "application/x-sh",
    "application/x-shellscript",
    "application/x-csh",
    "application/x-bat",
    "application/x-msi",
    "application/x-ms-shortcut",
    "application/x-desktop",
    "application/x-java-archive",
    "application/java-archive",
    "application/x-apple-diskimage",
    "application/vnd.debian.binary-package",
    "application/x-rpm",
    "application/x-powershell",
    "text/x-shellscript",
    "text/x-sh",
    "text/x-python",
    "application/x-python-code",
    "application/javascript",
    "application/x-javascript",
    "application/ecmascript",
    "text/javascript",
    "text/ecmascript",
    "text/vbscript",
    "application/x-httpd-php",
    "text/x-php",
    "text/x-perl",
    "text/x-ruby",
    "application/x-ruby",
    "application/x-perl",
    "text/html",
    "application/xhtml+xml",
    "image/svg+xml",
    "multipart/related",
    "message/rfc822",
    "application/hta",
    "application/vnd.ms-word.document.macroenabled.12",
    "application/vnd.ms-excel.sheet.macroenabled.12",
    "application/vnd.ms-powerpoint.presentation.macroenabled.12",
];

/// Mime groups whose members are interchangeable containers for this check
/// (an `.ogg` sent as `audio/ogg` or `video/ogg`, an `.m4a` sent as `audio/mp4`...).
const FAMILIES: &[&[&str]] = &[
    &[
        "audio/ogg",
        "video/ogg",
        "application/ogg",
        "audio/opus",
        "audio/vorbis",
    ],
    &[
        "video/mp4",
        "audio/mp4",
        "audio/x-m4a",
        "audio/m4a",
        "video/quicktime",
        "video/x-m4v",
        "audio/aac",
    ],
    &[
        "video/webm",
        "audio/webm",
        "video/x-matroska",
        "audio/x-matroska",
    ],
    &["audio/mpeg", "audio/mp3"],
    &["audio/wav", "audio/x-wav", "audio/vnd.wave", "audio/wave"],
    &["image/tiff", "image/tif"],
    &["text/plain", "text/markdown", "text/csv", "text/x-log"],
];

fn is_zip_based(mime: &str, ext: &str) -> bool {
    mime == "application/zip"
        || mime == "application/x-zip-compressed"
        || mime.ends_with("+zip")
        || mime.starts_with("application/vnd.openxmlformats-officedocument.")
        || mime.starts_with("application/vnd.oasis.opendocument.")
        || mime == "application/epub+zip"
        || mime == "application/vnd.android.package-archive"
        || mime == "application/x-xpinstall"
        || matches!(
            ext,
            "zip" | "docx" | "xlsx" | "pptx" | "odt" | "ods" | "odp" | "epub" | "apk" | "cbz"
        )
}

fn fold_alias(m: &str) -> &str {
    match m {
        "image/jpg" | "image/pjpeg" => "image/jpeg",
        "audio/mp3" | "audio/x-mpeg" | "audio/x-mp3" => "audio/mpeg",
        "audio/x-wav" | "audio/wave" | "audio/vnd.wave" => "audio/wav",
        "image/x-png" => "image/png",
        "application/x-pdf" => "application/pdf",
        "image/tif" => "image/tiff",
        "audio/x-flac" => "audio/flac",
        "image/heif" => "image/heic",
        "image/heif-sequence" | "image/heic-sequence" => "image/heic",
        "image/x-icon" | "image/vnd.microsoft.icon" | "image/x-ico" => "image/vnd.microsoft.icon",
        "audio/x-aiff" => "audio/aiff",
        "audio/x-ms-wma" => "audio/x-ms-wma",
        other => other,
    }
}

/// `type/subtype`, lower-case, without parameters, with common aliases folded.
pub fn normalize_mime(raw: &str) -> String {
    let base = raw
        .split(';')
        .next()
        .unwrap_or("")
        .trim()
        .to_ascii_lowercase();
    fold_alias(&base).to_owned()
}

fn is_generic(mime: &str) -> bool {
    mime.is_empty() || mime == "application/octet-stream" || mime == "binary/octet-stream"
}

fn equivalent(a: &str, b: &str) -> bool {
    let (a, b) = (fold_alias(a), fold_alias(b));
    if a == b {
        return true;
    }
    FAMILIES.iter().any(|f| f.contains(&a) && f.contains(&b))
}

/// Characters that make a name display differently from what it is
/// (right-to-left overrides turn `gpj.exe` into `exe.jpg` on screen).
fn is_bidi_control(c: char) -> bool {
    matches!(c, '\u{202A}'..='\u{202E}' | '\u{2066}'..='\u{2069}' | '\u{200E}' | '\u{200F}' | '\u{061C}')
}

const MAX_NAME_BYTES: usize = 120;

/// Reduces a sender-supplied name to something safe to create on disk: no
/// directories, no control/bidi characters, no characters invalid on Windows,
/// no leading/trailing dots or spaces, no reserved device names, bounded length.
pub fn sanitize_filename(raw: &str) -> String {
    let leaf = raw.rsplit(['/', '\\']).next().unwrap_or("");
    let mut s: String = leaf
        .chars()
        .filter(|c| !c.is_control() && !is_bidi_control(*c))
        .map(|c| {
            if matches!(c, '<' | '>' | ':' | '"' | '|' | '?' | '*') {
                '_'
            } else {
                c
            }
        })
        .collect();
    s = s
        .trim_matches(|c: char| c == '.' || c.is_whitespace())
        .to_owned();

    // Windows reserved device names are reserved with any extension.
    let stem_upper = s
        .split('.')
        .next()
        .unwrap_or("")
        .trim()
        .to_ascii_uppercase();
    let reserved = matches!(stem_upper.as_str(), "CON" | "PRN" | "AUX" | "NUL")
        || (stem_upper.len() == 4
            && (stem_upper.starts_with("COM") || stem_upper.starts_with("LPT"))
            && stem_upper.as_bytes()[3].is_ascii_digit()
            && stem_upper.as_bytes()[3] != b'0');
    if reserved {
        s.insert(0, '_');
    }

    if s.len() > MAX_NAME_BYTES {
        let (stem, ext) = split_ext(&s);
        let keep_ext = if ext.len() <= 16 { ext } else { "" };
        let budget =
            MAX_NAME_BYTES.saturating_sub(keep_ext.len() + usize::from(!keep_ext.is_empty()));
        let mut cut = budget.min(stem.len());
        while !stem.is_char_boundary(cut) {
            cut -= 1;
        }
        let mut out = stem[..cut].trim_end_matches(['.', ' ']).to_owned();
        if !keep_ext.is_empty() {
            out.push('.');
            out.push_str(keep_ext);
        }
        s = out;
    }

    if s.is_empty() {
        s.push_str("download");
    }
    s
}

/// `(stem, ext)` where `ext` has no dot; a leading-dot-only name has no ext.
fn split_ext(name: &str) -> (&str, &str) {
    match name.rfind('.') {
        Some(i) if i > 0 && i + 1 < name.len() => (&name[..i], &name[i + 1..]),
        _ => (name, ""),
    }
}

fn preferred_ext(mime: &str) -> Option<&'static str> {
    Some(match mime {
        "image/jpeg" => "jpg",
        "image/png" => "png",
        "image/gif" => "gif",
        "image/webp" => "webp",
        "image/jxl" => "jxl",
        "image/tiff" => "tiff",
        "image/svg+xml" => "svg",
        "audio/mpeg" => "mp3",
        "audio/ogg" => "ogg",
        "audio/wav" => "wav",
        "audio/mp4" => "m4a",
        "video/mp4" => "mp4",
        "video/webm" => "webm",
        "video/quicktime" => "mov",
        "video/x-matroska" => "mkv",
        "application/pdf" => "pdf",
        "application/zip" => "zip",
        "text/plain" => "txt",
        _ => return None,
    })
}

fn ext_for_mime(mime: &str) -> String {
    if let Some(e) = preferred_ext(mime) {
        return e.to_owned();
    }
    match mime_guess::get_mime_extensions_str(mime) {
        Some(list) if !list.is_empty() => {
            // The first entry is alphabetical ("jpe" before "jpg"); prefer a
            // conventional 3-4 letter one.
            list.iter()
                .find(|e| (3..=4).contains(&e.len()))
                .or_else(|| list.first())
                .map(|e| (*e).to_owned())
                .unwrap_or_default()
        }
        _ => String::new(),
    }
}

/// Mime recognised from the leading bytes, plus whether it is executable code.
fn sniff(head: &[u8]) -> (String, bool) {
    // A shebang line: "#!" followed by an absolute interpreter path or `env`.
    if head.starts_with(b"#!") {
        let rest = head[2..].iter().copied().skip_while(|b| *b == b' ');
        let first: Vec<u8> = rest.take(5).collect();
        if first.first() == Some(&b'/') || first.starts_with(b"env ") {
            return ("text/x-shellscript".to_owned(), true);
        }
        // Not a real shebang: infer would still call it a shell script.
        return (String::new(), false);
    }
    // A DOS "MZ" header alone is too weak (plain text can start with it): need
    // the PE signature the header points at.
    if head.starts_with(b"MZ") {
        if head.len() >= 0x40 {
            let off = u32::from_le_bytes([head[0x3C], head[0x3D], head[0x3E], head[0x3F]]) as usize;
            if head.get(off..off + 4) == Some(b"PE\0\0") {
                return (
                    "application/vnd.microsoft.portable-executable".to_owned(),
                    true,
                );
            }
            // The header may point past the bytes we were given; a binary
            // header (NULs in the DOS stub area) still counts.
            if off >= head.len() && head[2..0x40].contains(&0) {
                return (
                    "application/vnd.microsoft.portable-executable".to_owned(),
                    true,
                );
            }
        }
        // Not a real PE: infer would still call any "MZ" file an .exe.
        return (String::new(), false);
    }
    if head.starts_with(b"\x7fELF") {
        return ("application/x-executable".to_owned(), true);
    }
    // Windows shell link header.
    if head.starts_with(&[0x4C, 0x00, 0x00, 0x00, 0x01, 0x14, 0x02, 0x00]) {
        return ("application/x-ms-shortcut".to_owned(), true);
    }
    // Mach-O, thin and fat (both byte orders).
    const MACHO: [[u8; 4]; 6] = [
        [0xFE, 0xED, 0xFA, 0xCE],
        [0xFE, 0xED, 0xFA, 0xCF],
        [0xCE, 0xFA, 0xED, 0xFE],
        [0xCF, 0xFA, 0xED, 0xFE],
        [0xCA, 0xFE, 0xBA, 0xBE],
        [0xBE, 0xBA, 0xFE, 0xCA],
    ];
    if head.len() >= 4 && MACHO.iter().any(|m| head[..4] == m[..]) {
        // 0xCAFEBABE is also a Java class file; both are code.
        return ("application/x-mach-binary".to_owned(), true);
    }
    // JPEG XL: bare codestream or ISOBMFF container (infer has no matcher).
    if head.starts_with(&[0xFF, 0x0A]) || head.starts_with(b"\0\0\0\x0cJXL \r\n\x87\n") {
        return ("image/jxl".to_owned(), false);
    }
    match infer::get(head) {
        Some(t) => {
            let exec =
                t.matcher_type() == infer::MatcherType::App || EXEC_MIMES.contains(&t.mime_type());
            (t.mime_type().to_owned(), exec)
        }
        None => (String::new(), false),
    }
}

/// Classify `filename` + `declared_mime` against `head` (the first few KiB of
/// the file; 8 KiB is plenty).
pub fn check(filename: &str, declared_mime: &str, head: &[u8]) -> MediaTypeCheck {
    let safe_filename = sanitize_filename(filename);
    let (_, ext_raw) = split_ext(&safe_filename);
    let ext = ext_raw.to_ascii_lowercase();
    let declared = normalize_mime(declared_mime);
    let (sniffed, sniffed_exec) = sniff(head);
    let declared_known = !is_generic(&declared);

    // Best extension for the file: what the contents say, else the declared type.
    let suggested_ext = if !sniffed.is_empty() {
        infer::get(head)
            .map(|t| t.extension().to_owned())
            .filter(|e| !e.is_empty())
            .map(|e| preferred_ext(&sniffed).map(str::to_owned).unwrap_or(e))
            .unwrap_or_else(|| ext_for_mime(&sniffed))
    } else if declared_known {
        ext_for_mime(&declared)
    } else {
        String::new()
    };
    // A rename must never produce a launchable extension (a declared
    // "application/javascript" would otherwise offer "Rename to .js").
    let suggested_ext = if EXEC_EXTS.contains(&suggested_ext.as_str()) {
        String::new()
    } else {
        suggested_ext
    };
    let renamed_filename = if suggested_ext.is_empty() {
        safe_filename.clone()
    } else {
        let (stem, _) = split_ext(&safe_filename);
        format!("{stem}.{suggested_ext}")
    };

    let ext_first_mime = if ext.is_empty() {
        String::new()
    } else {
        mime_guess::from_ext(&ext)
            .first()
            .map(|m| normalize_mime(m.essence_str()))
            .unwrap_or_default()
    };
    let effective_mime = if !sniffed.is_empty() {
        sniffed.clone()
    } else if declared_known {
        declared.clone()
    } else {
        ext_first_mime
    };

    let make = |verdict| MediaTypeCheck {
        verdict,
        effective_mime: effective_mime.clone(),
        ext: ext.clone(),
        declared_mime: declared.clone(),
        sniffed_mime: sniffed.clone(),
        suggested_ext: suggested_ext.clone(),
        safe_filename: safe_filename.clone(),
        renamed_filename: renamed_filename.clone(),
    };

    if sniffed_exec || EXEC_EXTS.contains(&ext.as_str()) || EXEC_MIMES.contains(&declared.as_str())
    {
        return make(Verdict::Executable);
    }

    // The extensions' own mime types, for comparison against the other claims.
    let ext_mimes: Vec<String> = if ext.is_empty() {
        Vec::new()
    } else {
        mime_guess::from_ext(&ext)
            .iter()
            .map(|m| normalize_mime(m.essence_str()))
            .collect()
    };
    let ext_matches = |mime: &str| {
        ext_mimes.iter().any(|m| equivalent(m, mime))
            || (is_zip_based(mime, "") && is_zip_based("", &ext))
    };

    // Content vs the other two claims. An unrecognised format never warns.
    if !sniffed.is_empty() {
        let vs_declared = !declared_known
            || equivalent(&sniffed, &declared)
            || (is_zip_based(&sniffed, "") && is_zip_based(&declared, ""));
        let vs_ext = ext_mimes.is_empty() || ext_matches(&sniffed);
        if !vs_declared || !vs_ext {
            return make(Verdict::ContentMismatch);
        }
    }

    // Extension vs declared type.
    if declared_known {
        let ok = !ext.is_empty() && (ext_matches(&declared) || ext_mimes.is_empty());
        // An extension mime_guess has never heard of cannot be judged; only an
        // absent extension is a definite mismatch.
        if !ok {
            return make(Verdict::ExtMimeMismatch);
        }
    }

    // The OS picks the app from the extension, not from what the sender claims
    // or what the bytes look like, so the extension itself must be a known-safe
    // media type. An extension mime_guess has never heard of (".rdp", ".iso")
    // can't be vouched for, however convincing the declared type and contents.
    let ext_is_safe = ext.is_empty() || ext_mimes.iter().any(|m| is_safe_media(m));
    if !ext_is_safe || !is_safe_media(&effective_mime) {
        return make(Verdict::NotMedia);
    }
    make(Verdict::Ok)
}

/// Types that open in a viewer without scripting or macros: raster/vector-free
/// images, audio, video, PDF and plain text. (SVG is already refused as code.)
fn is_safe_media(mime: &str) -> bool {
    mime.starts_with("image/")
        || mime.starts_with("audio/")
        || mime.starts_with("video/")
        || mime == "application/pdf"
        || mime == "text/plain"
}

#[cfg(test)]
mod tests {
    use super::*;

    const PNG: &[u8] =
        b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x01\0\0\0\x01\x08\x06\0\0\0\x1f\x15\xc4\x89";
    const JPEG: &[u8] = b"\xff\xd8\xff\xe0\0\x10JFIF\0\x01\x01\0\0\x01\0\x01\0\0\xff\xdb";
    // Minimal DOS stub whose e_lfanew (0x40) points at a "PE\0\0" signature.
    const PE_HEADER: [u8; 0x48] = {
        let mut h = [0u8; 0x48];
        h[0] = b'M';
        h[1] = b'Z';
        h[0x3C] = 0x40;
        h[0x40] = b'P';
        h[0x41] = b'E';
        h
    };
    const PDF: &[u8] = b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n";

    #[test]
    fn matching_png_is_ok() {
        let r = check("cat.png", "image/png", PNG);
        assert_eq!(r.verdict, Verdict::Ok);
        assert_eq!(r.sniffed_mime, "image/png");
    }

    #[test]
    fn jpg_alias_and_params_are_normalised() {
        assert_eq!(check("a.jpg", "image/jpg", JPEG).verdict, Verdict::Ok);
        assert_eq!(
            check("a.JPEG", "IMAGE/JPEG; charset=x", JPEG).verdict,
            Verdict::Ok
        );
    }

    #[test]
    fn jxl_is_recognised_by_content() {
        let codestream: &[u8] = &[0xFF, 0x0A, 0x00, 0x00];
        let container: &[u8] = b"\0\0\0\x0cJXL \r\n\x87\n";
        for head in [codestream, container] {
            let r = check("photo.jxl", "image/jxl", head);
            assert_eq!(r.verdict, Verdict::Ok, "{r:?}");
            assert_eq!(r.sniffed_mime, "image/jxl");
        }
        // A JXL sent under a .png name is a content mismatch with a .jxl suggestion.
        let r = check("cat.png", "image/png", codestream);
        assert_eq!(r.verdict, Verdict::ContentMismatch);
        assert_eq!(r.suggested_ext, "jxl");
    }

    #[test]
    fn extension_vs_declared_mime_mismatch() {
        let r = check("cat.png", "image/jpeg", b"");
        assert_eq!(r.verdict, Verdict::ExtMimeMismatch);
        assert_eq!(r.suggested_ext, "jpg");
        assert_eq!(r.renamed_filename, "cat.jpg");
    }

    #[test]
    fn missing_extension_is_a_mismatch_with_suggestion() {
        let r = check("photo", "image/png", PNG);
        assert_eq!(r.verdict, Verdict::ExtMimeMismatch);
        assert_eq!(r.renamed_filename, "photo.png");
    }

    #[test]
    fn content_contradicting_both_claims() {
        let r = check("cat.png", "image/png", JPEG);
        assert_eq!(r.verdict, Verdict::ContentMismatch);
        assert_eq!(r.sniffed_mime, "image/jpeg");
        assert_eq!(r.suggested_ext, "jpg");
        assert_eq!(r.renamed_filename, "cat.jpg");
    }

    #[test]
    fn pdf_renamed_to_image_is_flagged() {
        let r = check("holiday.jpg", "image/jpeg", PDF);
        assert_eq!(r.verdict, Verdict::ContentMismatch);
        assert_eq!(r.suggested_ext, "pdf");
    }

    #[test]
    fn unrecognised_content_trusts_the_claims() {
        assert_eq!(
            check("notes.txt", "text/plain", b"hello world\n").verdict,
            Verdict::Ok
        );
        assert_eq!(
            check("data.csv", "text/csv", b"a,b\n1,2\n").verdict,
            Verdict::NotMedia
        );
    }

    #[test]
    fn generic_declared_mime_defers_to_extension_and_content() {
        assert_eq!(
            check("cat.png", "application/octet-stream", PNG).verdict,
            Verdict::Ok
        );
        assert_eq!(check("cat.png", "", JPEG).verdict, Verdict::ContentMismatch);
        assert_eq!(check("x.unknownext", "", b"abc").verdict, Verdict::NotMedia);
    }

    #[test]
    fn container_families_are_compatible() {
        // Ogg Opus voice message declared as audio/ogg or video/ogg.
        let ogg = b"OggS\0\x02\0\0\0\0\0\0\0\0\x01\0\0\0\0\0\0\0\0\x01\x13OpusHead\x01\x02";
        assert_eq!(check("v.ogg", "audio/ogg", ogg).verdict, Verdict::Ok);
        assert_eq!(check("v.ogg", "video/ogg", ogg).verdict, Verdict::Ok);
    }

    #[test]
    fn zip_based_office_documents_are_compatible() {
        let zip = b"PK\x03\x04\x14\0\0\0\x08\0";
        let docx = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
        assert_eq!(check("r.docx", docx, zip).verdict, Verdict::NotMedia);
        assert_eq!(
            check("r.zip", "application/zip", zip).verdict,
            Verdict::NotMedia
        );
    }

    #[test]
    fn executables_are_refused_whatever_the_claims() {
        for (name, mime, head) in [
            ("setup.exe", "application/x-msdownload", &b"MZ\x90\0"[..]),
            ("setup.exe", "image/png", PNG),
            ("run.sh", "text/plain", b"echo hi"),
            ("photo.png", "image/png", &PE_HEADER[..]),
            ("photo.png", "image/png", b"\x7fELF\x02\x01\x01"),
            ("notes.txt", "text/plain", b"#!/bin/sh\nrm -rf ~\n"),
            ("photo.jpg.exe", "image/jpeg", JPEG),
            ("a.desktop", "application/x-desktop", b"[Desktop Entry]"),
            ("a.lnk", "", b"\x4c\0\0\0\x01\x14\x02\0"),
        ] {
            assert_eq!(
                check(name, mime, head).verdict,
                Verdict::Executable,
                "{name}"
            );
        }
    }

    #[test]
    fn unknown_extension_cannot_be_vouched_for() {
        // Declared type and contents are a perfect PNG, but ".rdp" / ".iso" are
        // what the OS acts on.
        for name in ["a.reg2", "a.unknownext"] {
            assert_eq!(
                check(name, "image/png", PNG).verdict,
                Verdict::NotMedia,
                "{name}"
            );
        }
        // Known-but-not-media extensions never open silently either.
        for name in ["a.rdp", "a.iso", "a.vhd", "a.img"] {
            assert_ne!(check(name, "image/png", PNG).verdict, Verdict::Ok, "{name}");
        }
    }

    #[test]
    fn rename_never_offers_a_launchable_extension() {
        let r = check("a.png", "application/javascript", b"");
        assert_eq!(r.verdict, Verdict::Executable);
        let r = check("a.png", "application/x-futuristic-script", b"");
        assert!(!["js", "vbs", "exe"].contains(&r.suggested_ext.as_str()));
        assert_eq!(
            check("photo", "text/javascript", b"").verdict,
            Verdict::Executable
        );
    }

    #[test]
    fn only_known_safe_media_opens_silently() {
        assert_eq!(check("a.pdf", "application/pdf", PDF).verdict, Verdict::Ok);
        assert_eq!(check("a.mp4", "video/mp4", b"").verdict, Verdict::Ok);
        assert_eq!(check("a.mp3", "audio/mpeg", b"").verdict, Verdict::Ok);
        assert_eq!(
            check(
                "a.docx",
                "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
                b""
            )
            .verdict,
            Verdict::NotMedia
        );
        assert_eq!(
            check("a.7z", "application/x-7z-compressed", b"").verdict,
            Verdict::NotMedia
        );
        assert_eq!(check("a.xlsx", "", b"").verdict, Verdict::NotMedia);
        assert_eq!(check("a.png", "image/png", PNG).effective_mime, "image/png");
    }

    #[test]
    fn denylist_covers_types_the_os_acts_on() {
        for name in [
            "setup.rdp",
            "profile.mobileconfig",
            "disk.iso",
            "disk.vhdx",
            "bundle.cab",
            "invite.ics",
            "mail.eml",
            "vpn.ovpn",
            "ext.crx",
            "addon.xpi",
            "pkg.flatpakref",
            "x.torrent",
        ] {
            assert_eq!(check(name, "", b"x").verdict, Verdict::Executable, "{name}");
        }
    }

    #[test]
    fn denylist_covers_scriptable_documents() {
        for (name, mime) in [
            ("a.html", "text/html"),
            ("a.svg", "image/svg+xml"),
            ("a.docm", ""),
            ("a.webloc", ""),
            ("a.txt", "text/html"),
        ] {
            assert_eq!(
                check(name, mime, b"<x>").verdict,
                Verdict::Executable,
                "{name}"
            );
        }
    }

    #[test]
    fn container_aliases_do_not_false_alarm() {
        assert_eq!(check("a.heic", "image/heif", b"").verdict, Verdict::Ok);
        assert_eq!(
            check("a.flac", "audio/x-flac", b"fLaC\0\0\0\x22").verdict,
            Verdict::Ok
        );
        assert_eq!(check("a.ico", "image/x-icon", b"").verdict, Verdict::Ok);
    }

    #[test]
    fn weak_magic_does_not_flag_plain_text() {
        assert_eq!(
            check("a.txt", "text/plain", b"MZ-1234 widget\n").verdict,
            Verdict::Ok
        );
        assert_eq!(
            check("a.txt", "text/plain", b"#! not a script\n").verdict,
            Verdict::Ok
        );
        let mut pe = vec![0u8; 0x80];
        pe[0] = b'M';
        pe[1] = b'Z';
        pe[0x3C] = 0x40;
        pe[0x40..0x44].copy_from_slice(b"PE\0\0");
        assert_eq!(
            check("a.png", "image/png", &pe).verdict,
            Verdict::Executable
        );
        assert_eq!(
            check("a.txt", "text/plain", b"#!/bin/sh\n").verdict,
            Verdict::Executable
        );
        assert_eq!(
            check("a.txt", "text/plain", b"#!/usr/bin/env python\n").verdict,
            Verdict::Executable
        );
    }

    #[test]
    fn sanitize_strips_paths_and_dangerous_characters() {
        assert_eq!(sanitize_filename("../../etc/passwd"), "passwd");
        assert_eq!(sanitize_filename("C:\\Users\\x\\a.png"), "a.png");
        assert_eq!(sanitize_filename("a:b*c?.png"), "a_b_c_.png");
        assert_eq!(sanitize_filename("  ..hidden. "), "hidden");
        assert_eq!(sanitize_filename(""), "download");
        assert_eq!(sanitize_filename(".."), "download");
        assert_eq!(sanitize_filename("a\0b\n.txt"), "ab.txt");
    }

    #[test]
    fn sanitize_removes_bidi_override_spoofing() {
        // "photo" + RLO + "gpj.exe" renders as "photoexe.jpg".
        let s = sanitize_filename("photo\u{202E}gpj.exe");
        assert_eq!(s, "photogpj.exe");
        assert_eq!(
            check("photo\u{202E}gpj.exe", "image/jpeg", JPEG).verdict,
            Verdict::Executable
        );
    }

    #[test]
    fn sanitize_handles_reserved_names_and_length() {
        assert_eq!(sanitize_filename("CON.txt"), "_CON.txt");
        assert_eq!(sanitize_filename("nul"), "_nul");
        assert_eq!(sanitize_filename("com1.png"), "_com1.png");
        assert_eq!(sanitize_filename("COM0.png"), "COM0.png");
        let long = format!("{}.png", "é".repeat(200));
        let s = sanitize_filename(&long);
        assert!(s.len() <= MAX_NAME_BYTES);
        assert!(s.ends_with(".png"));
        assert!(std::str::from_utf8(s.as_bytes()).is_ok());
    }

    #[test]
    fn suggested_extension_prefers_conventional_names() {
        assert_eq!(ext_for_mime("image/jpeg"), "jpg");
        assert_eq!(ext_for_mime("text/plain"), "txt");
        assert_eq!(ext_for_mime("application/x-nothing-known"), "");
    }
}

//! Process-wide HTTP proxy configuration.
//!
//! Set once at launch (from the persisted setting, before any client is
//! built and before any runtime threads exist) via [`set_proxy`]; every
//! `reqwest` client builder in the crate then goes through [`apply`].
//!
//! Modes: `System` (reqwest's default: OS/env proxy), `None` (never use a
//! proxy, ignoring OS/env), `Manual` (an explicit `http(s)://[user:pass@]host:port`
//! URL; credentials in the URL are sent as basic auth).
//!
//! LiveKit call signalling (`livekit-api`) builds its own HTTP stack and only
//! reads `HTTPS_PROXY`/`HTTP_PROXY` from the environment, so [`set_proxy`]
//! mirrors the choice into the process environment. WebRTC media (UDP/TURN)
//! cannot traverse an HTTP proxy and is unaffected.

use std::sync::RwLock;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum ProxyMode {
    System,
    None,
    Manual(String),
    /// A Manual proxy was requested but its URL is invalid: fail closed. Every
    /// request is routed to an unresolvable proxy and errors, rather than
    /// quietly going out directly (or through the OS proxy) against the
    /// user's intent.
    Blocked,
}

/// Proxy used for [`ProxyMode::Blocked`]. `.invalid` is reserved (RFC 6761)
/// and never resolves, so every connection through it fails.
const BLOCKED_PROXY: &str = "http://proxy.invalid:9";

static PROXY: RwLock<ProxyMode> = RwLock::new(ProxyMode::System);

/// The proxy environment as it was before [`set_proxy`] rewrote it for a
/// non-System launch setting (so LiveKit sees the chosen proxy). Without it a
/// later live switch back to System would be answered by reqwest's own
/// environment lookup, i.e. by the proxy we just wrote there, until a restart.
/// `None` while the environment is untouched (System launch).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct OriginalEnv {
    https: Option<String>,
    http: Option<String>,
    no_proxy: Option<String>,
}

static ORIGINAL_ENV: RwLock<Option<OriginalEnv>> = RwLock::new(None);

fn first_set(names: &[&str]) -> Option<String> {
    names
        .iter()
        .find_map(|n| std::env::var(n).ok().filter(|v| !v.trim().is_empty()))
}

fn capture_original_env() -> OriginalEnv {
    OriginalEnv {
        https: first_set(&["HTTPS_PROXY", "https_proxy"]),
        http: first_set(&["HTTP_PROXY", "http_proxy"]),
        no_proxy: first_set(&["NO_PROXY", "no_proxy"]),
    }
}

/// System mode over an environment we have overwritten: reqwest's automatic
/// lookup is replaced by the proxies the environment held originally (none ->
/// no proxy, which is what System means on Linux; the OS proxy settings of
/// macOS/Windows are only honoured again after a restart).
fn system_from_original(o: &OriginalEnv, b: reqwest::ClientBuilder) -> reqwest::ClientBuilder {
    let mut b = b.no_proxy();
    let np = || {
        o.no_proxy
            .as_deref()
            .and_then(reqwest::NoProxy::from_string)
    };
    if let Some(Ok(p)) = o.https.as_deref().map(reqwest::Proxy::https) {
        b = b.proxy(p.no_proxy(np()));
    }
    if let Some(Ok(p)) = o.http.as_deref().map(reqwest::Proxy::http) {
        b = b.proxy(p.no_proxy(np()));
    }
    b
}

const ENV_VARS: [&str; 4] = ["HTTPS_PROXY", "HTTP_PROXY", "https_proxy", "http_proxy"];

/// True if `url` is an http/https proxy URL reqwest accepts.
pub fn validate_proxy_url(url: &str) -> bool {
    let url = url.trim();
    let Ok(parsed) = url::Url::parse(url) else {
        return false;
    };
    matches!(parsed.scheme(), "http" | "https")
        && parsed.host_str().is_some_and(|h| !h.is_empty())
        && reqwest::Proxy::all(url).is_ok()
}

/// Pure mode parsing: 0 = System, 1 = None, 2 = Manual. `None` result means
/// the input was invalid (unknown mode, or a bad Manual URL).
fn parse(mode: u8, url: &str) -> Option<ProxyMode> {
    match mode {
        0 => Some(ProxyMode::System),
        1 => Some(ProxyMode::None),
        2 => validate_proxy_url(url).then(|| ProxyMode::Manual(url.trim().to_owned())),
        _ => None,
    }
}

/// What an invalid request resolves to: a bad Manual URL fails closed
/// (the user asked for a proxy, so never go out without one); an unknown
/// mode value falls back to System.
fn fallback(mode: u8) -> ProxyMode {
    if mode == 2 {
        ProxyMode::Blocked
    } else {
        ProxyMode::System
    }
}

/// Store the process-wide proxy setting (0 = system, 1 = none, 2 = manual).
/// Returns false if the mode or Manual URL is invalid: a bad Manual URL then
/// fails closed (all requests error), an unknown mode falls back to System. Call before any client is built and before runtime threads start
/// (it edits the process environment).
pub fn set_proxy(mode: u8, url: &str) -> bool {
    store(mode, url, true)
}

/// Like [`set_proxy`] but safe to call while runtime threads exist: it only
/// updates the setting that every newly built `reqwest` client reads, and
/// leaves the process environment alone (so LiveKit call signalling, which
/// reads the environment, keeps the launch-time proxy until the next
/// launch). Clients that are already built keep their old proxy.
pub fn set_proxy_live(mode: u8, url: &str) -> bool {
    store(mode, url, false)
}

fn store(mode: u8, url: &str, touch_env: bool) -> bool {
    let (parsed, ok) = match parse(mode, url) {
        Some(m) => (m, true),
        None => (fallback(mode), false),
    };
    if touch_env {
        if !matches!(parsed, ProxyMode::System) {
            if let Ok(mut g) = ORIGINAL_ENV.write() {
                g.get_or_insert_with(capture_original_env);
            }
        }
        match &parsed {
            ProxyMode::System => {}
            ProxyMode::None => {
                for v in ENV_VARS {
                    std::env::remove_var(v);
                }
            }
            ProxyMode::Manual(u) => {
                for v in ENV_VARS {
                    std::env::set_var(v, u);
                }
            }
            ProxyMode::Blocked => {
                for v in ENV_VARS {
                    std::env::set_var(v, BLOCKED_PROXY);
                }
            }
        }
    }
    if let Ok(mut g) = PROXY.write() {
        *g = parsed;
    }
    ok
}

pub(crate) fn current() -> ProxyMode {
    PROXY.read().map(|g| g.clone()).unwrap_or(ProxyMode::System)
}

/// True when the user configured an explicit (Manual) proxy, so every
/// connection's peer is that proxy rather than the target.
pub(crate) fn is_manual() -> bool {
    matches!(current(), ProxyMode::Manual(_))
}

fn apply_mode(mode: &ProxyMode, b: reqwest::ClientBuilder) -> reqwest::ClientBuilder {
    match mode {
        ProxyMode::System => match ORIGINAL_ENV.read().ok().and_then(|g| g.clone()) {
            Some(orig) => system_from_original(&orig, b),
            None => b,
        },
        ProxyMode::None => b.no_proxy(),
        ProxyMode::Manual(url) => match reqwest::Proxy::all(url.as_str()) {
            Ok(p) => b.no_proxy().proxy(p),
            // Validated on entry, so unreachable; never fall open.
            Err(_) => apply_mode(&ProxyMode::Blocked, b),
        },
        ProxyMode::Blocked => match reqwest::Proxy::all(BLOCKED_PROXY) {
            Ok(p) => b.no_proxy().proxy(p),
            Err(_) => b,
        },
    }
}

/// Apply the configured proxy to a client builder.
pub(crate) fn apply(b: reqwest::ClientBuilder) -> reqwest::ClientBuilder {
    apply_mode(&current(), b)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_modes() {
        assert_eq!(parse(0, ""), Some(ProxyMode::System));
        assert_eq!(parse(1, ""), Some(ProxyMode::None));
        assert_eq!(
            parse(2, "http://u:p@h:3128"),
            Some(ProxyMode::Manual("http://u:p@h:3128".into()))
        );
        assert_eq!(parse(9, ""), None);
    }

    #[test]
    fn bad_manual_url_fails_closed() {
        assert_eq!(fallback(2), ProxyMode::Blocked);
        assert_eq!(fallback(9), ProxyMode::System);
    }

    #[test]
    fn rejects_bad_urls() {
        assert!(!validate_proxy_url("ftp://h:21"));
        assert!(!validate_proxy_url("socks5://h:1080"));
        assert!(!validate_proxy_url("garbage"));
        assert!(!validate_proxy_url(""));
        assert!(validate_proxy_url("http://u:p@h:3128"));
        assert!(validate_proxy_url("https://proxy.example.com:8080"));
        assert_eq!(parse(2, "socks5://h:1080"), None);
    }

    #[test]
    fn apply_builds() {
        for m in [
            ProxyMode::System,
            ProxyMode::None,
            ProxyMode::Manual("http://u:p@h:3128".into()),
            ProxyMode::Blocked,
        ] {
            assert!(apply_mode(&m, reqwest::Client::builder()).build().is_ok());
        }
    }

    #[test]
    fn system_over_an_overwritten_environment_uses_the_original_proxies() {
        let orig = OriginalEnv {
            https: Some("http://corp.example:3128".into()),
            http: Some("http://corp.example:3128".into()),
            no_proxy: Some("localhost,.internal".into()),
        };
        assert!(system_from_original(&orig, reqwest::Client::builder())
            .build()
            .is_ok());
        // Nothing was set originally: no proxy at all, never the launch one.
        assert!(
            system_from_original(&OriginalEnv::default(), reqwest::Client::builder())
                .build()
                .is_ok()
        );
        // A junk original value is skipped rather than failing every request open.
        let junk = OriginalEnv {
            https: Some("not a url".into()),
            ..Default::default()
        };
        assert!(system_from_original(&junk, reqwest::Client::builder())
            .build()
            .is_ok());
    }

    #[tokio::test]
    async fn system_mode_after_an_overridden_launch_goes_through_the_original_proxy() {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let seen = tokio::spawn(async move {
            let (mut sock, _) = listener.accept().await.unwrap();
            let mut buf = vec![0u8; 2048];
            let n = sock.read(&mut buf).await.unwrap();
            let _ = sock
                .write_all(b"HTTP/1.1 200 OK\r\ncontent-length: 2\r\nconnection: close\r\n\r\nok")
                .await;
            String::from_utf8_lossy(&buf[..n]).into_owned()
        });

        // The launch setting was Manual (environment overwritten), the original
        // environment named this proxy, and the user then switched to System.
        *ORIGINAL_ENV.write().unwrap() = Some(OriginalEnv {
            http: Some(format!("http://{addr}")),
            ..Default::default()
        });
        let client = apply_mode(&ProxyMode::System, reqwest::Client::builder())
            .build()
            .unwrap();
        let resp = client.get("http://target.example.invalid/x").send().await;
        *ORIGINAL_ENV.write().unwrap() = None;

        assert!(resp.is_ok(), "{resp:?}");
        let request = seen.await.unwrap();
        // An absolute-form request line is what a client sends to a proxy.
        assert!(
            request.starts_with("GET http://target.example.invalid/x "),
            "{request}"
        );
    }

    #[test]
    fn live_updates_setting_but_not_environment() {
        let before: Vec<_> = ENV_VARS.iter().map(|v| std::env::var(v).ok()).collect();
        assert!(set_proxy_live(2, "http://u:p@h:3128"));
        assert_eq!(current(), ProxyMode::Manual("http://u:p@h:3128".into()));
        // An invalid URL falls back to System and reports false.
        assert!(!set_proxy_live(2, "garbage"));
        assert_eq!(current(), ProxyMode::Blocked);
        // An unknown mode is not a proxy request: fall back to System.
        assert!(!set_proxy_live(9, ""));
        assert_eq!(current(), ProxyMode::System);
        let after: Vec<_> = ENV_VARS.iter().map(|v| std::env::var(v).ok()).collect();
        assert_eq!(before, after);
    }
}

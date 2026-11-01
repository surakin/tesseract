//! SSRF guard for outbound HTTP requests to arbitrary, user- or
//! attacker-supplied URLs (maps shortlink resolution, client-side URL-preview
//! fetching). The threat is a link that — directly, via DNS, or via a redirect
//! — points at the user's own machine or LAN (router admin pages, local dev
//! servers, cloud metadata endpoints).
//!
//! Two layers:
//! - [`is_disallowed_ip`] / [`host_is_safe`]: pure address classification and
//!   a pre-flight resolve check, used by the manual redirect walks.
//! - [`guarded_client`]: a `reqwest::Client` whose DNS resolver ([`SafeResolver`])
//!   refuses to hand out any disallowed address, so the address that was
//!   checked is the address actually connected to (no DNS-rebinding window).
//!   This holds only for direct connections: behind a proxy the proxy resolves
//!   the name, so that protection is lost (see [`guarded_client`]).
//!   The resolver never sees IP-literal hosts, so callers must still run
//!   [`url_is_fetchable`] on every hop.

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::time::Duration;
use url::Url;

/// True for IPv4 addresses a preview/shortlink fetch must never reach:
/// "this network" (0/8), loopback, RFC1918 private, link-local (incl. the
/// 169.254.169.254 cloud metadata endpoint), CGNAT (100.64/10), IETF protocol
/// assignments (192.0.0/24), documentation (TEST-NET-1/2/3), benchmarking
/// (198.18/15), multicast, reserved (240/4) and broadcast.
pub(crate) fn is_disallowed_v4(v4: Ipv4Addr) -> bool {
    let o = v4.octets();
    let is_this_network = o[0] == 0;
    let is_cgnat = o[0] == 100 && (o[1] & 0b1100_0000) == 0b0100_0000;
    let is_ietf_protocol = o[0] == 192 && o[1] == 0 && o[2] == 0;
    let is_documentation = (o[0] == 192 && o[1] == 0 && o[2] == 2)
        || (o[0] == 198 && o[1] == 51 && o[2] == 100)
        || (o[0] == 203 && o[1] == 0 && o[2] == 113);
    let is_benchmarking = o[0] == 198 && (o[1] & 0xfe) == 18;
    let is_reserved = o[0] >= 240;
    is_this_network
        || v4.is_loopback()
        || v4.is_private()
        || v4.is_link_local()
        || v4.is_multicast()
        || v4.is_broadcast()
        || is_cgnat
        || is_ietf_protocol
        || is_documentation
        || is_benchmarking
        || is_reserved
}

/// Same as `is_disallowed_v4`, plus IPv6 loopback / unspecified / multicast /
/// unique-local (fc00::/7) / link-local (fe80::/10) / deprecated site-local
/// (fec0::/10) / documentation (2001:db8::/32). IPv6 forms that embed an IPv4
/// address — IPv4-mapped (`::ffff:a.b.c.d`), NAT64 (`64:ff9b::a.b.c.d`) and
/// 6to4 (`2002:aabb:ccdd::`) — are unwrapped and checked as IPv4 so they
/// can't smuggle a disallowed address past an IPv6-only check.
pub(crate) fn is_disallowed_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => is_disallowed_v4(v4),
        IpAddr::V6(v6) => {
            if let Some(embedded) = embedded_v4(v6) {
                return is_disallowed_v4(embedded);
            }
            let s = v6.segments();
            let is_link_local = (s[0] & 0xffc0) == 0xfe80;
            let is_site_local = (s[0] & 0xffc0) == 0xfec0;
            let is_documentation = s[0] == 0x2001 && s[1] == 0x0db8;
            v6.is_loopback()
                || v6.is_unspecified()
                || v6.is_multicast()
                || v6.is_unique_local()
                || is_link_local
                || is_site_local
                || is_documentation
        }
    }
}

/// The IPv4 address embedded in an IPv4-mapped, NAT64 well-known-prefix or
/// 6to4 IPv6 address, if any.
fn embedded_v4(v6: Ipv6Addr) -> Option<Ipv4Addr> {
    if let Some(mapped) = v6.to_ipv4_mapped() {
        return Some(mapped);
    }
    let s = v6.segments();
    // NAT64 well-known prefix 64:ff9b::/96 — last 32 bits are the IPv4 address.
    if s[0] == 0x0064 && s[1] == 0xff9b && s[2..6].iter().all(|&x| x == 0) {
        let o = v6.octets();
        return Some(Ipv4Addr::new(o[12], o[13], o[14], o[15]));
    }
    // 6to4 2002::/16 — bits 16..48 are the IPv4 address.
    if s[0] == 0x2002 {
        let o = v6.octets();
        return Some(Ipv4Addr::new(o[2], o[3], o[4], o[5]));
    }
    None
}

/// `localhost` and the names under it always mean this machine (RFC 6761).
fn is_loopback_name(name: &str) -> bool {
    let name = name.trim_end_matches('.').to_ascii_lowercase();
    name == "localhost" || name.ends_with(".localhost")
}

/// Names that point at a machine on the local network rather than the public
/// internet: a single label (`router`, `intranet`), or a name under a
/// reserved internal suffix. A name that resolves to a private address is
/// refused by the resolver check, so this only matters behind the user's
/// Manual proxy, where the proxy resolves the name and the resolver-side check
/// never sees it. It is not applied otherwise, so a service on a public
/// address that happens to be called `box.lan` stays reachable.
fn is_internal_name(name: &str) -> bool {
    let name = name.trim_end_matches('.').to_ascii_lowercase();
    if !name.contains('.') {
        return true;
    }
    [".local", ".internal", ".lan", ".localdomain", ".home.arpa"]
        .iter()
        .any(|suffix| name.ends_with(suffix))
}

/// Pre-flight check for one hop of a guarded fetch: http/https only, and an
/// IP-literal host must not be disallowed. A hostname that is really an IP in
/// another spelling (e.g. `127.0.0.1.`) is checked as an IP; `localhost` is
/// always refused, and behind a Manual proxy so is any other internal name
/// ([`is_internal_name`]). Other names are vetted again at connect time by
/// [`SafeResolver`] (or by [`host_is_safe`] for callers not using
/// [`guarded_client`]).
pub(crate) fn url_is_fetchable(url: &Url) -> bool {
    url_is_fetchable_with(url, crate::net_proxy::is_manual())
}

fn url_is_fetchable_with(url: &Url, manual_proxy: bool) -> bool {
    if url.scheme() != "http" && url.scheme() != "https" {
        return false;
    }
    match url.host() {
        Some(url::Host::Ipv4(v4)) => !is_disallowed_v4(v4),
        Some(url::Host::Ipv6(v6)) => !is_disallowed_ip(IpAddr::V6(v6)),
        Some(url::Host::Domain(name)) => match name.trim_end_matches('.').parse::<IpAddr>() {
            Ok(ip) => !is_disallowed_ip(ip),
            Err(_) => !is_loopback_name(name) && !(manual_proxy && is_internal_name(name)),
        },
        None => false,
    }
}

/// Resolve `url`'s host and reject it (SSRF guard) if ANY resolved address
/// is disallowed (see [`is_disallowed_ip`]). Run before every hop of the
/// maps-shortlink manual redirect walk — a malicious or compromised redirect
/// target (or a plain DNS entry pointing at, say, a cloud metadata endpoint)
/// must not be followed. This does not fully close a live DNS-rebinding
/// attack (the resolution reqwest performs at actual connect time is separate
/// from this check), but it blocks the realistic case of a redirect/DNS
/// record that statically points at an internal address — proportionate for
/// a best-effort, opt-in feature whose caller already silently falls back to
/// plain text on any failure. [`guarded_client`] closes the rebinding gap.
pub(crate) async fn host_is_safe(url: &Url) -> bool {
    let Some(host) = url.host_str() else {
        return false;
    };
    let port = url.port_or_known_default().unwrap_or(443);
    match tokio::net::lookup_host((host, port)).await {
        Ok(addrs) => {
            let mut any = false;
            for addr in addrs {
                any = true;
                if is_disallowed_ip(addr.ip()) {
                    return false;
                }
            }
            any
        }
        Err(_) => false,
    }
}

/// Filter a DNS answer: `None` (reject the whole lookup) if it is empty or
/// ANY address is disallowed. Rejecting the whole answer rather than just
/// dropping bad entries keeps a mixed public/private record from being used
/// to probe the LAN on a retry.
fn vet_addrs(addrs: Vec<SocketAddr>) -> Option<Vec<SocketAddr>> {
    if addrs.is_empty() || addrs.iter().any(|a| is_disallowed_ip(a.ip())) {
        None
    } else {
        Some(addrs)
    }
}

/// `reqwest` DNS resolver that refuses to resolve a hostname to any
/// disallowed address. Because reqwest connects to exactly the addresses this
/// returns, a host that re-resolves to an internal address between a check
/// and the connect (DNS rebinding) can't slip through.
pub(crate) struct SafeResolver;

impl reqwest::dns::Resolve for SafeResolver {
    fn resolve(&self, name: reqwest::dns::Name) -> reqwest::dns::Resolving {
        let host = name.as_str().to_owned();
        Box::pin(async move {
            let addrs: Vec<SocketAddr> =
                tokio::net::lookup_host((host.as_str(), 0)).await?.collect();
            match vet_addrs(addrs) {
                Some(ok) => Ok(Box::new(ok.into_iter()) as reqwest::dns::Addrs),
                None => Err(format!("{host}: resolves to a disallowed address").into()),
            }
        })
    }
}

/// Maximum redirect hops a [`guarded_client`] caller should walk manually.
pub(crate) const GUARDED_MAX_REDIRECTS: u8 = 5;

/// A `reqwest::Client` for fetching arbitrary third-party URLs:
/// [`SafeResolver`] for DNS, the configured proxy (see [`crate::net_proxy`]),
/// no automatic redirects (callers walk them
/// with [`url_is_fetchable`] on each hop), and tight timeouts.
///
/// Tradeoff: when a proxy is in use (Manual, or a System/env proxy) the proxy
/// resolves the target name, so [`SafeResolver`] never runs and DNS-rebinding
/// protection is lost. The URL-literal checks ([`url_is_fetchable`]) still
/// apply to every hop. The peer-address check in [`guarded_get`] is skipped
/// only for a Manual proxy (the peer is then the proxy, not the target); an
/// OS/env proxy on a private address makes guarded fetches fail closed.
pub(crate) fn guarded_client(total_timeout: Duration) -> Option<reqwest::Client> {
    crate::net_proxy::apply(reqwest::Client::builder())
        .user_agent(crate::oauth::build_user_agent())
        .dns_resolver(std::sync::Arc::new(SafeResolver))
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(10))
        .timeout(total_timeout)
        .build()
        .ok()
}

/// GET `url` through the guarded client, walking redirects manually so
/// every hop's scheme and IP-literal host are vetted (the resolver
/// vets hostnames) and the peer address is re-checked. Returns the
/// final successful response and its URL.
pub(crate) async fn guarded_get(
    http: &reqwest::Client,
    url: Url,
) -> Option<(reqwest::Response, Url)> {
    guarded_walk(http, url, GUARDED_MAX_REDIRECTS, true).await
}

/// Like [`guarded_get`], but where the chain ends is enough: the final
/// response may be any status, and `max_redirects` hops are followed. For
/// callers that read the destination URL (a maps shortlink whose target page
/// answers 403 still names the coordinates) rather than its body.
pub(crate) async fn guarded_final_url(
    http: &reqwest::Client,
    url: Url,
    max_redirects: u8,
) -> Option<Url> {
    guarded_walk(http, url, max_redirects, false)
        .await
        .map(|(_, u)| u)
}

async fn guarded_walk(
    http: &reqwest::Client,
    url: Url,
    max_redirects: u8,
    require_success: bool,
) -> Option<(reqwest::Response, Url)> {
    let mut current = url;
    for _ in 0..=max_redirects {
        if !url_is_fetchable(&current) {
            return None;
        }
        let resp = http.get(current.clone()).send().await.ok()?;
        // Behind the user's Manual proxy the peer is that proxy, not the
        // target, so the check is skipped. Under System it still runs: an
        // OS/env proxy on a private address fails closed (the caller falls
        // back to the homeserver) rather than weakening the guard.
        if !crate::net_proxy::is_manual()
            && resp.remote_addr().is_some_and(|a| is_disallowed_ip(a.ip()))
        {
            return None;
        }
        if resp.status().is_redirection() {
            let location = resp
                .headers()
                .get(reqwest::header::LOCATION)?
                .to_str()
                .ok()?;
            current = current.join(location).ok()?;
            continue;
        }
        if require_success && !resp.status().is_success() {
            return None;
        }
        return Some((resp, current));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v6(s: &str) -> IpAddr {
        IpAddr::V6(s.parse().unwrap())
    }

    #[test]
    fn ssrf_guard_rejects_loopback() {
        assert!(is_disallowed_v4(Ipv4Addr::new(127, 0, 0, 1)));
        assert!(is_disallowed_ip(IpAddr::V6(Ipv6Addr::LOCALHOST)));
    }

    #[test]
    fn ssrf_guard_rejects_rfc1918_private_ranges() {
        assert!(is_disallowed_v4(Ipv4Addr::new(10, 0, 0, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(172, 16, 0, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(192, 168, 1, 1)));
    }

    #[test]
    fn ssrf_guard_rejects_link_local_and_metadata_ip() {
        // 169.254.169.254 is the AWS/GCP/Azure cloud metadata endpoint —
        // falls under the 169.254.0.0/16 link-local block.
        assert!(is_disallowed_v4(Ipv4Addr::new(169, 254, 169, 254)));
    }

    #[test]
    fn ssrf_guard_rejects_cgnat_range() {
        assert!(is_disallowed_v4(Ipv4Addr::new(100, 64, 0, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(100, 127, 255, 254)));
        // Just outside the /10 CGNAT block on either side — must NOT be flagged.
        assert!(!is_disallowed_v4(Ipv4Addr::new(100, 63, 255, 255)));
        assert!(!is_disallowed_v4(Ipv4Addr::new(100, 128, 0, 0)));
    }

    #[test]
    fn ssrf_guard_rejects_unspecified_and_broadcast() {
        assert!(is_disallowed_v4(Ipv4Addr::UNSPECIFIED));
        assert!(is_disallowed_v4(Ipv4Addr::new(255, 255, 255, 255)));
    }

    #[test]
    fn ssrf_guard_rejects_this_network_reserved_and_special_v4() {
        assert!(is_disallowed_v4(Ipv4Addr::new(0, 1, 2, 3)));
        assert!(is_disallowed_v4(Ipv4Addr::new(240, 0, 0, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(192, 0, 0, 8)));
        assert!(is_disallowed_v4(Ipv4Addr::new(192, 0, 2, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(198, 51, 100, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(203, 0, 113, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(198, 18, 0, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(198, 19, 255, 255)));
        assert!(!is_disallowed_v4(Ipv4Addr::new(198, 20, 0, 1)));
        assert!(is_disallowed_v4(Ipv4Addr::new(224, 0, 0, 1)));
    }

    #[test]
    fn ssrf_guard_rejects_ipv6_unique_local() {
        assert!(is_disallowed_ip(IpAddr::V6(Ipv6Addr::new(
            0xfd00, 0, 0, 0, 0, 0, 0, 1
        ))));
    }

    #[test]
    fn ssrf_guard_rejects_ipv6_link_local_site_local_and_documentation() {
        assert!(is_disallowed_ip(v6("fe80::1")));
        assert!(is_disallowed_ip(v6("febf::1")));
        assert!(is_disallowed_ip(v6("fec0::1")));
        assert!(is_disallowed_ip(v6("2001:db8::1")));
        assert!(is_disallowed_ip(v6("ff02::1")));
        assert!(is_disallowed_ip(v6("::")));
    }

    #[test]
    fn ssrf_guard_unwraps_ipv4_mapped_ipv6_before_checking() {
        // ::ffff:169.254.169.254 must not slip past an IPv6-only check.
        let mapped = Ipv6Addr::new(0, 0, 0, 0, 0, 0xffff, 0xa9fe, 0xa9fe);
        assert!(is_disallowed_ip(IpAddr::V6(mapped)));
    }

    #[test]
    fn ssrf_guard_unwraps_nat64_and_6to4() {
        assert!(is_disallowed_ip(v6("64:ff9b::127.0.0.1")));
        assert!(is_disallowed_ip(v6("64:ff9b::c0a8:0101"))); // 192.168.1.1
        assert!(!is_disallowed_ip(v6("64:ff9b::808:808"))); // 8.8.8.8
        assert!(is_disallowed_ip(v6("2002:0a00:0001::1"))); // 10.0.0.1
        assert!(!is_disallowed_ip(v6("2002:0808:0808::1"))); // 8.8.8.8
    }

    #[test]
    fn ssrf_guard_allows_ordinary_public_addresses() {
        assert!(!is_disallowed_v4(Ipv4Addr::new(8, 8, 8, 8)));
        assert!(!is_disallowed_ip(IpAddr::V6(Ipv6Addr::new(
            0x2001, 0x4860, 0x4860, 0, 0, 0, 0, 0x8888
        ))));
    }

    #[test]
    fn url_is_fetchable_checks_scheme_and_ip_literals() {
        let ok = |s: &str| url_is_fetchable(&Url::parse(s).unwrap());
        assert!(ok("https://example.com/a"));
        assert!(ok("http://8.8.8.8/"));
        assert!(!ok("http://127.0.0.1/"));
        assert!(!ok("http://192.168.1.1/admin"));
        assert!(!ok("http://[::1]:8080/"));
        assert!(!ok("http://[::ffff:10.0.0.1]/"));
        assert!(!ok("ftp://example.com/"));
        assert!(!ok("file:///etc/passwd"));
        // url normalises decimal/hex IPv4 forms to Host::Ipv4.
        assert!(!ok("http://2130706433/"));
        assert!(!ok("http://0x7f.1/"));
    }

    #[test]
    fn url_is_fetchable_always_refuses_loopback_names_and_ip_spellings() {
        for manual in [false, true] {
            let ok = |s: &str| url_is_fetchable_with(&Url::parse(s).unwrap(), manual);
            for host in [
                "localhost",
                "localhost.",
                "LOCALHOST.",
                "a.localhost",
                "127.0.0.1.",
                "169.254.169.254.",
            ] {
                assert!(
                    !ok(&format!("https://{host}/x")),
                    "{host} must be refused (manual={manual})"
                );
            }
            assert!(ok("https://example.com/"));
            assert!(ok("https://localhost.example.com/"));
            assert!(ok("https://notlocal.example/"));
        }
    }

    #[test]
    fn url_is_fetchable_refuses_internal_names_only_behind_a_manual_proxy() {
        let behind_proxy = |s: &str| url_is_fetchable_with(&Url::parse(s).unwrap(), true);
        let direct = |s: &str| url_is_fetchable_with(&Url::parse(s).unwrap(), false);
        for host in [
            "printer.local",
            "nas.internal",
            "router.lan",
            "intranet",
            "box.localdomain",
            "gw.home.arpa",
        ] {
            let url = format!("https://{host}/x");
            assert!(!behind_proxy(&url), "{host} must be refused behind a proxy");
            // Direct: the resolver check vets where the name points, so a
            // name on a public address stays reachable.
            assert!(
                direct(&url),
                "{host} is left to the resolver check when direct"
            );
        }
        assert!(behind_proxy("https://example.local.org/"));
    }

    #[test]
    fn vet_addrs_rejects_empty_and_any_private_entry() {
        let public: SocketAddr = "93.184.215.14:0".parse().unwrap();
        let private: SocketAddr = "10.0.0.5:0".parse().unwrap();
        assert!(vet_addrs(vec![]).is_none());
        assert!(vet_addrs(vec![public, private]).is_none());
        assert_eq!(vet_addrs(vec![public]), Some(vec![public]));
    }

    #[tokio::test]
    async fn safe_resolver_rejects_localhost() {
        use reqwest::dns::Resolve;
        let name: reqwest::dns::Name = "localhost".parse().unwrap();
        assert!(SafeResolver.resolve(name).await.is_err());
    }
}

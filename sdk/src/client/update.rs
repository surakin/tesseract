/// GitHub Releases update checker.
///
/// Fetches the latest release from the GitHub API and compares it against the
/// running version. Returns `Some((version, url))` when a strictly newer
/// release is available; `None` on any error or when already up-to-date.

fn parse_semver(s: &str) -> (u64, u64, u64) {
    let s = s.strip_prefix('v').unwrap_or(s);
    let mut parts = s.splitn(3, '.').map(|p| p.parse::<u64>().unwrap_or(0));
    let major = parts.next().unwrap_or(0);
    let minor = parts.next().unwrap_or(0);
    let patch = parts.next().unwrap_or(0);
    (major, minor, patch)
}

pub async fn check_github_release(
    http: &reqwest::Client,
    repo: &str,
    current: &str,
) -> Option<(String, String)> {
    let url = format!("https://api.github.com/repos/{}/releases/latest", repo);
    let response = http
        .get(&url)
        .header("User-Agent", format!("Tesseract/{}", current))
        .header("Accept", "application/vnd.github.v3+json")
        .timeout(std::time::Duration::from_secs(10))
        .send()
        .await
        .ok()?;

    if !response.status().is_success() {
        return None;
    }

    let body: serde_json::Value = response.json().await.ok()?;
    let tag = body["tag_name"].as_str()?;
    let html_url = body["html_url"].as_str()?;

    let tag_version = tag.strip_prefix('v').unwrap_or(tag);
    if parse_semver(tag_version) > parse_semver(current) {
        Some((tag_version.to_owned(), html_url.to_owned()))
    } else {
        None
    }
}

/// Strips the trailing `-pkgrel` from an AUR `Version` field (format
/// `pkgver-pkgrel`, e.g. `"0.8.20-1"`) to recover the plain pkgver.
fn strip_pkgrel(version: &str) -> &str {
    version.rsplit_once('-').map_or(version, |(pkgver, _)| pkgver)
}

pub async fn check_aur_package(
    http: &reqwest::Client,
    pkgname: &str,
    current: &str,
) -> Option<(String, String)> {
    let url = format!("https://aur.archlinux.org/rpc/v5/info?arg[]={}", pkgname);
    let response = http
        .get(&url)
        .header("User-Agent", format!("Tesseract/{}", current))
        .timeout(std::time::Duration::from_secs(10))
        .send()
        .await
        .ok()?;

    if !response.status().is_success() {
        return None;
    }

    let body: serde_json::Value = response.json().await.ok()?;
    let version = body["results"][0]["Version"].as_str()?;
    let pkgver = strip_pkgrel(version);
    let page_url = format!("https://aur.archlinux.org/packages/{}", pkgname);

    if parse_semver(pkgver) > parse_semver(current) {
        Some((pkgver.to_owned(), page_url))
    } else {
        None
    }
}

#[cfg(not(test))]
impl super::ClientFfi {
    pub fn check_for_update(&self, repo: &str, current_version: &str) -> crate::ffi::UpdateResult {
        let http = self.http_client.clone();
        let repo = repo.to_owned();
        let current = current_version.to_owned();
        match self
            .rt
            .block_on(check_github_release(&http, &repo, &current))
        {
            Some((version, url)) => crate::ffi::UpdateResult {
                has_update: true,
                version,
                url,
            },
            None => crate::ffi::UpdateResult {
                has_update: false,
                version: String::new(),
                url: String::new(),
            },
        }
    }

    pub fn check_for_aur_update(
        &self,
        pkgname: &str,
        current_version: &str,
    ) -> crate::ffi::UpdateResult {
        let http = self.http_client.clone();
        let pkgname = pkgname.to_owned();
        let current = current_version.to_owned();
        match self
            .rt
            .block_on(check_aur_package(&http, &pkgname, &current))
        {
            Some((version, url)) => crate::ffi::UpdateResult {
                has_update: true,
                version,
                url,
            },
            None => crate::ffi::UpdateResult {
                has_update: false,
                version: String::new(),
                url: String::new(),
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn semver_parsing() {
        assert_eq!(parse_semver("1.2.3"), (1, 2, 3));
        assert_eq!(parse_semver("v1.2.3"), (1, 2, 3));
        assert_eq!(parse_semver("0.8.5"), (0, 8, 5));
        assert_eq!(parse_semver("bad"), (0, 0, 0));
    }

    #[test]
    fn semver_comparison() {
        assert!(parse_semver("0.9.0") > parse_semver("0.8.5"));
        assert!(parse_semver("1.0.0") > parse_semver("0.99.99"));
        assert_eq!(parse_semver("0.8.5"), parse_semver("0.8.5"));
        assert!(parse_semver("0.8.4") < parse_semver("0.8.5"));
    }

    #[test]
    fn pkgrel_stripping() {
        assert_eq!(strip_pkgrel("0.8.20-1"), "0.8.20");
        assert_eq!(strip_pkgrel("0.8.20-2"), "0.8.20");
        assert_eq!(strip_pkgrel("0.8.20"), "0.8.20");
    }
}

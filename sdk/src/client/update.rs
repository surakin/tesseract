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
}

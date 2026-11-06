//! File and directory creation that is owner-only from the first byte.
//!
//! Decrypted history, media and exports must not be readable by other local
//! users, whatever the process umask is. On Unix the mode is applied at
//! creation, so there is no window where the file is wider; elsewhere the
//! platform's per-user profile ACLs apply and these are the plain calls.

use std::{fs::File, io, path::Path};

/// Creates (or truncates) `path` for writing; mode 0600 on Unix.
pub(crate) fn create_file(path: &Path) -> io::Result<File> {
    let mut opts = std::fs::OpenOptions::new();
    opts.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600);
    }
    let file = opts.open(path)?;
    // `mode` only applies to a new file; tighten one that already existed.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = file.set_permissions(std::fs::Permissions::from_mode(0o600));
    }
    Ok(file)
}

/// Async [`std::fs::write`] with [`create_file`]'s permissions.
pub(crate) async fn write(path: &Path, bytes: &[u8]) -> io::Result<()> {
    use tokio::io::AsyncWriteExt;
    let mut file = tokio::fs::File::from_std(create_file(path)?);
    file.write_all(bytes).await?;
    file.flush().await
}

/// `create_dir_all`, with directories it creates being 0700 on Unix.
pub(crate) fn create_dir_all(path: &Path) -> io::Result<()> {
    let mut builder = std::fs::DirBuilder::new();
    builder.recursive(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        builder.mode(0o700);
    }
    builder.create(path)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    /// A fresh scratch directory, removed on drop.
    struct Scratch(std::path::PathBuf);
    impl Scratch {
        fn new(tag: &str) -> Self {
            let p = std::env::temp_dir().join(format!(
                "tesseract-private-fs-{tag}-{}-{:?}",
                std::process::id(),
                std::thread::current().id()
            ));
            let _ = std::fs::remove_dir_all(&p);
            std::fs::create_dir_all(&p).unwrap();
            Self(p)
        }
        fn path(&self) -> &Path {
            &self.0
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn mode(p: &Path) -> u32 {
        std::fs::metadata(p).unwrap().permissions().mode() & 0o777
    }

    #[test]
    fn files_and_dirs_are_owner_only() {
        let base = Scratch::new("t");
        let dir = base.path().join("a/b");
        create_dir_all(&dir).unwrap();
        assert_eq!(mode(&dir), 0o700);
        assert_eq!(mode(&base.path().join("a")), 0o700);
        let f = dir.join("x");
        drop(create_file(&f).unwrap());
        assert_eq!(mode(&f), 0o600);
    }

    #[test]
    fn an_existing_wide_file_is_tightened_and_truncated() {
        let base = Scratch::new("t");
        let f = base.path().join("x");
        std::fs::write(&f, b"old contents").unwrap();
        std::fs::set_permissions(&f, std::fs::Permissions::from_mode(0o644)).unwrap();
        drop(create_file(&f).unwrap());
        assert_eq!(mode(&f), 0o600);
        assert_eq!(std::fs::metadata(&f).unwrap().len(), 0);
    }

    #[tokio::test]
    async fn async_write_is_owner_only() {
        let base = Scratch::new("t");
        let f = base.path().join("img");
        write(&f, b"data").await.unwrap();
        assert_eq!(mode(&f), 0o600);
        assert_eq!(std::fs::read(&f).unwrap(), b"data");
    }
}

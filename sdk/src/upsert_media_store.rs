//! Workaround for a matrix-sdk send-queue wedge: `replace_media_key` fails
//! with `UNIQUE constraint failed: media.uri, media.format` when the
//! destination `mxc://` already exists (a homeserver deduplicating
//! byte-identical uploads returns the same URI twice), and the send queue
//! then retries the failed dependent request forever. This wrapper gives
//! `replace_media_key` upsert semantics; everything else delegates.

use async_trait::async_trait;
use matrix_sdk::{
    cross_process_lock::CrossProcessLockConfig, SqliteCryptoStore, SqliteEventCacheStore,
    SqliteMediaStore, SqliteStateStore, SqliteStoreConfig,
};
use matrix_sdk_base::{
    cross_process_lock::CrossProcessLockGeneration,
    media::{
        store::{IgnoreMediaRetentionPolicy, MediaRetentionPolicy, MediaStore},
        MediaRequestParameters, UniqueKey,
    },
    ruma::MxcUri,
    store::StoreConfig,
};

type InnerError = <SqliteMediaStore as MediaStore>::Error;

#[derive(Debug, Clone)]
pub(crate) struct UpsertMediaStore(SqliteMediaStore);

#[async_trait]
impl MediaStore for UpsertMediaStore {
    type Error = InnerError;

    async fn try_take_leased_lock(
        &self,
        lease_duration_ms: u32,
        key: &str,
        holder: &str,
    ) -> Result<Option<CrossProcessLockGeneration>, Self::Error> {
        self.0.try_take_leased_lock(lease_duration_ms, key, holder).await
    }

    async fn add_media_content(
        &self,
        request: &MediaRequestParameters,
        content: Vec<u8>,
        ignore_policy: IgnoreMediaRetentionPolicy,
    ) -> Result<(), Self::Error> {
        self.0.add_media_content(request, content, ignore_policy).await
    }

    async fn replace_media_key(
        &self,
        from: &MediaRequestParameters,
        to: &MediaRequestParameters,
    ) -> Result<(), Self::Error> {
        // Identical content already lives under `to` (same mxc URI), so the
        // old row is redundant; clear it so the rename cannot collide.
        if from.unique_key() != to.unique_key() {
            self.0.remove_media_content(to).await?;
        }
        self.0.replace_media_key(from, to).await
    }

    async fn get_media_content(
        &self,
        request: &MediaRequestParameters,
    ) -> Result<Option<Vec<u8>>, Self::Error> {
        self.0.get_media_content(request).await
    }

    async fn remove_media_content(
        &self,
        request: &MediaRequestParameters,
    ) -> Result<(), Self::Error> {
        self.0.remove_media_content(request).await
    }

    async fn remove_media_content_for_uri(&self, uri: &MxcUri) -> Result<(), Self::Error> {
        self.0.remove_media_content_for_uri(uri).await
    }

    async fn set_media_retention_policy(
        &self,
        policy: MediaRetentionPolicy,
    ) -> Result<(), Self::Error> {
        self.0.set_media_retention_policy(policy).await
    }

    fn media_retention_policy(&self) -> MediaRetentionPolicy {
        self.0.media_retention_policy()
    }

    async fn set_ignore_media_retention_policy(
        &self,
        request: &MediaRequestParameters,
        ignore_policy: IgnoreMediaRetentionPolicy,
    ) -> Result<(), Self::Error> {
        self.0.set_ignore_media_retention_policy(request, ignore_policy).await
    }

    async fn clean(&self) -> Result<(), Self::Error> {
        self.0.clean().await
    }

    async fn close(&self) -> Result<(), Self::Error> {
        self.0.close().await
    }

    async fn reopen(&self) -> Result<(), Self::Error> {
        self.0.reopen().await
    }

    async fn optimize(&self) -> Result<(), Self::Error> {
        self.0.optimize().await
    }

    async fn get_size(&self) -> Result<Option<usize>, Self::Error> {
        self.0.get_size().await
    }
}

/// Mirrors matrix-sdk's own `sqlite_store_with_config_and_cache_path`
/// (no cache path) store assembly, with the media store wrapped.
pub(crate) async fn build_store_config(
    config: SqliteStoreConfig,
    cross_process: CrossProcessLockConfig,
) -> anyhow::Result<StoreConfig> {
    let (state, event_cache, media, crypto) = futures_util::try_join!(
        async { SqliteStateStore::open_with_config(&config).await.map_err(anyhow::Error::from) },
        async {
            SqliteEventCacheStore::open_with_config(&config).await.map_err(anyhow::Error::from)
        },
        async { SqliteMediaStore::open_with_config(&config).await.map_err(anyhow::Error::from) },
        async { SqliteCryptoStore::open_with_config(&config).await.map_err(anyhow::Error::from) },
    )?;
    Ok(StoreConfig::new(cross_process)
        .state_store(state)
        .event_cache_store(event_cache)
        .media_store(UpsertMediaStore(media))
        .crypto_store(crypto))
}

#[cfg(test)]
mod tests {
    use super::*;
    use matrix_sdk::media::MediaFormat;
    use matrix_sdk_base::ruma::{events::room::MediaSource, owned_mxc_uri};

    fn req(uri: &str) -> MediaRequestParameters {
        MediaRequestParameters {
            source: MediaSource::Plain(matrix_sdk_base::ruma::OwnedMxcUri::from(uri)),
            format: MediaFormat::File,
        }
    }

    async fn open(label: &str) -> SqliteMediaStore {
        let dir = std::env::temp_dir().join(format!(
            "tesseract-upsert-media-{label}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        SqliteMediaStore::open_with_config(&SqliteStoreConfig::new(&dir)).await.unwrap()
    }

    async fn rename_twice(store: &impl MediaStore) -> Result<(), String> {
        let remote = req(owned_mxc_uri!("mxc://x.org/same").as_str());
        for local in ["mxc://local/1", "mxc://local/2"] {
            let from = req(local);
            store
                .add_media_content(&from, b"bytes".to_vec(), IgnoreMediaRetentionPolicy::Yes)
                .await
                .map_err(|e| format!("{e:?}"))?;
            store.replace_media_key(&from, &remote).await.map_err(|e| format!("{e:?}"))?;
        }
        Ok(())
    }

    #[tokio::test]
    async fn bare_sqlite_store_collides_on_duplicate_destination() {
        let store = open("bare").await;
        assert!(rename_twice(&store).await.is_err());
    }

    #[tokio::test]
    async fn wrapper_allows_duplicate_destination() {
        let store = UpsertMediaStore(open("wrapped").await);
        rename_twice(&store).await.unwrap();
        let got = store.get_media_content(&req("mxc://x.org/same")).await.unwrap();
        assert_eq!(got.as_deref(), Some(&b"bytes"[..]));
        assert!(store.get_media_content(&req("mxc://local/2")).await.unwrap().is_none());
    }
}

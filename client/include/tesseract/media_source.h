#pragma once
#include <memory>
#include <string>

namespace tesseract {

/// Abstract representation of a Matrix media source — either a plain mxc://
/// URI or an encrypted attachment.  Use shared_ptr<const MediaSource> for
/// copyable ownership; nullptr means "no source present".
///
/// Call mxc_url() to get the ciphertext-or-plaintext mxc:// URI (e.g. for
/// displaying a thumbnail or constructing a download URL).
/// Call fetch_token() to get the opaque token for Client::fetch_source_bytes:
///   - plain  → same as mxc_url()
///   - encrypted → full JSON blob understood by the Rust fetch_source_bytes
class MediaSource
{
public:
    virtual ~MediaSource() = default;

    virtual bool is_encrypted() const noexcept = 0;

    /// The mxc:// URI of the media (plaintext or ciphertext location).
    virtual const std::string& mxc_url() const noexcept = 0;

    /// Token to pass to Client::fetch_source_bytes.
    /// Plain sources: same as mxc_url().
    /// Encrypted sources: the JSON blob understood by fetch_source_bytes.
    virtual const std::string& fetch_token() const noexcept = 0;

    /// Factory: plain mxc:// source.
    static std::shared_ptr<MediaSource> plain(std::string url);

    /// Factory: encrypted source.  `url` is the ciphertext mxc://, `json` is
    /// the full JSON MediaSource blob (as produced by the Rust bridge).
    static std::shared_ptr<MediaSource> encrypted(std::string url,
                                                   std::string json);
};

/// Shared ownership of an immutable MediaSource. nullptr = absent source.
using MediaSourceRef = std::shared_ptr<const MediaSource>;

} // namespace tesseract

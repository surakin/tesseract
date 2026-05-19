#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tesseract
{

/// Open (or create) the local waveform SQLite store at `db_path`.
/// Idempotent — only the first call takes effect. Call once at app startup
/// before the first voice row is displayed.
void init_waveform_cache(const std::string& db_path);

/// Return the cached waveform for `mxc_uri`, or an empty vector on a miss.
std::vector<std::uint16_t> load_voice_waveform(const std::string& mxc_uri);

/// Persist `waveform` for `mxc_uri`. Evicts the oldest entries beyond 2000.
void store_voice_waveform(const std::string&              mxc_uri,
                          const std::vector<std::uint16_t>& waveform);

/// Remove the cached waveform for `mxc_uri`.
void evict_voice_waveform(const std::string& mxc_uri);

/// Decode an Ogg/Opus buffer and compute MSC1767 waveform samples
/// (0..=1024, up to 200 values). Returns an empty vector on invalid input.
std::vector<std::uint16_t>
compute_waveform_from_ogg(const std::vector<std::uint8_t>& bytes);

} // namespace tesseract

#pragma once
#include <string>

namespace tk::gst {

// Call once after QApplication is constructed, before MainWindow.
// Reads the hardware-decoder capability cache from cache_dir/gst_hw_probe.dat.
// If the cache is absent or older than 7 days, probes each known hardware
// decoder by attempting a GST_STATE_READY transition (with GStreamer debug
// suppressed so no noise appears on the first run), then saves the result.
// Broken decoders are demoted to GST_RANK_NONE so QMediaPlayer's auto-plugger
// never attempts them — eliminating per-play hardware probe errors.
void apply_hw_decoder_cache(const std::string& cache_dir);

} // namespace tk::gst

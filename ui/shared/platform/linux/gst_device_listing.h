#pragma once

#include "tk/device_listing.h"

#include <string>
#include <vector>

namespace tk
{

// GStreamer-backed device enumeration for the capture-device dropdowns in
// Settings → Media, shared by the two Linux shells (Qt6 and GTK4). Linux
// only — Windows and macOS enumerate natively (DirectShow / AVFoundation) in
// their own Host implementations, and this file is not built for them.
//
// gst_class is a "/"-separated device class ("Audio/Source", "Video/Sink",
// …). caps_mime is a media type to filter on ("audio/x-raw", "video/x-raw"),
// or nullptr for any.
//
// For audio the returned id is a PulseAudio/PipeWire source or sink name; for
// video it is the /dev/videoN path. The video id matters: it is what
// VideoCapture::start() injects into the v4l2src pipeline, so it has to be a
// real device path rather than anything provider-specific.
std::vector<DeviceListing>
enumerate_gst_devices(const char* gst_class, const char* caps_mime);

// Strips the " (V4L2)" suffix and trailing whitespace that GstV4l2Device adds
// to its display name: the kernel truncates card names to 31 characters and
// often leaves a trailing space, which renders as "Name  (V4L2)".
std::string clean_device_display_name(const std::string& display);

} // namespace tk

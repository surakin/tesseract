// CallType — what kind of call the user chose to start. A video call stays a
// video call when its camera is off, so the camera toggle remains available.
#pragma once

namespace tesseract::views
{

enum class CallType
{
    Audio, // no camera toggle in the call
    Video, // camera toggle available; the camera may start off
};

} // namespace tesseract::views

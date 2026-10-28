#pragma once

namespace tesseract {

namespace detail {
inline bool update_checks_disabled = false;
} // namespace detail

// Turns update checks off for this process at runtime, for installs whose
// updates are delivered by something else (e.g. an MSIX package, updated by
// the Store or App Installer). Call once at startup, before any UI exists —
// the Settings "Updates" group is only built when checks are enabled.
inline void disable_update_checks()
{
    detail::update_checks_disabled = true;
}

// True when an update-check backend was configured at build time
// (TESSERACT_UPDATE_CHECKS) and disable_update_checks() hasn't been called.
inline bool update_checks_enabled()
{
#ifdef TESSERACT_UPDATE_CHECKS
    return !detail::update_checks_disabled;
#else
    return false;
#endif
}

} // namespace tesseract

#!/bin/bash
# Regenerates iana_timezones.h from the system tzdata.
#
# Source: /usr/share/zoneinfo/zone.tab, column 3 (the IANA per-location zone
# table). zone.tab lists every distinct populated location — including the
# ones zone1970.tab collapses onto a shared representative (Europe/Oslo,
# Europe/Stockholm, ... -> Europe/Berlin) — so a timezone another client
# published still resolves to a picker row. It excludes deprecated
# backward-compat aliases (US/Eastern, Asia/Calcutta, ...).
#
# Run from anywhere; writes iana_timezones.h next to this script.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/iana_timezones.h"
tztab=/usr/share/zoneinfo/zone.tab

n=$(awk -F'\t' '!/^#/{print $3}' "$tztab" | sort -u | wc -l)

{
  cat <<HDR
#pragma once

// Static table of IANA tzdb zone identifiers, backing TimezonePicker's
// searchable dropdown for the account settings Timezone field
// (us.cloke.msc4175.tz). Zone IDs are data values, like this app's
// pronoun-language codes in bcp47_languages.h — not translated UI chrome
// (see CLAUDE.md's i18n rule, which applies to UI strings, not data).
//
// Generated from this system's /usr/share/zoneinfo/zone.tab (IANA's
// per-location table), column 3, deduped and sorted. zone.tab lists every
// distinct populated location — including the ones zone1970.tab collapses
// onto a shared representative (e.g. Europe/Oslo, Europe/Stockholm →
// Europe/Berlin) — so a timezone another client published still resolves to
// a picker row. It still excludes deprecated backward-compat aliases
// (US/Eastern, Asia/Calcutta, ...). $n entries as of the tzdata version this
// was generated from. Regenerate with gen_iana_timezones.sh (this directory).

#include <string_view>

namespace tesseract::views
{

struct IanaTimezone
{
    std::string_view id;
};

inline constexpr IanaTimezone kIanaTimezones[] = {
HDR
  awk -F'\t' '!/^#/{print $3}' "$tztab" | sort -u | awk '
    BEGIN { printf "    " }
    { printf "{\"%s\"},", $0; if (++c % 3 == 0) printf "\n    "; else printf " " }
    END { print "" }' | sed 's/[[:space:]]*$//'
  cat <<'FTR'
};

} // namespace tesseract::views
FTR
} > "$out"

echo "wrote $out ($n zones)"

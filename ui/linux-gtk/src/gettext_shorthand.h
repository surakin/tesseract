#pragma once

// Shared `_(s)` shorthand for gettext(), used by the GTK4 shell's own
// glue code (outside tk::tr()/tk::trf() — see ui/shared/tk/i18n.h for the
// cross-platform translation API used everywhere else).
#include <libintl.h>
#define _(s) gettext(s)

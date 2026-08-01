#pragma once

// Qt6 accessibility bridge: builds a real, navigable QAccessibleInterface
// tree from tk::build_access_tree()'s output, registered via Qt's own
// QAccessibleInterface factory mechanism — so it becomes part of Qt's
// *existing* single AT-SPI application instead of a second, separate one
// (the problem the reverted AccessKit-based Linux spike hit). See
// /home/rayden/.claude/plans/i-need-an-assessment-graceful-steele.md for the
// full history — this file supersedes the earlier hardcoded
// qt_accessible_spike.{h,cpp} (one fake button) now that the generic
// role/name/state/action model exists to build a real tree from.

#include <QtWidgets/QWidget>

namespace tk::qt6
{

// Call once (e.g. from main()) before any accessible query can occur.
void install_accessible_factory();

} // namespace tk::qt6

#pragma once

// Phase 2 spike (native QAccessibleInterface, replacing the reverted
// AccessKit-based Linux spike — see
// /home/rayden/.claude/plans/i-need-an-assessment-graceful-steele.md).
//
// Registers one hardcoded accessible button as a child of a
// tk::qt6::Surface, via Qt's own QAccessibleInterface factory mechanism —
// so it becomes part of Qt's *existing* single AT-SPI application instead
// of a second, separate one (the problem the AccessKit-based spike hit).
// No generic tk::Widget-tree walking yet; that's the next step once this
// proves the mechanism.

#include <QtWidgets/QWidget>

namespace tk::qt6
{

// Call once (e.g. from main()) before any accessible query can occur.
void install_accessible_spike_factory();

} // namespace tk::qt6

#pragma once
#include <string>
#include <vector>

namespace mac
{

// Hands the relaunch arguments (--relaunch, --profile) to the next instance
// through a file in the app's temp directory. The Mac App Store edition
// relaunches through NSWorkspace, which ignores
// NSWorkspaceOpenConfiguration.arguments for a sandboxed caller; without
// --relaunch the new instance would find the old one still holding the
// single-instance lock and quit as a duplicate. Both instances share the
// sandbox container, so they see the same temp directory.

// Writes `args` for the next launch. Returns false if the file couldn't be
// written.
bool write_relaunch_handoff(const std::vector<std::string>& args);

// Deletes a handoff written by write_relaunch_handoff (a relaunch that
// failed to start).
void discard_relaunch_handoff();

// Returns and deletes the handed-off arguments, or an empty vector if there
// are none or they're older than a few seconds (a stale file from a
// relaunch that never started must not turn a later launch into one).
std::vector<std::string> take_relaunch_handoff();

} // namespace mac

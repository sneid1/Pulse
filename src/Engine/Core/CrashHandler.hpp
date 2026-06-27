#pragma once

// Process-wide crash diagnostics. installCrashHandler() registers an unhandled
// structured-exception filter (catches access violations etc. on ANY thread,
// including the audio thread) that writes a human-readable pulse_crash.log and a
// pulse_crash.dmp minidump next to the exe, then lets the process die. This turns
// an otherwise silent close into an actionable stack: open the .dmp in Visual
// Studio / WinDbg, or read the log for the faulting module + offset.
//
// It is a no-op when a debugger is attached (the debugger gets the exception
// first), so F5 debugging is unaffected.

namespace pulse {

void installCrashHandler();

} // namespace pulse

// InstanceRender - Watchdog.h
// Catching a freeze in somebody else's Nuke session.
//
// IR_LOG says which line was reached last.  That is enough when the stall is in
// our own code and the next line is obvious, and useless otherwise: it cannot
// say what the thread is BLOCKED ON, and it says nothing at all when the thing
// that stopped is Nuke's main loop rather than a render.  A freeze reported from
// a real session has to be caught without a debugger attached, without a repro,
// and while the process is wedged - which rules out anything driven from Qt,
// because a wedged Qt is the thing being measured.
//
// So: a plain OS thread, owned by this plugin, that watches two clocks.
//
//   1. The PHASE clock.  Render code calls mark() as it goes; if a phase runs
//      longer than the limit, the watchdog fires.
//   2. The PULSE clock.  A timer in the GUI touches a file every half second
//      (test/gui_probe_freeze/watchdog_menu.py).  If that file stops advancing,
//      Nuke's main thread has stopped, whatever the renderer is doing - so this
//      catches freezes that are nothing to do with us, which is exactly what is
//      needed when it is not yet known whose fault it is.
//
// When it fires it writes a stack for EVERY thread in the process, and
// optionally a minidump.  Even with no symbols for Nuke's own libraries, the
// module and offset per frame says which DLL each thread is sitting in, which
// is normally enough to tell "blocked in our render" from "blocked in DDImage
// waiting for a lock" from "blocked in the driver".
//
// Costs nothing unless IR_WATCHDOG is set.  Strict ASCII.
//
//   IR_WATCHDOG=<seconds>     turn it on; fire after this long (e.g. 20)
//   IR_WATCHDOG_DIR=<dir>     where dumps go (default: beside IR_LOG, else TEMP)
//   IR_WATCHDOG_PULSE=<file>  the file the GUI timer touches
//   IR_WATCHDOG_MINIDUMP=1    also write a .dmp for WinDbg / Visual Studio
//   IR_WATCHDOG_MAX=<n>       stop after n dumps (default 3)
#pragma once

#include <string>

namespace ir {

// Starts the watching thread the first time it is called, if IR_WATCHDOG is
// set.  Safe to call from anywhere, any number of times.
void watchdogStart();

// Says what the render is doing now.  Cheap: one pointer store and one clock
// read, no allocation, no lock.  Pass nullptr when a phase ends and nothing has
// taken its place.
void watchdogMark(const char* phase);

// Writes the dump immediately, whatever the clocks say.  This is what a knob or
// a test calls to prove the machinery works without waiting for a real freeze.
// Returns the file it wrote, or an empty string.
std::string watchdogDumpNow(const std::string& reason);

// Marks a phase for as long as it is in scope, and restores the phase that was
// running before - so a nested phase does not make its caller look finished.
class WatchdogPhase {
public:
  explicit WatchdogPhase(const char* phase);
  ~WatchdogPhase();
private:
  const char* _previous;
  WatchdogPhase(const WatchdogPhase&);
  WatchdogPhase& operator=(const WatchdogPhase&);
};

} // namespace ir

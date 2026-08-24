// InstanceRender - Watchdog.cpp
// See Watchdog.h for what this is for.  Strict ASCII.
#include "Watchdog.h"

#include "Trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#  include <tlhelp32.h>
#  pragma comment(lib, "dbghelp.lib")
#endif

namespace ir {
namespace {

typedef std::chrono::steady_clock Clock;

std::atomic<const char*> gPhase(nullptr);
std::atomic<long long>   gPhaseSinceMs(0);
std::atomic<unsigned long> gPhaseThread(0);
std::atomic<int>         gDumps(0);
std::atomic<bool>        gStarted(false);
std::mutex               gDumpMutex;
Clock::time_point        gEpoch;

long long nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - gEpoch).count();
}

std::string env(const char* name)
{
#ifdef _WIN32
  char buf[1024];
  const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

double envSeconds(const char* name, double dflt)
{
  const std::string v = env(name);
  if (v.empty()) return dflt;
  const double d = std::atof(v.c_str());
  return d > 0.0 ? d : dflt;
}

// Where the dumps go: next to IR_LOG if there is one, so a report arrives as
// one directory rather than as files the reporter has to go and find.
std::string dumpDir()
{
  std::string d = env("IR_WATCHDOG_DIR");
  if (!d.empty()) return d;
  const std::string log = env("IR_LOG");
  if (!log.empty()) {
    const size_t slash = log.find_last_of("/\\");
    if (slash != std::string::npos) return log.substr(0, slash);
  }
#ifdef _WIN32
  char buf[MAX_PATH];
  const DWORD n = GetTempPathA(sizeof(buf), buf);
  if (n > 0 && n < sizeof(buf)) return std::string(buf, n);
#endif
  return std::string(".");
}

#ifdef _WIN32

// Symbols are looked up AFTER the thread is resumed.  Suspending a thread and
// then calling into dbghelp - which takes locks and allocates - is how a stack
// dumper deadlocks the process it was meant to diagnose.  So the suspended
// window holds nothing but StackWalk64, and only one thread is ever suspended
// at a time.
struct ThreadStack {
  DWORD id = 0;
  std::vector<DWORD64> frames;
  bool walked = false;
  unsigned long long created = 0;   // to find the main thread: it is the oldest
};

// The main thread is not labelled by Windows, but it is always the first one
// the process made, and that is recorded.
unsigned long long threadCreatedAt(DWORD tid)
{
  HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!h) return 0;
  FILETIME created, exited, kernel, user;
  unsigned long long out = 0;
  if (GetThreadTimes(h, &created, &exited, &kernel, &user))
    out = (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  CloseHandle(h);
  return out;
}

bool walkOneThread(DWORD tid, ThreadStack& out)
{
  out.id = tid;
  HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME,
                        FALSE, tid);
  if (!h) return false;
  if (SuspendThread(h) == DWORD(-1)) { CloseHandle(h); return false; }

  CONTEXT ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.ContextFlags = CONTEXT_FULL;
  if (GetThreadContext(h, &ctx)) {
    STACKFRAME64 sf;
    std::memset(&sf, 0, sizeof(sf));
#if defined(_M_X64)
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
    const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    sf.AddrPC.Offset = ctx.Pc;
    sf.AddrFrame.Offset = ctx.Fp;
    sf.AddrStack.Offset = ctx.Sp;
#else
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
    sf.AddrPC.Offset = ctx.Eip;
    sf.AddrFrame.Offset = ctx.Ebp;
    sf.AddrStack.Offset = ctx.Esp;
#endif
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Mode = AddrModeFlat;

    out.frames.reserve(64);
    for (int i = 0; i < 64; ++i) {
      if (!StackWalk64(machine, GetCurrentProcess(), h, &sf, &ctx, nullptr,
                       SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        break;
      if (sf.AddrPC.Offset == 0) break;
      out.frames.push_back(sf.AddrPC.Offset);
    }
    out.walked = true;
  }

  ResumeThread(h);
  CloseHandle(h);
  return out.walked;
}

void describe(FILE* f, DWORD64 addr)
{
  // module first: it is the part that still works with no symbols at all, and
  // "sitting in DDImage" is most of the answer most of the time
  char modName[MAX_PATH] = "?";
  IMAGEHLP_MODULE64 mi;
  std::memset(&mi, 0, sizeof(mi));
  mi.SizeOfStruct = sizeof(mi);
  DWORD64 base = 0;
  if (SymGetModuleInfo64(GetCurrentProcess(), addr, &mi)) {
    std::strncpy(modName, mi.ModuleName, sizeof(modName) - 1);
    base = mi.BaseOfImage;
  }

  char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
  std::memset(buf, 0, sizeof(buf));
  SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = MAX_SYM_NAME;
  DWORD64 disp = 0;
  const bool haveSym = SymFromAddr(GetCurrentProcess(), addr, &disp, sym) != FALSE;

  IMAGEHLP_LINE64 line;
  std::memset(&line, 0, sizeof(line));
  line.SizeOfStruct = sizeof(line);
  DWORD lineDisp = 0;
  const bool haveLine = SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisp, &line) != FALSE;

  std::fprintf(f, "      %-20s +0x%-8llx  %s", modName,
               static_cast<unsigned long long>(base ? addr - base : 0),
               haveSym ? sym->Name : "(no symbol)");
  if (haveSym && disp) std::fprintf(f, "+0x%llx", static_cast<unsigned long long>(disp));
  if (haveLine) std::fprintf(f, "   [%s:%lu]", line.FileName, static_cast<unsigned long>(line.LineNumber));
  std::fprintf(f, "\n");
}

void writeMinidump(const std::string& path)
{
  HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, nullptr, nullptr, nullptr);
  CloseHandle(f);
}

#endif // _WIN32

std::string dump(const std::string& reason)
{
  std::lock_guard<std::mutex> lock(gDumpMutex);
  const int n = ++*(&gDumps);
  char name[64];
  std::snprintf(name, sizeof(name), "/ir_freeze_%lu_%d.txt",
                static_cast<unsigned long>(
#ifdef _WIN32
                    GetCurrentProcessId()
#else
                    0
#endif
                    ), n);
  const std::string path = dumpDir() + name;

  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return std::string();

  const char* phase = gPhase.load();
  std::fprintf(f, "InstanceRender freeze report\n");
  std::fprintf(f, "  reason        : %s\n", reason.c_str());
  std::fprintf(f, "  current phase : %s\n", phase ? phase : "(none - the renderer is idle)");
  if (phase) std::fprintf(f, "  phase age     : %lld ms\n", nowMs() - gPhaseSinceMs.load());
  std::fprintf(f, "  uptime        : %lld ms\n", nowMs());
  std::fprintf(f, "\nIf the phase is (none) the renderer was not doing anything, and whatever\n"
                  "stopped Nuke is somewhere else - look for the main thread below.\n\n");
  // On disk NOW, before the slow part.  Walking a hundred threads and
  // resolving symbols for them takes seconds, and a frozen Nuke is usually
  // killed rather than closed - so without this flush the common case is a
  // report that says nothing at all.  The header alone (reason, phase, how
  // long it had been stuck) is most of the value; the stacks are detail.
  std::fflush(f);
  trace("watchdog: dumping to " + path + " - " + reason);

#ifdef _WIN32
  // One snapshot of the thread list, then one thread at a time
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    std::fprintf(f, "could not enumerate threads (error %lu)\n", GetLastError());
    std::fclose(f);
    return path;
  }
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();

  std::vector<DWORD> tids;
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);

  std::vector<ThreadStack> stacks;
  stacks.reserve(tids.size());
  for (size_t i = 0; i < tids.size(); ++i) {
    ThreadStack ts;
    if (!walkOneThread(tids[i], ts)) ts.id = tids[i];
    ts.created = threadCreatedAt(tids[i]);
    stacks.push_back(ts);
  }

  // oldest first, which puts Nuke's main thread at the top where it is wanted
  std::sort(stacks.begin(), stacks.end(),
            [](const ThreadStack& a, const ThreadStack& b) {
              if (a.created && b.created) return a.created < b.created;
              return a.created > b.created;      // unknown creation time sinks
            });
  const DWORD phaseTid = static_cast<DWORD>(gPhaseThread.load());

  std::fprintf(f, "%d thread(s), oldest first.  READ THESE FIRST:\n", int(stacks.size()));
  std::fprintf(f, "  - the one marked MAIN: if it is stuck, the interface is frozen\n");
  if (phaseTid)
    std::fprintf(f, "  - the one marked RENDER: it is the one inside the phase named above\n");
  else
    std::fprintf(f, "  - no thread was in a render phase, so this freeze is not a render\n");
  std::fprintf(f, "\n");

  for (size_t i = 0; i < stacks.size(); ++i) {
    const bool isMain = (i == 0 && stacks[i].created != 0);
    const bool isPhase = phaseTid && stacks[i].id == phaseTid;
    std::fprintf(f, "  thread %lu%s%s%s\n", static_cast<unsigned long>(stacks[i].id),
                 isMain ? "   <== MAIN (Nuke's interface runs here)" : "",
                 isPhase ? "   <== RENDER (this one is in the phase above)" : "",
                 stacks[i].walked ? "" : "   (could not be walked)");
    for (size_t k = 0; k < stacks[i].frames.size(); ++k) describe(f, stacks[i].frames[k]);
    if (stacks[i].frames.empty()) std::fprintf(f, "      (no frames)\n");
    std::fprintf(f, "\n");
  }
#else
  std::fprintf(f, "(thread stacks are only implemented on Windows)\n");
#endif

  std::fclose(f);

#ifdef _WIN32
  if (env("IR_WATCHDOG_MINIDUMP") == "1") {
    std::string dmp = path;
    const size_t dot = dmp.find_last_of('.');
    if (dot != std::string::npos) dmp = dmp.substr(0, dot);
    dmp += ".dmp";
    writeMinidump(dmp);
    trace("watchdog: minidump " + dmp);
  }
#endif

  trace("watchdog: DUMPED " + path + " (" + reason + ")");
  return path;
}

void watchLoop()
{
  const double limit = envSeconds("IR_WATCHDOG", 20.0);
  const long long limitMs = static_cast<long long>(limit * 1000.0);
  const std::string pulse = env("IR_WATCHDOG_PULSE");
  const std::string maxs = env("IR_WATCHDOG_MAX");
  const int maxDumps = maxs.empty() ? 3 : std::max(1, std::atoi(maxs.c_str()));

  trace("watchdog: watching, limit " + std::to_string(limitMs) + " ms" +
        (pulse.empty() ? std::string(", no GUI pulse") : (", pulse " + pulse)));

  long long lastPulseValue = -1;
  long long lastPulseChangeMs = nowMs();
  bool firedPhase = false, firedPulse = false;

  while (gDumps.load() < maxDumps) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    // 1. a render phase that has outstayed its welcome
    const char* phase = gPhase.load();
    const long long age = nowMs() - gPhaseSinceMs.load();
    if (phase && age > limitMs) {
      if (!firedPhase) {
        firedPhase = true;
        dump(std::string("phase \"") + phase + "\" has been running " +
             std::to_string(age) + " ms");
      }
    }
    else {
      firedPhase = false;
    }

    // 2. a GUI that has stopped pulsing - a freeze that may be nothing to do
    //    with this node at all, which is the case worth catching
    if (!pulse.empty()) {
      long long v = -1;
      if (FILE* pf = std::fopen(pulse.c_str(), "rb")) {
        char buf[64] = {0};
        const size_t got = std::fread(buf, 1, sizeof(buf) - 1, pf);
        std::fclose(pf);
        if (got > 0) v = std::atoll(buf);
      }
      if (v != lastPulseValue) {
        lastPulseValue = v;
        lastPulseChangeMs = nowMs();
        firedPulse = false;
      }
      else if (v >= 0 && !firedPulse && (nowMs() - lastPulseChangeMs) > limitMs) {
        firedPulse = true;
        dump("Nuke's GUI stopped pulsing for " +
             std::to_string(nowMs() - lastPulseChangeMs) + " ms");
      }
    }
  }
  trace("watchdog: dump limit reached, standing down");
}

} // namespace

void watchdogStart()
{
  if (env("IR_WATCHDOG").empty()) return;
  bool expected = false;
  if (!gStarted.compare_exchange_strong(expected, true)) return;
  gEpoch = Clock::now();
  gPhaseSinceMs.store(0);
#ifdef _WIN32
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
#endif
  std::thread(watchLoop).detach();
}

void watchdogMark(const char* phase)
{
  if (!gStarted.load()) return;
  gPhase.store(phase);
  gPhaseSinceMs.store(nowMs());
#ifdef _WIN32
  gPhaseThread.store(phase ? GetCurrentThreadId() : 0);
#endif
}

std::string watchdogDumpNow(const std::string& reason)
{
  if (!gStarted.load()) return std::string();
  return dump(reason);
}

WatchdogPhase::WatchdogPhase(const char* phase)
  : _previous(gStarted.load() ? gPhase.load() : nullptr)
{
  watchdogMark(phase);
}

WatchdogPhase::~WatchdogPhase()
{
  watchdogMark(_previous);
}

} // namespace ir

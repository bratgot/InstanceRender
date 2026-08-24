// InstanceRender - Trace.h
// The IR_LOG trace, shared by every translation unit that wants to be in it.
//
// Set IR_LOG=<file> and each call appends one flushed line, stamped with
// milliseconds since the first.  The stamps are the point: a trace of what
// happened cannot tell a slow step from a fast one, and "which step took the
// ninety seconds" is the only question this file is ever opened to answer.
//
// It used to live in an anonymous namespace inside InstanceRender.cpp, which
// meant the loaders - where a render actually spends its time when something is
// wrong - could not write to it, and a stall inside one traced as a gap between
// two lines with nothing in between.
//
// Deliberately cheap when off: one function-local static read and a return, so
// calls can sit in loops without being guarded.  Strict ASCII.
#pragma once

#include <chrono>
#include <cstdio>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cstdlib>
#endif

namespace ir {

inline const std::string& tracePath()
{
  static const std::string path = [] {
#ifdef _WIN32
    char buf[1024];
    const DWORD n = GetEnvironmentVariableA("IR_LOG", buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
#else
    const char* v = std::getenv("IR_LOG");
    return v ? std::string(v) : std::string();
#endif
  }();
  return path;
}

inline void trace(const std::string& msg)
{
  const std::string& path = tracePath();
  if (path.empty()) return;
  static const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (FILE* f = std::fopen(path.c_str(), "a")) {
    std::fprintf(f, "%10.1fms  %s\n", ms, msg.c_str());
    std::fclose(f);
  }
}

} // namespace ir

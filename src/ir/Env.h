// Reading environment variables, on either platform.
//
// This exists because the same three lines of Win32 were repeated at a dozen
// call sites, and half of them were NOT inside a _WIN32 guard - which compiles
// perfectly on Windows and stops a Linux build dead at the first one. One
// helper is one place to guard.
//
// GetEnvironmentVariableA rather than getenv on Windows on purpose: getenv reads
// a snapshot taken at process start, so a variable set by a host after Nuke
// launched is invisible to it.
// Strict ASCII.
#pragma once

#include <cstdlib>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace ir {

// The variable's value, or empty when it is not set.
inline std::string envString(const char* name)
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

// Set to anything other than "0" or empty.
inline bool envOn(const char* name)
{
  const std::string v = envString(name);
  return !v.empty() && v != "0";
}

// The variable as a number, or 'dflt' when it is unset or not one.
inline int envInt(const char* name, int dflt = 0)
{
  const std::string v = envString(name);
  if (v.empty()) return dflt;
  return std::atoi(v.c_str());
}

} // namespace ir

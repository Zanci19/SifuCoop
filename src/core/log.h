#pragma once

namespace sifucoop::log {

void Open();
void Close();
void Write(const char* fmt, ...);

// Every line carries a stamp on a clock BOTH machines share (the host's), so
// the host log and the client log of one session can be read side by side.
// log/ knows nothing about the network, so the session installs its clock
// here once it is running; until then lines carry no shared stamp.
using ClockFn = unsigned long (*)();
using ClockValidFn = bool (*)();
void SetSharedClock(ClockFn clock, ClockValidFn valid);

// One letter identifying this machine in every line: H, C or '-' offline.
void SetRoleTag(char tag);

// The shared stamp as the log would print it right now, for callers that build
// their own lines (see core/trace.cpp). Zero when no clock is installed yet.
unsigned long SharedStamp();

}

#define SC_LOG(...) ::sifucoop::log::Write(__VA_ARGS__)

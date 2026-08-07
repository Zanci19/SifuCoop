#pragma once

// Deliberately dependency-free and crash-tolerant: this log is often the only
// evidence available when the game dies during startup, so every line is
// flushed immediately rather than buffered.

namespace sifucoop::log {

void Open();
void Close();
void Write(const char* fmt, ...);

}  // namespace sifucoop::log

#define SC_LOG(...) ::sifucoop::log::Write(__VA_ARGS__)

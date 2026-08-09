#pragma once

// on game crash, this will write the log file

namespace sifucoop::log {

void Open();
void Close();
void Write(const char* fmt, ...);

}  // namespace sifucoop::log

#define SC_LOG(...) ::sifucoop::log::Write(__VA_ARGS__)

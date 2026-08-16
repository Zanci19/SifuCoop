#pragma once





namespace sifucoop::log {

void Open();
void Close();
void Write(const char* fmt, ...);

}

#define SC_LOG(...) ::sifucoop::log::Write(__VA_ARGS__)

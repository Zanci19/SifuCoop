#pragma once

#include <cstddef>
#include <cstdint>















namespace sifucoop::net {

constexpr int kSha256Size = 32;

void Sha256(const void* data, std::size_t size, std::uint8_t out[kSha256Size]);



void HmacSha256(const std::uint8_t* key, std::size_t key_size, const void* data,
                std::size_t size, std::uint8_t out[kSha256Size]);




bool SecureEqual(const std::uint8_t* a, const std::uint8_t* b, std::size_t size);

}

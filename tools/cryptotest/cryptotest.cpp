// Checks src/net/crypto.cpp against the published test vectors.
//
// This is the one piece of the mod where "it seems to work" is not evidence: a
// broken HMAC either rejects everything (obvious) or accepts everything
// (invisible, and the whole point of having it is gone). Vectors from FIPS
// 180-4 for SHA-256 and RFC 4231 for HMAC-SHA256.

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../../src/net/crypto.h"

using namespace sifucoop::net;

namespace {

int g_failures = 0;

void ToHex(const unsigned char* bytes, int size, char* out) {
    for (int i = 0; i < size; ++i) sprintf(out + i * 2, "%02x", bytes[i]);
    out[size * 2] = '\0';
}

void Check(const char* what, const unsigned char* actual, const char* expected) {
    char hex[kSha256Size * 2 + 1];
    ToHex(actual, kSha256Size, hex);
    const bool ok = strcmp(hex, expected) == 0;
    if (!ok) ++g_failures;
    printf("%-34s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        printf("    expected %s\n    got      %s\n", expected, hex);
    }
}

}  // namespace

int main() {
    unsigned char digest[kSha256Size];

    // FIPS 180-4, one-block message.
    Sha256("abc", 3, digest);
    Check("sha256(\"abc\")", digest,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Empty message: exercises the padding-only path.
    Sha256("", 0, digest);
    Check("sha256(\"\")", digest,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // The padding boundaries, where a length that no longer fits forces a
    // second block. These are the lengths a hand-written SHA-256 gets wrong,
    // and a packet is not a round number of blocks -- so they are worth more
    // here than any number of comfortable middle-sized inputs.
    //
    // Every expected value below was computed independently (Python hashlib),
    // not read off this implementation. An earlier version of this file carried
    // a digest written from memory; it disagreed, and the implementation turned
    // out to be right. A test whose expectations come from the thing under test
    // is not a test.
    struct Boundary {
        int length;
        const char* expected;
    };
    static const Boundary kBoundaries[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    };
    for (const Boundary& boundary : kBoundaries) {
        char message[160];
        memset(message, 'a', sizeof(message));
        Sha256(message, static_cast<std::size_t>(boundary.length), digest);
        char label[48];
        sprintf(label, "sha256(%d x 'a')", boundary.length);
        Check(label, digest, boundary.expected);
    }

    // FIPS 180-4, multi-block message.
    const char* two_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256(two_block, strlen(two_block), digest);
    Check("sha256(two-block message)", digest,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // RFC 4231 case 1.
    unsigned char key1[20];
    memset(key1, 0x0b, sizeof(key1));
    HmacSha256(key1, sizeof(key1), "Hi There", 8, digest);
    Check("hmac-sha256 rfc4231 case 1", digest,
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // RFC 4231 case 2: key shorter than the block.
    HmacSha256(reinterpret_cast<const unsigned char*>("Jefe"), 4, "what do ya want for nothing?",
               28, digest);
    Check("hmac-sha256 rfc4231 case 2", digest,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // RFC 4231 case 3: 50 bytes of 0xdd, exercising a multi-block message.
    unsigned char key3[20];
    memset(key3, 0xaa, sizeof(key3));
    unsigned char data3[50];
    memset(data3, 0xdd, sizeof(data3));
    HmacSha256(key3, sizeof(key3), data3, sizeof(data3), digest);
    Check("hmac-sha256 rfc4231 case 3", digest,
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // RFC 4231 case 6: key longer than the block, so it gets hashed first.
    unsigned char key6[131];
    memset(key6, 0xaa, sizeof(key6));
    HmacSha256(key6, sizeof(key6), "Test Using Larger Than Block-Size Key - Hash Key First",
               54, digest);
    Check("hmac-sha256 rfc4231 case 6", digest,
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // A one-bit change must change the tag; equality must be exact.
    unsigned char a[kSha256Size], b[kSha256Size];
    Sha256("payload", 7, a);
    Sha256("payloae", 7, b);
    printf("%-34s %s\n", "distinct inputs differ",
           SecureEqual(a, b, kSha256Size) ? "FAILED" : "ok");
    if (SecureEqual(a, b, kSha256Size)) ++g_failures;
    printf("%-34s %s\n", "identical inputs match",
           SecureEqual(a, a, kSha256Size) ? "ok" : "FAILED");
    if (!SecureEqual(a, a, kSha256Size)) ++g_failures;

    printf("\n%s\n", g_failures == 0 ? "all vectors pass" : "SOME VECTORS FAILED");
    return g_failures == 0 ? 0 : 1;
}

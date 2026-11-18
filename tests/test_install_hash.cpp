// The SHA-256 of the hash-reference helper (tests/install_hash.h) against the FIPS 180-4 test values; no game folder needed.
#include "install_hash.h"

TEST(InstallHash, Sha256KnownValues) {
    EXPECT_EQ(md_test::Sha256("", 0), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(md_test::Sha256("abc", 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   // 56 bytes: the padding needs a second block
    EXPECT_EQ(md_test::Sha256(two.data(), two.size()), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string a(1000000, 'a');
    EXPECT_EQ(md_test::Sha256(a.data(), a.size()), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

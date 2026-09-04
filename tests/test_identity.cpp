#include <gtest/gtest.h>

#include <array>

#include "liquid/identity.hpp"
#include "unsupported.hpp"

TEST(Identity, FacadeNamesOwningIssue) {
    static_assert(sizeof(liquid::signer) == sizeof(void*));
    static_assert(sizeof(liquid::key_provider) == sizeof(void*));
    liquid::signer signer;
    liquid::key_provider provider;
    const liquid::actor actor{"taap:example"};
    std::array<std::byte, 1> bytes{};
    expect_unsupported(signer.sign(bytes), "liquid #4");
    expect_unsupported(liquid::verify(actor, bytes, bytes), "liquid #4");
}

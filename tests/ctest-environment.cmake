# SPDX-License-Identifier: MIT

# Read by ctest before it runs any test (TEST_INCLUDE_FILES in tests/CMakeLists.txt), so every test
# process inherits these settings.
#
# Tests create throwaway identities in temporary homes and have no terminal to answer a passphrase
# prompt, so they opt into file-permission-only custody for the identity wrapping key (liquid
# #402). The tests of custody itself set their own value.
set(ENV{LIQUID_IDENTITY_KEY_CUSTODY} file)

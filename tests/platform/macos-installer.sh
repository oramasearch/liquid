# SPDX-License-Identifier: MIT

set -euo pipefail

liquid_binary=$(realpath "$1")
projection_fixture=$(realpath "$2")
installer_program=$(realpath "$3")
test_root=$(mktemp -d "${RUNNER_TEMP:-/tmp}/liquid-macos-installer.XXXXXX")
keychain=$test_root/test.keychain-db
keychain_password=liquid-test-keychain
identity="Liquid Installer Test"
application_identity="Liquid Application Test"
installed_program=$HOME/bin/liquid-installer-fixture

cleanup() {
  if [[ ${trusted_system:-false} == true ]]; then
    sudo security delete-certificate -c "$identity" /Library/Keychains/System.keychain \
      >/dev/null 2>&1 || true
  fi
  security list-keychains -d user -s ${previous_keychains:-} >/dev/null 2>&1 || true
  security delete-keychain "$keychain" >/dev/null 2>&1 || true
  rm -f "$installed_program"
  rm -rf "$test_root"
}
trap cleanup EXIT

mkdir -p "$test_root/root/bin"
cp "$installer_program" "$test_root/root/bin/liquid-installer-fixture"
chmod 0755 "$test_root/root/bin/liquid-installer-fixture"

"$projection_fixture" macos-pkg "$test_root/root" "$test_root/first" "$test_root/repository-first" |
  tee "$test_root/first.txt"
"$projection_fixture" macos-pkg "$test_root/root" "$test_root/second" "$test_root/repository-second" |
  tee "$test_root/second.txt"
"$projection_fixture" macos-dmg "$test_root/root" "$test_root/dmg" "$test_root/repository-dmg" |
  tee "$test_root/dmg.txt"
first_artifact=$(sed -n 's/^artifact=//p' "$test_root/first.txt")
second_artifact=$(sed -n 's/^artifact=//p' "$test_root/second.txt")
cmp "$first_artifact" "$second_artifact"

openssl req -new -newkey rsa:2048 -nodes -x509 -days 1 \
  -keyout "$test_root/installer-key.pem" \
  -out "$test_root/installer-certificate.pem" \
  -subj "/CN=$identity" \
  -addext "basicConstraints=critical,CA:false" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=critical,1.2.840.113635.100.4.13" \
  -addext "1.2.840.113635.100.6.1.14=critical,DER:05:00" \
  2>/dev/null
openssl pkcs12 -export \
  -legacy \
  -inkey "$test_root/installer-key.pem" \
  -in "$test_root/installer-certificate.pem" \
  -out "$test_root/installer-identity.p12" \
  -passout pass:liquid-test
openssl req -new -newkey rsa:2048 -nodes -x509 -days 1 \
  -keyout "$test_root/application-key.pem" \
  -out "$test_root/application-certificate.pem" \
  -subj "/CN=$application_identity" \
  -addext "basicConstraints=critical,CA:false" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=critical,codeSigning" \
  -addext "1.2.840.113635.100.6.1.13=critical,DER:05:00" \
  2>/dev/null
openssl pkcs12 -export \
  -legacy \
  -inkey "$test_root/application-key.pem" \
  -in "$test_root/application-certificate.pem" \
  -out "$test_root/application-identity.p12" \
  -passout pass:liquid-test

previous_keychains=$(security list-keychains -d user | tr -d '"')
security create-keychain -p "$keychain_password" "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
security set-keychain-settings -lut 21600 "$keychain"
security import "$test_root/installer-identity.p12" \
  -k "$keychain" \
  -P liquid-test \
  -T /usr/bin/codesign \
  -T /usr/bin/productsign
security import "$test_root/application-identity.p12" \
  -k "$keychain" \
  -P liquid-test \
  -T /usr/bin/codesign \
  -T /usr/bin/productsign
security set-key-partition-list \
  -S apple-tool:,apple: \
  -s \
  -k "$keychain_password" \
  "$keychain" >/dev/null
security list-keychains -d user -s "$keychain" $previous_keychains
if [[ ${CI:-false} == true ]]; then
  sudo security add-trusted-cert \
    -d \
    -r trustRoot \
    -k /Library/Keychains/System.keychain \
    "$test_root/installer-certificate.pem"
  trusted_system=true
fi

(
  cd "$test_root/repository-first"
  "$liquid_binary" sign macos "$first_artifact" \
    --identity "keychain:$identity|$application_identity" \
    --no-notarize
) | tee "$test_root/sign.txt"
signed_artifact=$(sed -n 's/^artifact: \([^ ]*\) .*/\1/p' "$test_root/sign.txt")
pkgutil --check-signature "$signed_artifact"
if [[ ${trusted_system:-false} == true ]]; then
  installer -pkg "$signed_artifact" -target CurrentUserHomeDirectory
else
  installer -allowUntrusted -pkg "$signed_artifact" -target CurrentUserHomeDirectory
fi
test "$("$installed_program")" = "liquid installer fixture"
"$liquid_binary" sign verify "$signed_artifact"

dmg_artifact=$(sed -n 's/^artifact=//p' "$test_root/dmg.txt")
(
  cd "$test_root/repository-dmg"
  "$liquid_binary" sign macos "$dmg_artifact" \
    --identity "keychain:$identity|$application_identity" \
    --no-notarize
) | tee "$test_root/dmg-sign.txt"
signed_dmg=$(sed -n 's/^artifact: \([^ ]*\) .*/\1/p' "$test_root/dmg-sign.txt")
codesign --verify --verbose=4 "$signed_dmg"
"$liquid_binary" sign verify "$signed_dmg"

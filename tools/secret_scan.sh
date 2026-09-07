#!/bin/bash
# Fails if anything that must never be published is present in the working tree.
#
# This project ships no ROMs and no Nintendo key material. Run before every push; CI runs it too.
set -uo pipefail

status=0

fail() {
    echo "FAIL: $1"
    status=1
}

echo "== file names =="
hits=$(find . -path ./.git -prune -o \
    \( -iname 'aes_keys.txt' -o -iname 'keys.txt' -o -iname 'default_keys.h' \
       -o -iname 'boot9*.bin' -o -iname 'seeddb.bin' -o -iname 'sector0x96.bin' \
       -o -iname 'ctr-common-1-*' -o -iname 'movable.sed' -o -iname 'otp.bin' \
       -o -iname '*.p12' -o -iname '*.pem' -o -iname '*.cer' -o -iname '*.der' \
       -o -iname '*.mobileprovision' -o -iname '*.certSigningRequest' \
       -o -iname 'id_rsa*' -o -iname '*.keystore' -o -iname '*.jks' \
       -o -iname '*.3ds' -o -iname '*.cci' -o -iname '*.cxi' -o -iname '*.cia' \
       -o -iname '*.ipa' \) -print)
if [ -n "$hits" ]; then
    echo "$hits"
    fail "key material, a certificate, a ROM or a build product is present"
else
    echo "ok"
fi

echo "== removed key blobs must stay removed =="
if grep -rIl "default_keys_enc\|ctr_common_1_key_bin\|ctr_common_1_cert_bin\|ENABLE_BUILTIN_KEYBLOB" \
     . --exclude-dir=.git --exclude-dir=tools --exclude-dir=.github --exclude=README.md 2>/dev/null | grep -q .; then
    grep -rIl "default_keys_enc\|ctr_common_1_key_bin\|ctr_common_1_cert_bin\|ENABLE_BUILTIN_KEYBLOB" \
        . --exclude-dir=.git --exclude-dir=tools --exclude-dir=.github --exclude=README.md
    fail "a reference to a bundled key blob is back"
else
    echo "ok"
fi

echo "== PEM blocks =="
if grep -rIl "BEGIN .*PRIVATE KEY\|BEGIN CERTIFICATE\|BEGIN OPENSSH" . --exclude-dir=.git --exclude-dir=tools --exclude-dir=.github 2>/dev/null | grep -q .; then
    grep -rIl "BEGIN .*PRIVATE KEY\|BEGIN CERTIFICATE\|BEGIN OPENSSH" . --exclude-dir=.git --exclude-dir=tools --exclude-dir=.github
    fail "an embedded certificate or private key is present"
else
    echo "ok"
fi

echo "== signing identity =="
if grep -q "DEVELOPMENT_TEAM = [A-Z0-9]" Folium.xcodeproj/project.pbxproj 2>/dev/null; then
    grep -n "DEVELOPMENT_TEAM = [A-Z0-9]" Folium.xcodeproj/project.pbxproj
    fail "a development team id is committed"
else
    echo "ok"
fi

echo "== git history =="
if [ -d .git ]; then
    if git log --all --pretty=format: --name-only --diff-filter=A 2>/dev/null | sort -u | \
        grep -qiE '(^|/)(aes_keys\.txt|keys\.txt|default_keys\.h|seeddb\.bin|boot9.*\.bin|ctr-common-1-.*)$'; then
        git log --all --pretty=format: --name-only --diff-filter=A | sort -u | \
            grep -iE '(^|/)(aes_keys\.txt|keys\.txt|default_keys\.h|seeddb\.bin|boot9.*\.bin|ctr-common-1-.*)$'
        fail "key material was committed at some point in history"
    else
        echo "ok"
    fi
else
    echo "skipped (no .git)"
fi

echo
if [ "$status" -eq 0 ]; then
    echo "PASS: nothing that must stay private is in the tree."
else
    echo "One or more checks failed."
fi
exit "$status"

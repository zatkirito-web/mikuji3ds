# Mikuji3DS

A 3DS emulator for iPadOS, built for one purpose: playing backups of cartridges you own, on your
own iPad, decrypted with the AES keys you dumped from your own console.

## What this fork changes

This is a fork of [Folium](https://github.com/folium-app/Folium) and its 3DS core
[Cytrus](https://github.com/folium-app/Cytrus), which are themselves derived from
[Azahar](https://github.com/azahar-emu/azahar) and Citra. Upstream Folium loads decrypted titles
only. This fork restores the ability to load encrypted ones using keys the user supplies.

1. **Encrypted NCCH support restored.** Azahar removed it in commit `dc1ebb63c`
   ("Major revamps to match game loading decisions", 2025-02-27). That commit also added unrelated
   features, so it was not reverted; the decryption path alone was reintroduced into
   `Cytrus/System/core/file_sys/ncch_container.cpp` and `romfs_reader.{h,cpp}`, merged with the
   `is_proto` handling the same commit introduced.
2. **All bundled Nintendo key material removed.**
   - `Cytrus/System/include/core/hw/default_keys.h`, the obfuscated key blob, is deleted along with
     the `ENABLE_BUILTIN_KEYBLOB` fallback in `key.cpp`.
   - `ctr-common-1-cert.h` and `ctr-common-1-key.h`, a copy of Nintendo's ClCertA client
     certificate and its private key, are deleted along with the fallback that used them.
   - CI fails the build if any key material reappears in the tree.
3. **Key loading fixed.** Both the legacy Citra `aes_keys.txt` layout and the newer sectioned
   `keys.txt` layout are accepted and auto-detected, a UTF-8 BOM and CRLF line endings are
   tolerated, and the result — file used, layout detected, keys loaded, parse errors — is logged.
   Previously a legacy `aes_keys.txt` was skipped in its entirety without a single message.
4. **Missing keys are reported, not guessed at.** A title that cannot be decrypted names the keys
   that are missing instead of failing silently or producing garbage.

## Keys and ROMs

This repository contains no ROMs, no AES keys, no Nintendo private keys, and no code that
downloads any of them. Nothing is fetched from the network and nothing is hardcoded.

You supply your own `aes_keys.txt`, dumped from your own console with GodMode9's DumpKeys script,
by importing it through the Files app. It is stored at:

```
<App Documents>/Cytrus/sysdata/aes_keys.txt
```

Titles that use seed crypto also need `seeddb.bin` from the same dump, in the same directory.

Note that GodMode9's stock DumpKeys script does not emit `generatorConstant`, without which no
normal key can be derived from a KeyX/KeyY pair. If the key file does not contain it, the emulator
tries to solve for it using a slot for which the file supplies KeyX, KeyY and the resulting normal
key, and reports a specific error if it cannot.

## Building

Builds are produced by GitHub Actions on a macOS runner; see
[`.github/workflows/build-ipa.yml`](.github/workflows/build-ipa.yml). The workflow produces an
**unsigned** IPA suitable for re-signing with SideStore or AltStore. No Apple ID, certificate or
provisioning profile is stored in this repository.

To build locally on a Mac:

```bash
xcodebuild build -project Folium.xcodeproj -scheme Folium -configuration Release \
  -sdk iphoneos -destination 'generic/platform=iOS'
```

## License

GPL-3.0, inherited from Folium and Cytrus. Citra and Azahar code is GPL-2.0-or-later. See
[LICENSE](LICENSE).

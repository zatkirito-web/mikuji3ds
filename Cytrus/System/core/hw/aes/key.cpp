// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/file_sys/certificate.h"
#include "core/file_sys/otp.h"
#include "core/hle/service/fs/archive.h"
#include "core/hw/aes/arithmetic128.h"
#include "core/hw/aes/key.h"
#include "core/hw/rsa/rsa.h"
#include "core/loader/loader.h"

namespace HW::AES {

namespace {

// The generator constant was calculated using the 0x39 KeyX and KeyY retrieved from a 3DS and the
// normal key dumped from a Wii U solving the equation:
// NormalKey = (((KeyX ROL 2) XOR KeyY) + constant) ROL 87
// On a real 3DS the generation for the normal key is hardware based, and thus the constant can't
// get dumped. Generated normal keys are also not accessible on a 3DS. The used formula for
// calculating the constant is a software implementation of what the hardware generator does.
AESKey generator_constant;

AESKey HexToKey(const std::string& hex) {
    if (hex.size() < 32) {
        throw std::invalid_argument("hex string is too short");
    }

    AESKey key;
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<u8>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }

    return key;
}

std::vector<u8> HexToVector(const std::string& hex) {
    std::vector<u8> vector(hex.size() / 2);
    for (std::size_t i = 0; i < vector.size(); ++i) {
        vector[i] = static_cast<u8>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }

    return vector;
}

std::optional<std::size_t> ParseCommonKeyName(const std::string& full_name) {
    std::size_t index;
    int end;
    if (std::sscanf(full_name.c_str(), "common%zd%n", &index, &end) == 1 &&
        end == static_cast<int>(full_name.size())) {
        return index;
    } else {
        return std::nullopt;
    }
}

std::optional<std::pair<std::size_t, std::string>> ParseNfcSecretName(
    const std::string& full_name) {
    std::size_t index;
    int end;
    if (std::sscanf(full_name.c_str(), "nfcSecret%zd%n", &index, &end) == 1) {
        return std::make_pair(index, full_name.substr(end));
    } else {
        return std::nullopt;
    }
}

std::optional<std::pair<std::size_t, char>> ParseKeySlotName(const std::string& full_name) {
    std::size_t slot;
    char type;
    int end;
    if (std::sscanf(full_name.c_str(), "slot0x%zXKey%c%n", &slot, &type, &end) == 2 &&
        end == static_cast<int>(full_name.size())) {
        return std::make_pair(slot, type);
    } else {
        return std::nullopt;
    }
}

struct KeySlot {
    std::optional<AESKey> x;
    std::optional<AESKey> y;
    std::optional<AESKey> normal;

    void SetKeyX(std::optional<AESKey> key) {
        x = key;
        GenerateNormalKey();
    }

    void SetKeyY(std::optional<AESKey> key) {
        y = key;
        GenerateNormalKey();
    }

    void SetNormalKey(std::optional<AESKey> key) {
        normal = key;
    }

    void GenerateNormalKey() {
        if (x && y) {
            normal = Lrot128(Add128(Xor128(Lrot128(*x, 2), *y), generator_constant), 87);
        } else {
            normal.reset();
        }
    }

    void Clear() {
        x.reset();
        y.reset();
        normal.reset();
    }
};

std::array<KeySlot, KeySlotID::MaxKeySlotID> key_slots;
std::array<std::optional<AESKey>, MaxCommonKeySlot> common_key_y_slots;
std::array<std::optional<AESKey>, NumDlpNfcKeyYs> dlp_nfc_key_y_slots;
std::array<NfcSecret, NumNfcSecrets> nfc_secrets;
AESIV nfc_iv;

AESKey otp_key{};
AESIV otp_iv{};

// gets xor'd with the mac address to produce the final iv
AESIV dlp_checksum_mod_iv;

KeySlot movable_key;
KeySlot movable_cmac;

struct KeyDesc {
    char key_type;
    std::size_t slot_id;
    // This key is identical to the key with the same key_type and slot_id -1
    bool same_as_before;
};

KeyLoadReport key_load_report;

// The raw values as they appeared in the user's key file, kept so that the normal keys can be
// regenerated if the generator constant only becomes known after the whole file has been read.
struct RawSlotKeys {
    std::optional<AESKey> x;
    std::optional<AESKey> y;
    std::optional<AESKey> n;
};
std::array<RawSlotKeys, KeySlotID::MaxKeySlotID> raw_slot_keys;

// Contents of the user's key file, normalized so that the legacy Citra layout and the sectioned
// layout parse identically. Cached because the AES, RSA and ECC loaders each ask for it.
std::string cached_keys_data;
bool keys_data_cached = false;

// Strips a UTF-8 BOM, a trailing carriage return and surrounding whitespace, so that files
// generated or edited on other platforms parse the same as native ones.
std::string NormalizeKeyLine(std::string line) {
    if (line.size() >= 3 && static_cast<u8>(line[0]) == 0xEF && static_cast<u8>(line[1]) == 0xBB &&
        static_cast<u8>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
    const std::size_t first = line.find_first_not_of(" 	");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = line.find_last_not_of(" 	
");
    return line.substr(first, last - first + 1);
}

// Solves for the key generator constant using a slot for which the user's own file supplies KeyX,
// KeyY and the resulting normal key. This keeps the constant out of this source tree while still
// allowing key files that do not list it to work.
void TryDeriveGeneratorConstant() {
    for (std::size_t i = 0; i < raw_slot_keys.size(); ++i) {
        const auto& raw = raw_slot_keys[i];
        if (!raw.x || !raw.y || !raw.n) {
            continue;
        }

        // NormalKey = (((KeyX ROL 2) XOR KeyY) + constant) ROL 87
        generator_constant = Sub128(Rrot128(*raw.n, 87), Xor128(Lrot128(*raw.x, 2), *raw.y));
        key_load_report.generator_constant_derived = true;
        LOG_INFO(HW_AES, "Derived the key generator constant from slot {:#04X} of the key file", i);

        // Normal keys generated before the constant was known are wrong, so re-apply everything.
        for (std::size_t j = 0; j < raw_slot_keys.size(); ++j) {
            if (raw_slot_keys[j].x) {
                key_slots[j].SetKeyX(raw_slot_keys[j].x);
            }
            if (raw_slot_keys[j].y) {
                key_slots[j].SetKeyY(raw_slot_keys[j].y);
            }
            if (raw_slot_keys[j].n) {
                key_slots[j].SetNormalKey(raw_slot_keys[j].n);
            }
        }
        return;
    }
}

void LoadPresetKeys() {
    auto s = GetKeysStream();

    std::string mode = "";

    while (!s.eof()) {
        std::string line;
        std::getline(s, line);
        line = NormalizeKeyLine(std::move(line));

        // Ignore empty or commented lines.
        if (line.empty() || line.starts_with("#")) {
            continue;
        }

        if (line.starts_with(":")) {
            mode = line.substr(1);
            continue;
        }

        if (mode != "AES") {
            continue;
        }

        key_load_report.entries_seen++;

        const auto parts = Common::SplitString(line, '=');
        if (parts.size() != 2) {
            key_load_report.parse_errors++;
            LOG_ERROR(HW_AES, "Failed to parse {}", line);
            continue;
        }

        const std::string& name = parts[0];

        const auto nfc_secret = ParseNfcSecretName(name);
        if (nfc_secret) {
            auto value = HexToVector(parts[1]);
            if (nfc_secret->first >= nfc_secrets.size()) {
                key_load_report.parse_errors++;
                LOG_ERROR(HW_AES, "Invalid NFC secret index {}", nfc_secret->first);
            } else if (nfc_secret->second == "Phrase") {
                nfc_secrets[nfc_secret->first].phrase = value;
            } else if (nfc_secret->second == "Seed") {
                nfc_secrets[nfc_secret->first].seed = value;
            } else if (nfc_secret->second == "HmacKey") {
                nfc_secrets[nfc_secret->first].hmac_key = value;
            } else {
                key_load_report.parse_errors++;
                LOG_ERROR(HW_AES, "Invalid NFC secret '{}'", name);
            }
            continue;
        }

        AESKey key;
        try {
            key = HexToKey(parts[1]);
        } catch (const std::logic_error& e) {
            key_load_report.parse_errors++;
            LOG_ERROR(HW_AES, "Invalid key {}: {}", parts[1], e.what());
            continue;
        }

        const auto common_key = ParseCommonKeyName(name);
        if (common_key) {
            if (common_key >= common_key_y_slots.size()) {
                key_load_report.parse_errors++;
                LOG_ERROR(HW_AES, "Invalid common key index {}", common_key.value());
            } else {
                common_key_y_slots[common_key.value()] = key;
            }
            continue;
        }

        if (name == "generatorConstant") {
            generator_constant = key;
            key_load_report.generator_constant_loaded = true;
            continue;
        }

        if (name == "otpKey") {
            otp_key = key;
            continue;
        }

        if (name == "otpIV") {
            otp_iv = key;
            continue;
        }

        if (name == "movableKeyY") {
            movable_key.SetKeyY(key);
            continue;
        }

        if (name == "movableCmacY") {
            movable_cmac.SetKeyY(key);
            continue;
        }

        if (name == "dlpKeyY") {
            dlp_nfc_key_y_slots[DlpNfcKeyY::Dlp] = key;
            continue;
        }

        if (name == "nfcKeyY") {
            dlp_nfc_key_y_slots[DlpNfcKeyY::Nfc] = key;
            continue;
        }

        if (name == "nfcIv") {
            nfc_iv = key;
            continue;
        }

        if (name == "dlpChecksumModIv") {
            dlp_checksum_mod_iv = key;
            continue;
        }

        const auto key_slot = ParseKeySlotName(name);
        if (!key_slot) {
            key_load_report.parse_errors++;
            LOG_ERROR(HW_AES, "Invalid key name '{}'", name);
            continue;
        }

        if (key_slot->first >= MaxKeySlotID) {
            key_load_report.parse_errors++;
            LOG_ERROR(HW_AES, "Out of range key slot ID {:#X}", key_slot->first);
            continue;
        }

        switch (key_slot->second) {
        case 'X':
            raw_slot_keys[key_slot->first].x = key;
            key_slots.at(key_slot->first).SetKeyX(key);
            break;
        case 'Y':
            raw_slot_keys[key_slot->first].y = key;
            key_slots.at(key_slot->first).SetKeyY(key);
            break;
        case 'N':
            raw_slot_keys[key_slot->first].n = key;
            key_slots.at(key_slot->first).SetNormalKey(key);
            break;
        default:
            key_load_report.parse_errors++;
            LOG_ERROR(HW_AES, "Invalid key type '{}'", key_slot->second);
            break;
        }
    }
    if (generator_constant == AESKey{}) {
        TryDeriveGeneratorConstant();
    }

    if (key_load_report.file_found) {
        LOG_INFO(HW_AES, "Key file '{}': {} entries read, {} failed to parse, layout is {}",
                 key_load_report.file_name,
                 key_load_report.entries_seen - key_load_report.parse_errors,
                 key_load_report.parse_errors,
                 key_load_report.legacy_format ? "legacy Citra" : "sectioned");
        if (generator_constant == AESKey{}) {
            LOG_ERROR(HW_AES,
                      "The key generator constant is missing and could not be derived from the key "
                      "file. Encrypted titles cannot be decrypted without it.");
        }
    }
}


// Reads the user-provided key file. Keys are never bundled with the application and are never
// fetched from anywhere: this is the only place a key can enter the emulator.
void LoadKeysFile() {
    if (keys_data_cached) {
        return;
    }
    keys_data_cached = true;
    cached_keys_data.clear();
    key_load_report = KeyLoadReport{};

    const std::string sysdata_dir = FileUtil::GetUserPath(FileUtil::UserPath::SysDataDir);
    FileUtil::CreateFullPath(sysdata_dir);

    // AES_KEYS is the name GodMode9's DumpKeys script produces and is what users are told to
    // import; KEYS_FILE is the newer sectioned layout. Both are accepted, in that order.
    const std::array<const char*, 2> candidates{AES_KEYS, KEYS_FILE};

    std::string used_name;
    std::string contents;
    for (const char* name : candidates) {
        const std::string path = sysdata_dir + name;
        if (!FileUtil::Exists(path)) {
            continue;
        }

        FileUtil::IOFile file(path, "rb");
        if (!file.IsOpen()) {
            LOG_ERROR(HW_AES, "Key file '{}' exists but could not be opened", path);
            continue;
        }

        contents.resize(static_cast<std::size_t>(file.GetSize()));
        if (!contents.empty() &&
            file.ReadBytes(contents.data(), contents.size()) != contents.size()) {
            LOG_ERROR(HW_AES, "Failed to read key file '{}'", path);
            contents.clear();
            continue;
        }

        used_name = name;
        break;
    }

    if (used_name.empty()) {
        LOG_WARNING(HW_AES,
                    "No key file found in '{}'. Encrypted titles cannot be loaded until the user "
                    "imports their own '{}'.",
                    sysdata_dir, AES_KEYS);
        return;
    }

    if (contents.empty()) {
        LOG_ERROR(HW_AES, "Key file '{}{}' is empty", sysdata_dir, used_name);
        return;
    }

    key_load_report.file_found = true;
    key_load_report.file_name = used_name;

    if (contents.size() >= 3 && static_cast<u8>(contents[0]) == 0xEF &&
        static_cast<u8>(contents[1]) == 0xBB && static_cast<u8>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }

    // The sectioned layout groups entries under ":AES", ":RSA" and ":ECC" markers, and the parser
    // ignores everything until it sees one. A legacy file has no markers at all, so without this
    // it would be skipped in its entirety without a single error being reported.
    bool has_section_marker = false;
    for (const auto& raw_line : Common::SplitString(contents, '
')) {
        const std::string line = NormalizeKeyLine(raw_line);
        if (line.starts_with(":")) {
            has_section_marker = true;
            break;
        }
    }

    if (!has_section_marker) {
        key_load_report.legacy_format = true;
        contents.insert(0, ":AES
");
    }

    cached_keys_data = std::move(contents);
    LOG_INFO(HW_AES, "Reading keys from '{}{}' ({} bytes, {} layout)", sysdata_dir, used_name,
             cached_keys_data.size(), key_load_report.legacy_format ? "legacy Citra" : "sectioned");
}

} // namespace

std::istringstream GetKeysStream() {
    LoadKeysFile();
    return std::istringstream(cached_keys_data);
}

void InitKeys(bool force) {
    static bool initialized = false;
    if (initialized && !force) {
        return;
    }

    if (force) {
        // Drop everything so that a key file imported while the app is running fully replaces the
        // keys that were loaded before it.
        keys_data_cached = false;
        generator_constant = AESKey{};
        for (auto& slot : key_slots) {
            slot.Clear();
        }
        raw_slot_keys = {};
        common_key_y_slots = {};
        dlp_nfc_key_y_slots = {};
        movable_key.Clear();
        movable_cmac.Clear();
    }

    initialized = true;
    LoadPresetKeys();
    movable_key.SetKeyX(key_slots[0x35].x);
    movable_cmac.SetKeyX(key_slots[0x35].x);

    HW::RSA::InitSlots();
    HW::ECC::InitSlots();
}

void SetKeyX(std::size_t slot_id, const AESKey& key) {
    key_slots.at(slot_id).SetKeyX(key);
}

void SetKeyY(std::size_t slot_id, const AESKey& key) {
    key_slots.at(slot_id).SetKeyY(key);
}

void SetNormalKey(std::size_t slot_id, const AESKey& key) {
    key_slots.at(slot_id).SetNormalKey(key);
}

bool IsKeyXAvailable(std::size_t slot_id) {
    return key_slots.at(slot_id).x.has_value();
}

bool IsNormalKeyAvailable(std::size_t slot_id) {
    return key_slots.at(slot_id).normal.has_value();
}

AESKey GetNormalKey(std::size_t slot_id) {
    return key_slots.at(slot_id).normal.value_or(AESKey{});
}

void SelectCommonKeyIndex(u8 index) {
    key_slots[KeySlotID::TicketCommonKey].SetKeyY(common_key_y_slots.at(index));
}

void SelectDlpNfcKeyYIndex(u8 index) {
    key_slots[KeySlotID::DLPNFCDataKey].SetKeyY(dlp_nfc_key_y_slots.at(index));
}

bool NfcSecretsAvailable() {
    auto missing_secret =
        std::find_if(nfc_secrets.begin(), nfc_secrets.end(), [](auto& nfc_secret) {
            return nfc_secret.phrase.empty() || nfc_secret.seed.empty() ||
                   nfc_secret.hmac_key.empty();
        });
    SelectDlpNfcKeyYIndex(DlpNfcKeyY::Nfc);
    return IsNormalKeyAvailable(KeySlotID::DLPNFCDataKey) && missing_secret == nfc_secrets.end();
}

const NfcSecret& GetNfcSecret(NfcSecretId secret_id) {
    return nfc_secrets[secret_id];
}

const AESIV& GetNfcIv() {
    return nfc_iv;
}

std::pair<AESKey, AESIV> GetOTPKeyIV() {
    return {otp_key, otp_iv};
}

const AESKey& GetMovableKey(bool cmac_key) {
    return cmac_key ? movable_cmac.normal.value() : movable_key.normal.value();
}

const AESIV& GetDlpChecksumModIv() {
    return dlp_checksum_mod_iv;
}

const KeyLoadReport& GetKeyLoadReport() {
    InitKeys();
    return key_load_report;
}

bool IsGeneratorConstantAvailable() {
    InitKeys();
    return generator_constant != AESKey{};
}

std::vector<std::string> GetMissingNCCHKeyNames() {
    InitKeys();

    std::vector<std::string> missing;
    if (!IsGeneratorConstantAvailable()) {
        missing.emplace_back("generatorConstant");
    }

    const std::array<std::pair<std::size_t, const char*>, 4> ncch_slots{{
        {KeySlotID::NCCHSecure1, "slot0x2CKeyX"},
        {KeySlotID::NCCHSecure2, "slot0x25KeyX"},
        {KeySlotID::NCCHSecure3, "slot0x18KeyX"},
        {KeySlotID::NCCHSecure4, "slot0x1BKeyX"},
    }};
    for (const auto& [slot_id, name] : ncch_slots) {
        if (!IsKeyXAvailable(slot_id)) {
            missing.emplace_back(name);
        }
    }

    return missing;
}

} // namespace HW::AES

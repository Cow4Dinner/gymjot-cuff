#include "DeviceIdentity.h"
#include "Config.h"

#ifdef ARDUINO
#include <Preferences.h>
#include <esp_system.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace gymjot {
namespace {
constexpr const char* kNamespace = "cuffid";
constexpr const char* kKeyName = "name";
constexpr const char* kKeyId = "id";
constexpr const char* kKeyPass = "pass";

constexpr std::array<const char*, 128> kWords = {
    "amber", "arbor", "atlas", "azure", "balmy", "birch", "bluff", "brisk",
    "cairn", "cedar", "cider", "cobalt", "coral", "crisp", "dawn", "delta",
    "ember", "fable", "frost", "gale", "gleam", "glint", "grove", "harbor",
    "hazel", "hydra", "ionic", "ivory", "jolly", "jumbo", "keeps", "krona",
    "lanes", "lilac", "lumen", "lyric", "maple", "merit", "mirth", "misty",
    "noble", "nova", "nymph", "oaken", "olive", "onyx", "orbit", "oscar",
    "palis", "penny", "perch", "petal", "pinto", "plume", "prair", "quail",
    "quell", "quick", "quill", "quirk", "rally", "raven", "ridge", "river",
    "sable", "sage", "scout", "sepal", "shoal", "sienna", "silky", "solar",
    "sorrel", "sprig", "sugar", "swirl", "tango", "teal", "terra", "tidal",
    "tulip", "twine", "umber", "valor", "vapor", "verve", "vivid", "vulcan",
    "waltz", "wharf", "whisk", "willow", "wisp", "witty", "woven", "xenon",
    "yodel", "young", "yucca", "zenith", "zephy", "zesty", "alpha", "basil",
    "celes", "dingo", "easel", "ferns", "gusto", "hinge", "inlet", "jaunt",
    "kudos", "ledge", "magma", "nomad", "opal", "poppy", "radii", "shale",
    "topaz", "ultra", "vigor", "waver", "xeric", "yokel", "zonal", "auric"
};

constexpr std::array<char, 32> kBase32Alphabet = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K',
    'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'V', 'W', 'X',
    'Y', 'Z'
};

DeviceIdentity g_identity;
bool g_identityLoaded = false;

std::string encodeBase32(uint64_t value) {
    std::array<char, 14> buffer{};
    size_t pos = buffer.size();
    do {
        uint8_t index = value & 0x1F;
        buffer[--pos] = kBase32Alphabet[index];
        value >>= 5U;
    } while (value > 0 && pos > 0);
    return std::string(buffer.data() + pos, buffer.size() - pos);
}

std::string makeName(uint64_t id) {
    const size_t count = kWords.size();
    auto pick = [count](uint64_t value, uint8_t shift) {
        uint64_t idx = (value >> shift) % count;
        return kWords[idx];
    };
    std::string word1 = pick(id, 0);
    std::string word2 = pick(id, 16);
    std::string tail = encodeBase32(id);

    std::string name = "cuff-";
    name += word1;
    name += '-';
    name += word2;
    name += '-';
    name += tail;
    return name;
}

DeviceIdentity loadOrCreateIdentity() {
    Preferences prefs;
    prefs.begin(kNamespace, false);

    DeviceIdentity id;
    bool hasId = prefs.isKey(kKeyId);
    bool hasName = prefs.isKey(kKeyName);
    bool hasPass = prefs.isKey(kKeyPass);

    if (hasId) {
        id.deviceId = prefs.getULong64(kKeyId, 0ULL);
    }
    if (hasName) {
        id.name = prefs.getString(kKeyName, "").c_str();
    }
    if (hasPass) {
        id.passkey = prefs.getUInt(kKeyPass, 0U);
    }

    if (!hasId || id.deviceId == 0) {
        uint64_t upper = static_cast<uint64_t>(esp_random());
        uint64_t lower = static_cast<uint64_t>(esp_random());
        id.deviceId = (upper << 32) | lower;
        if (id.deviceId == 0) {
            id.deviceId = 1;
        }
        prefs.putULong64(kKeyId, id.deviceId);
    }

    if (!hasName || id.name.empty()) {
        id.name = makeName(id.deviceId);
        prefs.putString(kKeyName, id.name.c_str());
    }

    // Check if we need to update the passkey
    bool needsUpdate = !hasPass || id.passkey < 100000 || id.passkey > 999999;

#if defined(BLE_FIXED_PASSKEY) && BLE_FIXED_PASSKEY >= 100000 && BLE_FIXED_PASSKEY <= 999999
    // If using fixed passkey and stored passkey doesn't match, update it
    if (hasPass && id.passkey != BLE_FIXED_PASSKEY) {
        needsUpdate = true;
    }
#endif

    if (needsUpdate) {
        // Use fixed passkey if defined, otherwise generate random
#if defined(BLE_FIXED_PASSKEY) && BLE_FIXED_PASSKEY >= 100000 && BLE_FIXED_PASSKEY <= 999999
        id.passkey = BLE_FIXED_PASSKEY;
#else
        // Generate a random 6-digit passkey
        uint32_t value = esp_random() % 900000U;
        id.passkey = 100000U + value;
#endif
        prefs.putUInt(kKeyPass, id.passkey);
    }

    prefs.end();
    return id;
}

}  // namespace

const DeviceIdentity& deviceIdentity() {
    if (!g_identityLoaded) {
        g_identity = loadOrCreateIdentity();
        g_identityLoaded = true;
    }
    return g_identity;
}

void clearDeviceIdentity() {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.clear();
    prefs.end();
    g_identityLoaded = false;
    g_identity = DeviceIdentity{};
}

}  // namespace gymjot

#else

namespace gymjot {
namespace {
DeviceIdentity g_identity = {"cuff-test", 1, 123456};
}

const DeviceIdentity& deviceIdentity() {
    return g_identity;
}

void clearDeviceIdentity() {
    g_identity = DeviceIdentity{};
}

}  // namespace gymjot

#endif  // ARDUINO

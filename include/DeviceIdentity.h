#pragma once

#include <cstdint>
#include <string>

namespace gymjot {

struct DeviceIdentity {
    std::string name;
    uint64_t deviceId = 0;
    uint32_t passkey = 0;
};

const DeviceIdentity& deviceIdentity();
void clearDeviceIdentity();

}  // namespace gymjot

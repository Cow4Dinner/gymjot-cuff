# Firmware BLE Best Practices - Implementation Guide

## Critical Implementation Requirements

### 1. Discovery Without Encryption ⚠️ CRITICAL

**Problem**: Encrypted service discovery prevents connection establishment.

**Solution**: Always allow discovery without encryption. Only require encryption for actual data operations.

```cpp
// ❌ WRONG - Don't do this
g_commandChar = service->createCharacteristic(CHAR_RX_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);  // Blocks discovery!

// ✅ CORRECT - Allow discovery, encrypt operations
g_commandChar = service->createCharacteristic(CHAR_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);  // Write needs encryption, but discovery works

g_tx = service->createCharacteristic(CHAR_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);  // Discovery works, notifications can be unencrypted
```

**Implementation**:
- ✅ Service UUID: No encryption required
- ✅ Characteristic UUIDs: No encryption required
- ✅ Characteristic descriptors: No encryption required
- ⚠️ Writing to Control/Command: Require encryption (`WRITE_ENC`)
- ⚠️ Sensitive notifications: Require encryption if needed
- ✅ Device info reads: Can be unencrypted (name, firmware version)

### 2. Bond Hygiene APIs

**Requirement**: Users must be able to clear bonds and retry pairing.

#### Implementation A: Command-Based Bond Clearing

```cpp
// Add to DeviceCommand proto
message ClearBondsCommand {
    bool confirm = 1;  // Safety confirmation
}

// In processCommand()
case com_gymjot_cuff_DeviceCommand_clear_bonds_tag:
    if (cmd.command.clear_bonds.confirm) {
        Serial.println("Clearing all bonds...");
        NimBLEDevice::deleteAllBonds();

        // Send confirmation event
        com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
        evt.timestamp_ms = millis();
        evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;
        std::strncpy(evt.event.status.status_label, "bonds-cleared",
                     sizeof(evt.event.status.status_label) - 1);
        sendEvent(evt);

        Serial.println("All bonds cleared - ready for fresh pairing");
    }
    break;
```

#### Implementation B: Physical Button (Long Press)

```cpp
// Example with physical button on GPIO pin
#define BOND_CLEAR_BUTTON_PIN 0  // Boot button on ESP32-CAM
#define LONG_PRESS_DURATION_MS 5000

void checkBondClearButton() {
    static uint64_t pressStartTime = 0;
    static bool wasPressed = false;

    bool isPressed = (digitalRead(BOND_CLEAR_BUTTON_PIN) == LOW);

    if (isPressed && !wasPressed) {
        // Button just pressed
        pressStartTime = millis();
        wasPressed = true;
    } else if (!isPressed && wasPressed) {
        // Button released
        wasPressed = false;
        pressStartTime = 0;
    } else if (isPressed && wasPressed) {
        // Button held down
        if (millis() - pressStartTime >= LONG_PRESS_DURATION_MS) {
            Serial.println("LONG PRESS DETECTED - Clearing all bonds");
            NimBLEDevice::deleteAllBonds();

            // Visual feedback (blink LED, etc.)
            flashLED(3);

            wasPressed = false;  // Prevent multiple triggers
            Serial.println("Bonds cleared - release button");
        }
    }
}

// Call in loop()
void loop() {
    checkBondClearButton();
    // ... rest of loop
}
```

#### Implementation C: Auto-Recovery on Authentication Failure

```cpp
void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    g_clientConnected = false;

    Serial.println("=== BLE CLIENT DISCONNECTED ===");
    Serial.print("Reason code: 0x");
    Serial.println(reason, HEX);

    // Check for authentication/pairing failures
    bool pairingFailed = false;

    switch(reason) {
        case 0x05:  // BLE_HS_EAUTHEN - Authentication failure
            Serial.println("AUTHENTICATION FAILURE - Clearing bonds");
            pairingFailed = true;
            break;

        case 0x06:  // PIN or key missing
            Serial.println("PIN/KEY MISSING - Clearing bonds");
            pairingFailed = true;
            break;

        case 0x3D:  // Connection failed to establish (often pairing timeout)
            Serial.println("CONNECTION FAILED - May be pairing issue");
            pairingFailed = true;
            break;
    }

    if (pairingFailed) {
        Serial.println("Auto-clearing bonds to allow fresh pairing...");
        NimBLEDevice::deleteAllBonds();
    }

    // Always restart advertising
    Serial.print("Restarting advertising...");
    if (NimBLEDevice::startAdvertising()) {
        Serial.println("OK - Ready for fresh pairing");
    } else {
        Serial.println("FAILED");
    }
}
```

### 3. Whitelist/Privacy Management

**Requirement**: Don't block connections during discovery phase.

```cpp
static void setupBLE() {
    // ... existing setup ...

    // ✅ DO NOT enable whitelist until after successful bonding
    // NimBLEDevice::whiteListAdd(...);  // Don't do this at startup

    // ✅ Allow all connections during discovery
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    // Default behavior allows any central to connect

    Serial.println("Whitelist: DISABLED (allowing all connections for discovery)");
}

// After successful pairing (if you want to restrict)
void onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    if (connInfo.isBonded()) {
        // Optionally add to whitelist for future connections
        // NimBLEDevice::whiteListAdd(connInfo.getAddress());
        // Serial.println("Device added to whitelist");
    }
}
```

### 4. Security Request Flow

**Requirement**: Explicitly request encryption when needed, don't silently fail.

```cpp
class RxCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
        // Check if connection is encrypted
        if (!connInfo.isEncrypted()) {
            Serial.println("Write attempted on unencrypted connection");
            Serial.println("Requesting encryption...");

            // Request security upgrade
            NimBLEDevice::getServer()->startSecurity(connInfo.getConnHandle());

            // Optionally queue the command for processing after encryption
            Serial.println("Encryption requested - retry write after pairing");
            return;
        }

        // Process write normally
        std::string val = characteristic->getValue();
        if (val.size() < kLengthPrefixBytes) {
            Serial.println("<- command too short");
            return;
        }

        const auto* data = reinterpret_cast<const uint8_t*>(val.data());
        const uint16_t expected = static_cast<uint16_t>(data[0]) |
                                 (static_cast<uint16_t>(data[1]) << 8);
        const size_t available = val.size() - kLengthPrefixBytes;

        if (expected != available) {
            Serial.println("<- length mismatch");
            return;
        }

        processCommand(data + kLengthPrefixBytes, available);
    }
};
```

### 5. Status Telemetry

**Requirement**: Expose security/bonding status to mobile app.

#### Add to Protobuf

```protobuf
// In cuff.proto
message SecurityStatus {
    uint32 bonded_count = 1;           // Number of bonded devices
    string last_pair_fail_reason = 2;  // Last pairing failure reason
    uint32 sec_level_current = 3;      // Current security level (0=none, 1=encrypted, 2=authenticated)
    bool encryption_active = 4;        // Is current connection encrypted
    bool authentication_active = 5;    // Is current connection authenticated
    bool bonded = 6;                   // Is current connection bonded
}

// Add to SnapshotEvent
message SnapshotEvent {
    // ... existing fields ...
    SecurityStatus security = 20;
}
```

#### Implementation

```cpp
static uint32_t g_bondedDeviceCount = 0;
static std::string g_lastPairFailReason = "";

static void updateBondedDeviceCount() {
    g_bondedDeviceCount = NimBLEDevice::getNumBonds();
}

static void fillSnapshot(com_gymjot_cuff_SnapshotEvent& snapshot) {
    // ... existing fields ...

    // Add security status
    snapshot.has_security = true;
    snapshot.security.bonded_count = g_bondedDeviceCount;

    if (!g_lastPairFailReason.empty()) {
        std::strncpy(snapshot.security.last_pair_fail_reason,
                     g_lastPairFailReason.c_str(),
                     sizeof(snapshot.security.last_pair_fail_reason) - 1);
    }

    // Get current connection security level
    if (g_clientConnected && g_server) {
        auto connInfo = g_server->getPeerInfo(0);  // Get first connection
        if (connInfo.isEncrypted()) {
            snapshot.security.sec_level_current = connInfo.isAuthenticated() ? 2 : 1;
            snapshot.security.encryption_active = true;
            snapshot.security.authentication_active = connInfo.isAuthenticated();
            snapshot.security.bonded = connInfo.isBonded();
        } else {
            snapshot.security.sec_level_current = 0;
        }
    }
}

// Update on disconnect
void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    // ... existing code ...

    // Record failure reason for telemetry
    switch(reason) {
        case 0x05:
            g_lastPairFailReason = "Authentication failure";
            break;
        case 0x06:
            g_lastPairFailReason = "PIN/Key missing";
            break;
        case 0x3D:
            g_lastPairFailReason = "Connection failed to establish";
            break;
        default:
            if (reason != 0x13 && reason != 0x16) {  // Ignore normal disconnects
                g_lastPairFailReason = "Code 0x" + String(reason, HEX).c_str();
            }
            break;
    }

    updateBondedDeviceCount();
}
```

#### Manufacturer Data Pairing Bit

```cpp
static void setupBLE() {
    // ... existing setup ...

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    advData.setName(g_identity->name);
    advData.addServiceUUID(SERVICE_UUID);

    std::string mfg;
    mfg.push_back(static_cast<char>(MANUFACTURER_ID & 0xFF));
    mfg.push_back(static_cast<char>((MANUFACTURER_ID >> 8) & 0xFF));

    uint64_t deviceId = g_identity->deviceId;
    for (int i = 0; i < 8; ++i) {
        mfg.push_back(static_cast<char>((deviceId >> (8 * i)) & 0xFF));
    }

    // Add flags byte
    uint8_t flags = 0;

    // Bit 0: Pairing required for writes
    flags |= 0x01;  // Always require pairing for command writes

    // Bit 1: Device is bonded to at least one device
    if (NimBLEDevice::getNumBonds() > 0) {
        flags |= 0x02;
    }

    mfg.push_back(static_cast<char>(flags));

    advData.setManufacturerData(mfg);
    adv->setAdvertisementData(advData);
}
```

### 6. Connection Parameters & MTU Timing

**Requirement**: Don't change connection parameters until after pairing.

```cpp
class ServerCallback : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        g_clientConnected = true;
        g_lastConnectionTime = millis();

        Serial.println("=== BLE CLIENT CONNECTED ===");
        Serial.print("Client address: ");
        Serial.println(connInfo.getAddress().toString().c_str());

        // ❌ DON'T do connection parameter updates immediately
        // pServer->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 400);

        // ✅ DO wait for MTU negotiation first
        // MTU request happens in onMTUChange

        Serial.println("Waiting for MTU negotiation...");
    }

    void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
        Serial.print("MTU negotiated: ");
        Serial.println(MTU);

        // ❌ DON'T update connection params yet if encryption is required
        // Wait for pairing to complete

        if (connInfo.isEncrypted()) {
            // Already encrypted (re-connection with existing bond)
            optimizeConnectionParams(connInfo);
        } else {
            // Wait for pairing before optimizing
            Serial.println("Waiting for pairing before optimizing connection...");
        }
    }

    void optimizeConnectionParams(NimBLEConnInfo& connInfo) {
        Serial.println("Optimizing connection parameters...");

        // ✅ Safe to update now
        // min_interval: 12 * 1.25ms = 15ms
        // max_interval: 24 * 1.25ms = 30ms
        // latency: 0 (no slave latency)
        // timeout: 400 * 10ms = 4000ms (4 seconds) ✅ >= 4s requirement
        NimBLEDevice::getServer()->updateConnParams(
            connInfo.getConnHandle(),
            12,   // min interval
            24,   // max interval
            0,    // latency
            400   // supervision timeout (4 seconds)
        );
    }
};

// Track encryption state changes
static bool g_encryptionEstablished = false;

void checkEncryptionEstablished() {
    if (!g_clientConnected) {
        g_encryptionEstablished = false;
        return;
    }

    if (g_encryptionEstablished) {
        return;  // Already done
    }

    auto connInfo = NimBLEDevice::getServer()->getPeerInfo(0);
    if (connInfo.isEncrypted() && connInfo.isAuthenticated()) {
        g_encryptionEstablished = true;

        Serial.println("=== ENCRYPTION ESTABLISHED ===");

        // Now it's safe to optimize connection
        ServerCallback::optimizeConnectionParams(connInfo);

        // Send pairing success event
        com_gymjot_cuff_DeviceEvent evt = com_gymjot_cuff_DeviceEvent_init_default;
        evt.timestamp_ms = millis();
        evt.which_event = com_gymjot_cuff_DeviceEvent_status_tag;
        std::strncpy(evt.event.status.status_label, "pairing-success",
                     sizeof(evt.event.status.status_label) - 1);
        sendEvent(evt);
    }
}

// Call in loop()
void loop() {
    uint64_t now = millis();

    checkEncryptionEstablished();

    // ... rest of loop
}
```

## Summary Checklist

### Critical (Must Implement)

- [ ] ✅ Remove encryption requirement from service/characteristic discovery
- [ ] ✅ Keep encryption only on writes to Command RX characteristic
- [ ] ✅ Implement bond clearing command via BLE
- [ ] ✅ Auto-clear bonds on authentication failure (0x05, 0x06, 0x3D)
- [ ] ✅ Always resume advertising after disconnect
- [ ] ✅ Disable whitelist until after successful bonding
- [ ] ✅ Explicitly request encryption when command write is attempted
- [ ] ✅ Set supervision timeout ≥ 4 seconds (400 units)
- [ ] ✅ Don't update connection params until after pairing

### Important (Should Implement)

- [ ] ⚠️ Add security status to snapshot telemetry
- [ ] ⚠️ Add manufacturer data flags byte (pairing required bit)
- [ ] ⚠️ Track bonded device count
- [ ] ⚠️ Record last pairing failure reason
- [ ] ⚠️ Add physical button for bond clearing (optional)
- [ ] ⚠️ Delay MTU changes until after pairing

### Nice to Have

- [ ] 💡 Expose current security level in device info
- [ ] 💡 Log all security events to separate log
- [ ] 💡 Visual/audio feedback on pairing success/failure
- [ ] 💡 Allow selective bond deletion (not just all bonds)

## Testing Procedure

1. **Discovery Test**
   - Fresh device (no bonds)
   - Scan from mobile app
   - ✅ Should see service UUID immediately
   - ✅ Should discover all characteristics
   - ❌ Should NOT require pairing for discovery

2. **Pairing Flow Test**
   - Attempt to write to Command RX
   - ✅ Should trigger pairing request
   - Enter passkey `123456`
   - ✅ Should pair successfully
   - ✅ Command should then execute

3. **Auto-Recovery Test**
   - Pair device successfully
   - Delete bond from phone (but not device)
   - Attempt to reconnect
   - ❌ Will fail with authentication error
   - ✅ Device should auto-clear bonds
   - ✅ Should advertise again
   - ✅ Fresh pairing should succeed

4. **Bond Clearing Test**
   - Pair device successfully
   - Send `clear_bonds` command
   - ✅ Device should clear all bonds
   - ✅ Should report "bonds-cleared" status
   - ✅ Should be ready for fresh pairing

5. **Connection Params Test**
   - Monitor serial during connection
   - ✅ No param updates before pairing
   - ✅ MTU negotiation happens first
   - ✅ Param updates only after encryption
   - ✅ Supervision timeout is 4+ seconds

## Mobile App Implications

Your mobile app should:

1. **Always attempt discovery without encryption**
   - Don't require bonding before service discovery
   - Enumerate all services/characteristics freely

2. **Trigger pairing on first write**
   - When user attempts to send command
   - Handle pairing dialog
   - Retry write after pairing

3. **Handle bond clearing gracefully**
   - Detect authentication failures
   - Forget bond on phone side
   - Re-scan and re-pair

4. **Monitor security telemetry**
   - Check `security` field in snapshot
   - Show user if pairing is required
   - Display last pairing failure reason

5. **Respect connection timing**
   - Don't assume instant MTU negotiation
   - Wait for "pairing-success" event
   - Allow time for connection optimization

## Example Mobile Code

```kotlin
// Check if pairing is required before write
fun sendCommand(command: DeviceCommand) {
    val connInfo = getCurrentConnectionInfo()

    if (!connInfo.isEncrypted) {
        Log.d(TAG, "Connection not encrypted - will trigger pairing")
        // Attempt write anyway - device will request pairing
    }

    try {
        writeCommand(command)
    } catch (e: SecurityException) {
        Log.d(TAG, "Pairing required - waiting for user...")
        // Android will show pairing dialog
    }
}

// Handle authentication failure
override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
    if (newState == BluetoothProfile.STATE_DISCONNECTED) {
        when (status) {
            5, 6 -> {  // Authentication failure
                Log.d(TAG, "Authentication failed - forgetting bond")
                device.removeBond()  // Clear phone-side bond
                scheduleReconnection()  // Device auto-cleared, can re-pair
            }
        }
    }
}
```

## NimBLE-Specific Constants

```cpp
// NimBLE disconnect reason codes
#define BLE_HS_EAUTHEN      0x05  // Authentication failure
#define BLE_HS_EENCRYPT_KEY 0x06  // PIN or key missing
#define BLE_HS_ETIMEOUT     0x08  // Connection timeout
#define BLE_HS_EDONE        0x13  // Remote user terminated
#define BLE_HS_ENOTCONN     0x3D  // Connection failed to establish

// Security IO capabilities
#define BLE_HS_IO_DISPLAY_ONLY       0  // Device displays passkey
#define BLE_HS_IO_DISPLAY_YESNO      1  // Device displays and has yes/no
#define BLE_HS_IO_KEYBOARD_ONLY      2  // Device has keyboard
#define BLE_HS_IO_NO_INPUT_OUTPUT    3  // No display, no keyboard
#define BLE_HS_IO_KEYBOARD_DISPLAY   4  // Both keyboard and display
```

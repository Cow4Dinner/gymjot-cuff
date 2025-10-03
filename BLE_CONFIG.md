# GymJot Cuff - BLE Configuration Guide

## Current BLE Profile

The device uses the following BLE configuration:

### Service & Characteristics

| Component | UUID | Description |
|-----------|------|-------------|
| **Primary Service** | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Main GATT service |
| Command RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write commands (encrypted) |
| Event TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Read/notify events (encrypted) |
| Device Info | `6E400004-B5A3-F393-E0A9-E50E24DCCA9E` | Read device info (encrypted) |
| Snapshot | `6E400005-B5A3-F393-E0A9-E50E24DCCA9E` | Read/notify state snapshot (encrypted) |
| OTA | `6E400006-B5A3-F393-E0A9-E50E24DCCA9E` | Write OTA updates (encrypted) |

### Device Identity

The device automatically generates a unique identity on first boot:

- **Device ID**: Random 64-bit identifier
- **Device Name**: `cuff-{word1}-{word2}-{base32id}`
  - Example: `cuff-amber-azure-A1B2C3D4`
- **Passkey**: Fixed 6-digit PIN configured in firmware
  - **Default: `123456`**
  - Configurable via `BLE_FIXED_PASSKEY` in [include/Config.h](include/Config.h#L6)
  - Same passkey for all devices (simplifies user experience)

These values are stored in NVS (non-volatile storage) and persist across reboots.

**Note**: For development/testing, you can set `BLE_FIXED_PASSKEY` to `0` to use random passkeys (requires serial monitor to see the generated code).

### Advertising Configuration

| Parameter | Value | Notes |
|-----------|-------|-------|
| Manufacturer ID | `0xFFFF` | Included in manufacturer data |
| Device ID | 64-bit | Included in manufacturer data (little-endian) |
| Advertising Interval | 100-200ms | Min: 160 units, Max: 320 units (0.625ms each) |
| TX Power | -12 dBm | Lower power for reduced interference |
| Service UUID | Included | Primary service advertised |

### Security Settings

| Feature | Status | Description |
|---------|--------|-------------|
| Authentication | **REQUIRED** | Device requires pairing |
| Bonding | **REQUIRED** | Pairing information is stored |
| Encryption | **REQUIRED** | All characteristics require encryption |
| I/O Capability | NoInputNoOutput | Passkey entry via serial/display |
| MTU Size | 247 bytes | Maximum transmission unit |

## Configuration Files

### BLE Profile Configuration

The BLE profile can be configured via `data/ble_config.json`:

```json
{
  "profile": {
    "service_uuid": "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
    "manufacturer_id": 65535,
    "description": "GymJot Cuff BLE Profile"
  },
  "advertising": {
    "min_interval_ms": 100,
    "max_interval_ms": 200,
    "tx_power_dbm": -12
  },
  "connection": {
    "required_security": true,
    "require_encryption": true,
    "mtu_size": 247
  }
}
```

### Modifying the Configuration

To change UUIDs or settings:

1. Edit `include/Config.h`:
   ```cpp
   #define SERVICE_UUID "YOUR-SERVICE-UUID-HERE"
   #define CHAR_RX_UUID "YOUR-RX-UUID-HERE"
   // ... etc
   ```

2. Or update `data/ble_config.json` for runtime configuration

3. Rebuild and flash the firmware

## Pairing Process

### From Serial Monitor

When the device boots, the serial monitor displays:

```
=== BLE ADVERTISING STARTED ===
Device is now discoverable and ready to pair!

PAIRING INSTRUCTIONS:
1. Scan for BLE devices on your mobile app
2. Look for device: cuff-amber-azure-A1B2C3D4
3. When prompted, enter passkey: 123456  ← Fixed passkey (same for all devices)
4. Watch this serial output for connection status
===============================
```

**Production Note**: The passkey is `123456` by default. All devices use the same code for simplicity. To change it, edit `BLE_FIXED_PASSKEY` in [include/Config.h](include/Config.h#L6).

### Connection Events

The device logs detailed connection events to serial:

#### Successful Connection
```
=== BLE CLIENT CONNECTED ===
Client address: XX:XX:XX:XX:XX:XX
Connection ID: 0
MTU: 247
=== PASSKEY REQUESTED ===
Passkey: 123456
=== PAIRING SUCCESSFUL ===
Connection is encrypted
Connection is authenticated
Device is bonded
```

#### Connection Failure
```
=== BLE CLIENT DISCONNECTED ===
Reason code: 0x13 - Remote user terminated
Restarting advertising...OK
```

### Common Disconnect Reasons

| Code | Meaning | Likely Cause |
|------|---------|--------------|
| `0x08` | Connection timeout | Signal lost or out of range |
| `0x13` | Remote user terminated | User cancelled or app closed |
| `0x16` | Local host terminated | Device initiated disconnect |
| `0x3D` | Connection failed to establish | Pairing failed or timeout |
| `0x3E` | LMP response timeout | BLE stack communication issue |
| `0x22` | LMP error | Protocol error |

## Troubleshooting

### Device Not Discoverable

1. **Check Serial Output**
   ```
   === BLE ADVERTISING STARTED ===
   ```
   If this message doesn't appear, BLE failed to start.

2. **Verify Identity**
   ```
   Device name: cuff-amber-azure-A1B2C3D4
   Device ID: 0x123456789ABCDEF0
   Passkey: 123456
   ```

3. **Check Advertising Status**
   - If you see "ERROR: Failed to start advertising!", the BLE stack may be in an invalid state
   - Try power cycling the device

### Pairing Fails

1. **Wrong Passkey**
   - Default passkey is **`123456`**
   - If you changed `BLE_FIXED_PASSKEY`, use your custom code
   - Check serial monitor to verify the passkey being used

2. **Previously Bonded**
   - Device may be bonded to a previous phone
   - Send factory reset command or clear bonds via serial
   - Android: Go to Bluetooth settings → Forget device

3. **Security Requirements Not Met**
   - Ensure your BLE client supports:
     - Encryption
     - Authentication
     - Bonding

### Factory Reset

To reset device identity and clear all bonds:

1. Send `factory_reset` command with `confirm=true`
2. Or clear NVS via serial/platformio

### Connection Drops Frequently

1. **Check Signal Strength**
   - Move closer to device (< 10 meters)
   - Reduce obstacles between device and phone

2. **Verify Power**
   - Ensure adequate power supply
   - Check for brownouts in serial logs

3. **Check Watchdog**
   - Watchdog resets every 5 seconds
   - If device hangs, watchdog will reset after 30 seconds

## Diagnostic Output

### Boot Sequence
```
========================================
    GymJot Cuff - Booting
========================================
Firmware version: 0.1.0

Initializing watchdog timer (30s timeout)...
Watchdog enabled

Initializing hardware...
Camera: OK
AprilTag detector: OK

=== BLE INITIALIZATION ===
Device name: cuff-amber-azure-A1B2C3D4
Device ID: 0x123456789ABCDEF0
Passkey: 123456
Security settings:
  - Authentication: REQUIRED
  - Bonding: REQUIRED
  - Encryption: REQUIRED
  - IO Capability: NoInputNoOutput
  - MTU: 247 bytes
  - TX Power: -12 dBm

BLE Service created:
  - Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
  - Characteristics:
    * Command RX: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
    * Event TX: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
    * Info: 6E400004-B5A3-F393-E0A9-E50E24DCCA9E
    * Snapshot: 6E400005-B5A3-F393-E0A9-E50E24DCCA9E
    * OTA: 6E400006-B5A3-F393-E0A9-E50E24DCCA9E

Advertising configuration:
  - Manufacturer ID: 0xFFFF
  - Device ID in adv data: 0x123456789ABCDEF0
  - Min interval: 100ms (160 * 0.625ms)
  - Max interval: 200ms (320 * 0.625ms)
  - Service UUID included: YES

=== BLE ADVERTISING STARTED ===
Device is now discoverable and ready to pair!

========================================
    Boot Complete - System Ready
========================================
```

## Resilience Features

### Watchdog Timer
- **Timeout**: 30 seconds
- **Reset Interval**: Every 5 seconds in main loop
- **Purpose**: Automatic recovery from hangs or crashes

### Automatic Reconnection
- When disconnected, advertising automatically restarts
- No manual intervention required
- Device remains discoverable

### Error Reporting
- All connection events logged to serial
- Events sent via BLE notifications when connected
- Status labels: `ble-connected`, `pairing-success`, `ble-disconnected-0xXX`

## Mobile App Integration

Your mobile app should:

1. **Scan for devices** with service UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
2. **Filter by name prefix** `cuff-*`
3. **Extract device ID** from manufacturer data (bytes 2-9, little-endian)
4. **Use passkey `123456`** when pairing (or your custom `BLE_FIXED_PASSKEY`)
5. **Listen for status events** on Event TX characteristic
6. **Handle reconnection** on disconnect

### Example Event Processing

```javascript
// Subscribe to Event TX characteristic
characteristic.onNotification = (data) => {
  const event = parseProtobuf(data);
  if (event.status) {
    switch(event.status.status_label) {
      case "ble-connected":
        console.log("Device connected");
        break;
      case "pairing-success":
        console.log("Pairing successful");
        break;
      case "ble-disconnected-0x13":
        console.log("User terminated connection");
        break;
    }
  }
};
```

## Advanced Configuration

### Custom UUIDs

To use custom UUIDs for your own app:

1. Generate UUIDs using online tools or `uuidgen`
2. Update `include/Config.h`
3. Update your mobile app to match
4. Rebuild and flash firmware

### Custom Passkey

To use a different passkey:

1. Edit `include/Config.h`:
   ```cpp
   #define BLE_FIXED_PASSKEY 654321  // Your 6-digit code (100000-999999)
   ```

2. Rebuild and flash firmware

3. On next boot, device will automatically update to new passkey

4. Update your mobile app to use the new code

**To use random passkeys** (different per device, requires serial):
```cpp
#define BLE_FIXED_PASSKEY 0  // Random passkey on first boot
```

### Custom Manufacturer ID

To register a custom manufacturer ID:

1. Register with Bluetooth SIG (https://www.bluetooth.com/specifications/assigned-numbers/)
2. Update `MANUFACTURER_ID` in `include/Config.h`
3. Update mobile app to recognize your manufacturer ID

### Adjusting Security

⚠️ **Warning**: Reducing security may expose your device to attacks

To disable encryption (not recommended):

```cpp
// In setupBLE()
NimBLEDevice::setSecurityAuth(false, false, false);
g_commandChar = service->createCharacteristic(CHAR_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);  // Remove WRITE_ENC
```

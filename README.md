# GymJot Cuff Firmware

PlatformIO firmware for the GymJot BLE cuff built on the ESP32-CAM. The device captures AprilTag detections, tracks repetitions, and exposes a secure BLE interface for the companion mobile apps.

## Features
- Protobuf-based BLE protocol (command/event split) with 2-byte length framing
- Secure BLE pairing with per-device name, manufacturer ID, and static 6-digit passkey
- Configurable scan/test/loiter modes with AprilTag pipeline and simulated workout mode
- Persistent storage of FPS, loiter FPS, min travel, and idle thresholds in NVS
- Dynamic snapshot characteristic + event stream describing device state
- Power and factory-reset commands (deep sleep & bonding reset support)
- OTA command scaffolding with progress reporting (transfer handler stubbed)
- Unique human-readable advertising name (`cuff-{word}-{word}-{base32id}`)

## Build & Flash
```bash
# Build for ESP32-CAM
pio run -e esp32cam

# Flash to cuff on COM5 (adjust port as needed)
pio run -e esp32cam --target upload

# Monitor serial output (115200)
pio device monitor -b 115200
```

## Native Development & Tests
```bash
# Host build (logic only)
pio run -e native

# Run unit tests
pio test -e native
```

## Debugging Tips
- Serial logs show BLE packets (length + status messages) and AprilTag pipeline events.
- Use `pio device monitor` to observe boot/status/scan/rep events in human-readable form.
- To clear stored config or identity:
  - Issue a `FactoryResetCommand` (`confirm=true`) via BLE, **or**
  - Invoke `gymjot::clearDeviceIdentity()` and `gymjot::clearPersistentSettings()` (done automatically by the factory reset command).
- After a factory reset, the cuff regenerates its name, device ID, and passkey; bonding is required again before reconnecting.

## BLE Interface Overview
- **Advertising**
  - Device name: `cuff-{word}-{word}-{base32id}`
  - Manufacturer data: little-endian device ID prefixed with `MANUFACTURER_ID` (`0xFFFF`)
- **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- **Characteristics**
  - Commands (write w/ response, encrypted): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
  - Events (notify + read, encrypted): `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
  - Device Info (read, encrypted): `6E400004-B5A3-F393-E0A9-E50E24DCCA9E`
  - Snapshot (read/notify, encrypted): `6E400005-B5A3-F393-E0A9-E50E24DCCA9E`
  - OTA Staging (write, encrypted): `6E400006-B5A3-F393-E0A9-E50E24DCCA9E`
- **Payloads**: 2-byte little-endian length prefix followed by protobuf messages from `proto/cuff.proto`.
- **Security**: bonding + MITM via the static passkey printed on the cuff. After pairing, trusted reconnects occur automatically.

## Interfacing With the Firmware
1. **Discovery**
   - Scan for advertising names with the `cuff-` prefix.
   - Display the friendly name and manufacturer data ID so users pick the right cuff in dense environments.
2. **Pairing**
   - Initiate bonding and respond to Android's `ACTION_PAIRING_REQUEST` with the cuff's static passkey.
   - Cache the bonded device ID/name locally for frictionless reconnects.
3. **Command Flow**
   - Serialize `DeviceCommand` protobufs, prepend the 2-byte length prefix, and write to the command characteristic (write-with-response).
   - Commands cover FPS adjustments, threshold overrides, mode toggles, shutdown, snapshot requests, and factory reset.
4. **Event Handling**
   - Enable notifications on the events characteristic.
   - Decode the length-prefixed `DeviceEvent` payloads to react to status, scan, rep, snapshot, OTA status, and power updates.
5. **Snapshots & Info**
   - Read the Info characteristic for firmware name/device ID/OTA state.
   - Read or subscribe to the Snapshot characteristic for a structured snapshot of the cuff's current configuration.
6. **OTA (planned)**
   - `OtaBeginCommand`, `OtaChunkCommand`, and `OtaCompleteCommand` currently respond with an error status. Implement chunk streaming when OTA rollouts are ready.

For Android client implementation guidance (scan/connect/bond/serialize), see `docs/android_ble_prompt.md`.

## AprilTag Pipeline
- Real detections require AprilTag support (enabled by default).
- Test mode generates deterministic motion for UI/testing; enable via `SetTestModeCommand`.

## Reset & Power Commands
- **PowerCommand** with `shutdown=true`: emits a power event, stops advertising, and enters deep sleep. Wake requires external trigger.
- **FactoryResetCommand** with `confirm=true`: clears stored config + identity, deletes BLE bonds, and restarts the device.

## Project Structure Highlights
- `src/main.cpp` - BLE transport, command decoder, snapshot handling, device setup.
- `src/CuffController.cpp` - workout logic, AprilTag integration, event emission.
- `src/DeviceIdentity.cpp` - unique naming & passkey management.
- `src/PersistentConfig.cpp` - NVS-backed config store (with host fallbacks).
- `proto/cuff.proto` - canonical protobuf schema.
- `docs/android_ble_prompt.md` - ready-to-use prompt for Kotlin BLE integration.

---
Contributions welcome, especially around OTA implementation, diagnostics, or additional device behaviors.

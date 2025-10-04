# GymJot Cuff Firmware

PlatformIO firmware for the GymJot BLE cuff built on the ESP32-CAM. The device captures AprilTag detections, tracks repetitions, and exposes a secure BLE interface for the companion mobile apps.

## Quick Start
1. Clone the repository and install the PlatformIO CLI.
2. Connect the ESP32-CAM cuff over USB and flash with `pio run -e esp32cam --target upload`.
3. Use `pio device monitor -b 115200` to verify boot, note the advertised name and passkey.
4. Pair from the mobile app, subscribe to events, and start streaming reps.

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

## GATT Schema

```
┌─────────────────────────────────────────────────────────────────────┐
│ Service: GymJot Cuff Service                                        │
│ UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ Command RX Characteristic                                    │   │
│  │ UUID: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E                   │   │
│  │ Properties: WRITE, WRITE_NO_RESPONSE                         │   │
│  │ Security: WRITE_ENC (Encryption Required)                    │   │
│  │ Format: [2-byte length][DeviceCommand protobuf]              │   │
│  │                                                              │   │
│  │ Commands:                                                    │   │
│  │   • SetTestModeCommand        - Enable/disable test mode    │   │
│  │   • SetTargetFpsCommand        - Set camera FPS             │   │
│  │   • StationUpdateCommand       - Configure station metadata │   │
│  │   • ResetRepsCommand           - Reset rep counter          │   │
│  │   • PowerCommand               - Shutdown/sleep device      │   │
│  │   • FactoryResetCommand        - Factory reset device       │   │
│  │   • SnapshotRequestCommand     - Request device snapshot    │   │
│  │   • UpdateDeviceConfigCommand  - Update device settings     │   │
│  │   • OtaBeginCommand            - Begin OTA update           │   │
│  │   • OtaChunkCommand            - Send OTA data chunk        │   │
│  │   • OtaCompleteCommand         - Finalize OTA update        │   │
│  │   • ClearBondsCommand          - Clear bonding data         │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ Event TX Characteristic                                      │   │
│  │ UUID: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E                   │   │
│  │ Properties: NOTIFY, READ                                     │   │
│  │ Security: None (discovery allowed)                           │   │
│  │ Format: [2-byte length][DeviceEvent protobuf]                │   │
│  │                                                              │   │
│  │ Events:                                                      │   │
│  │   • BootEvent             - Device boot complete            │   │
│  │   • StatusEvent           - Device status update            │   │
│  │   • TagEvent              - AprilTag detected               │   │
│  │   • StationRequestEvent   - Station info requested          │   │
│  │   • StationBroadcastEvent - Station info broadcast          │   │
│  │   • ScanEvent             - AprilTag scan result            │   │
│  │   • RepEvent              - Repetition counted              │   │
│  │   • StationReadyEvent     - Station ready for use           │   │
│  │   • SnapshotEvent         - Device state snapshot           │   │
│  │   • OtaStatusEvent        - OTA update progress             │   │
│  │   • PowerEvent            - Power state change              │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ Device Info Characteristic                                   │   │
│  │ UUID: 6E400004-B5A3-F393-E0A9-E50E24DCCA9E                   │   │
│  │ Properties: READ                                             │   │
│  │ Security: None (public info)                                 │   │
│  │ Format: Plain text key=value pairs                          │   │
│  │                                                              │   │
│  │ Fields:                                                      │   │
│  │   • name  - Device name (cuff-word-word-id)                 │   │
│  │   • id    - 64-bit device ID (hex)                          │   │
│  │   • fw    - Firmware version string                         │   │
│  │   • ota   - OTA in progress (true/false)                    │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ Snapshot Characteristic                                      │   │
│  │ UUID: 6E400005-B5A3-F393-E0A9-E50E24DCCA9E                   │   │
│  │ Properties: READ, NOTIFY                                     │   │
│  │ Security: None (discovery allowed)                           │   │
│  │ Format: [2-byte length][SnapshotEvent protobuf]              │   │
│  │                                                              │   │
│  │ Contains:                                                    │   │
│  │   • device_id, name, mode, test_mode                        │   │
│  │   • target_fps, loiter_fps, min_travel_cm                   │   │
│  │   • max_rep_idle_ms, camera_ready                           │   │
│  │   • ota_in_progress, active_tag_id                          │   │
│  │   • BLE telemetry (MTU, conn params, etc.)                  │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │ OTA Characteristic (Legacy)                                  │   │
│  │ UUID: 6E400006-B5A3-F393-E0A9-E50E24DCCA9E                   │   │
│  │ Properties: WRITE, WRITE_NO_RESPONSE                         │   │
│  │ Security: WRITE_ENC (Encryption Required)                    │   │
│  │ Status: Deprecated - Use DeviceCommand OTA interface         │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                       │
└─────────────────────────────────────────────────────────────────────┘

Advertising Data:
  • Device Name: cuff-{word}-{word}-{base32id}
  • Manufacturer Data: [0xFFFF][8-byte device ID, little-endian]
  • Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E

Security:
  • Authentication: Required (MITM protection)
  • Bonding: Required
  • Encryption: Required for Command RX
  • Passkey: Static 6-digit (123456 or device-specific)
  • IO Capability: Display Only
```

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

## Screenshots
Screenshots live in `docs/screenshots/`. Suggested captures (add files as you collect them):
- ![BLE Scan](docs/screenshots/ble_scan.png) (mobile app discovering multiple cuffs).
- ![Pairing Prompt](docs/screenshots/pairing_prompt.png) (static passkey entry dialog).
- ![Session Dashboard](docs/screenshots/session_dashboard.png) (app displaying status and reps).

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

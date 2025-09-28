## Android BLE Prompt Cheat-Sheet

Give this prompt to an LLM coding assistant when working on the GymJot cuff Android client:

```
You are modifying a native Android app written in Kotlin.
Goal: connect to GymJot "cuff" BLE devices and exchange protobuf messages.

Device facts:
- Advertised name pattern: cuff-{word}-{word}-{base32id}
- Primary service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
- Characteristics:
  * Commands (write with response, encrypted): UUID 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
    - Payload = 2-byte little-endian length prefix + protobuf DeviceCommand message from proto/cuff.proto
  * Events (notify, encrypted): UUID 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
    - Notifications carry length-prefixed protobuf DeviceEvent messages
  * Device Info (read, encrypted): UUID 6E400004-B5A3-F393-E0A9-E50E24DCCA9E
    - Simple key=value text, fetch after connect for diagnostics
  * Snapshot (read/notify, encrypted): UUID 6E400005-B5A3-F393-E0A9-E50E24DCCA9E
    - Length-prefixed protobuf SnapshotEvent structure
  * OTA staging (write, encrypted): UUID 6E400006-B5A3-F393-E0A9-E50E24DCCA9E (not yet active)

Security:
- Device uses BLE bonding with MITM protection; pairing uses a static 6-digit passkey printed on the cuff label.
- Handle ACTION_PAIRING_REQUEST and send the passkey automatically from the UI/local store.
- After bonding, reconnect without new prompts; keep the BluetoothDevice reference and auto-connect on app launch.

Protocol reminders:
- Serialize/deserialize protobufs with `com.gymjot.cuff` Kotlin classes generated from proto/cuff.proto.
- All payloads are prefixed with uint16 length (little endian) before the raw protobuf bytes.
- Send DeviceCommand messages for FPS changes, mode toggles, shutdown, snapshot requests, etc.
- Listen for DeviceEvent notifications and update UI/state accordingly (status, scan, rep, snapshot, OTA status, power).

Implementation steps:
1. Request BLUETOOTH_SCAN/CONNECT permissions (API 31+) and enable Bluetooth.
2. Scan for devices whose name starts with "cuff-"; show friendly name + manufacturer data ID.
3. Let the user pick a cuff, initiate GATT connection, bond (passkey) if needed.
4. Discover services, obtain the characteristics above, enable notifications on Events and Snapshot.
5. Fetch Device Info and Snapshot, store passkey/bond data keyed by deviceId for auto reconnect.
6. Wrap all writes with the length prefix helper; use write-with-response for the Command characteristic.
7. Decode notifications, update repositories/view models, and surface errors from DeviceEvent.ota_status / power_event.
8. Provide a manual “factory reset” flow that warns the user it will require re-pairing afterward.
```

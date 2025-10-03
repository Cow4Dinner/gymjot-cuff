# Kotlin BLE Integration Guide for GymJot Cuff

## Overview

This guide provides complete instructions for integrating with the GymJot Cuff BLE device from a Kotlin Android app. The cuff firmware has been designed to be as forgiving and easy to work with as possible.

## Quick Reference

### BLE Profile Constants

```kotlin
object CuffBleProfile {
    // Primary Service
    const val SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"

    // Characteristics
    const val CHAR_COMMAND_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Write commands
    const val CHAR_EVENT_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"    // Receive events
    const val CHAR_INFO = "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"        // Device info
    const val CHAR_SNAPSHOT = "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"    // State snapshot
    const val CHAR_OTA = "6E400006-B5A3-F393-E0A9-E50E24DCCA9E"         // OTA updates

    // Advertising
    const val MANUFACTURER_ID = 0xFFFF
    const val DEVICE_NAME_PREFIX = "cuff-"

    // Protocol
    const val LENGTH_PREFIX_BYTES = 2  // All messages are length-prefixed (uint16 LE)
    const val MAX_PAYLOAD_SIZE = 512   // Max protobuf payload size

    // Pairing
    const val DEFAULT_PASSKEY = "123456"  // Fixed passkey (same for all devices)
}
```

## Connection Lifecycle Management

### State Machine

```kotlin
sealed class CuffConnectionState {
    object Disconnected : CuffConnectionState()
    object Scanning : CuffConnectionState()
    object Connecting : CuffConnectionState()
    object AwaitingPairing : CuffConnectionState()
    object Connected : CuffConnectionState()
    object Reconnecting : CuffConnectionState()
    data class Error(val reason: String, val code: Int? = null) : CuffConnectionState()
}
```

### Connection Manager

```kotlin
class CuffConnectionManager(
    private val context: Context,
    private val bluetoothAdapter: BluetoothAdapter
) {
    private var connectionState = MutableStateFlow<CuffConnectionState>(
        CuffConnectionState.Disconnected
    )

    private var bluetoothGatt: BluetoothGatt? = null
    private var targetDevice: BluetoothDevice? = null
    private var shouldReconnect = true

    // Exponential backoff for reconnection
    private var reconnectAttempt = 0
    private val reconnectDelays = listOf(1000L, 2000L, 5000L, 10000L, 30000L)

    // Characteristics cache
    private var commandChar: BluetoothGattCharacteristic? = null
    private var eventChar: BluetoothGattCharacteristic? = null
    private var snapshotChar: BluetoothGattCharacteristic? = null
    private var infoChar: BluetoothGattCharacteristic? = null

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
}
```

### Step-by-Step Connection Flow

#### 1. Scanning for Devices

```kotlin
fun startScan(onDeviceFound: (ScanResult) -> Unit) {
    connectionState.value = CuffConnectionState.Scanning

    val scanSettings = ScanSettings.Builder()
        .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
        .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
        .build()

    val scanFilter = ScanFilter.Builder()
        .setServiceUuid(ParcelUuid.fromString(CuffBleProfile.SERVICE_UUID))
        .build()

    val scanner = bluetoothAdapter.bluetoothLeScanner
    scanner.startScan(listOf(scanFilter), scanSettings, object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            // Cuff devices always start with "cuff-"
            if (result.device.name?.startsWith(CuffBleProfile.DEVICE_NAME_PREFIX) == true) {
                onDeviceFound(result)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            connectionState.value = CuffConnectionState.Error(
                "Scan failed: $errorCode",
                errorCode
            )
        }
    })
}

fun stopScan() {
    bluetoothAdapter.bluetoothLeScanner?.stopScan(scanCallback)
}
```

#### 2. Extracting Device Info from Scan Result

```kotlin
data class CuffDeviceInfo(
    val name: String,
    val address: String,
    val deviceId: Long,
    val rssi: Int
)

fun extractDeviceInfo(scanResult: ScanResult): CuffDeviceInfo? {
    val manufacturerData = scanResult.scanRecord
        ?.getManufacturerSpecificData(CuffBleProfile.MANUFACTURER_ID)

    if (manufacturerData == null || manufacturerData.size < 8) {
        return null
    }

    // Device ID is stored as 8 bytes in little-endian format after manufacturer ID
    val deviceId = ByteBuffer.wrap(manufacturerData)
        .order(ByteOrder.LITTLE_ENDIAN)
        .getLong()

    return CuffDeviceInfo(
        name = scanResult.device.name ?: "Unknown",
        address = scanResult.device.address,
        deviceId = deviceId,
        rssi = scanResult.rssi
    )
}
```

#### 3. Connecting to Device

```kotlin
fun connect(device: BluetoothDevice) {
    targetDevice = device
    shouldReconnect = true
    reconnectAttempt = 0

    connectionState.value = CuffConnectionState.Connecting

    // Connect with TRANSPORT_LE for BLE devices
    bluetoothGatt = device.connectGatt(
        context,
        false,  // autoConnect = false for faster connection
        gattCallback,
        BluetoothDevice.TRANSPORT_LE
    )
}

private val gattCallback = object : BluetoothGattCallback() {
    override fun onConnectionStateChange(
        gatt: BluetoothGatt,
        status: Int,
        newState: Int
    ) {
        when (newState) {
            BluetoothProfile.STATE_CONNECTED -> {
                Log.d(TAG, "Connected to GATT server")
                connectionState.value = CuffConnectionState.AwaitingPairing

                // Reset reconnect attempt counter on successful connection
                reconnectAttempt = 0

                // Request higher MTU for better throughput
                gatt.requestMtu(247)
            }

            BluetoothProfile.STATE_DISCONNECTED -> {
                Log.d(TAG, "Disconnected from GATT server. Status: $status")
                handleDisconnection(status)
            }
        }
    }

    override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
        Log.d(TAG, "MTU changed to $mtu")

        // Discover services after MTU negotiation
        gatt.discoverServices()
    }

    override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
        if (status == BluetoothGatt.GATT_SUCCESS) {
            val service = gatt.getService(UUID.fromString(CuffBleProfile.SERVICE_UUID))

            if (service == null) {
                connectionState.value = CuffConnectionState.Error(
                    "Service not found",
                    status
                )
                return
            }

            // Cache characteristics
            commandChar = service.getCharacteristic(
                UUID.fromString(CuffBleProfile.CHAR_COMMAND_RX)
            )
            eventChar = service.getCharacteristic(
                UUID.fromString(CuffBleProfile.CHAR_EVENT_TX)
            )
            snapshotChar = service.getCharacteristic(
                UUID.fromString(CuffBleProfile.CHAR_SNAPSHOT)
            )
            infoChar = service.getCharacteristic(
                UUID.fromString(CuffBleProfile.CHAR_INFO)
            )

            // Enable notifications on event and snapshot characteristics
            enableNotifications(gatt, eventChar)
            enableNotifications(gatt, snapshotChar)

            // Read device info
            infoChar?.let { gatt.readCharacteristic(it) }

            connectionState.value = CuffConnectionState.Connected
        } else {
            connectionState.value = CuffConnectionState.Error(
                "Service discovery failed",
                status
            )
        }
    }

    override fun onCharacteristicRead(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        status: Int
    ) {
        if (status == BluetoothGatt.GATT_SUCCESS) {
            when (characteristic.uuid.toString().uppercase()) {
                CuffBleProfile.CHAR_INFO.uppercase() -> {
                    val info = String(characteristic.value, Charsets.UTF_8)
                    Log.d(TAG, "Device info: $info")
                }
            }
        }
    }

    override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic
    ) {
        when (characteristic.uuid.toString().uppercase()) {
            CuffBleProfile.CHAR_EVENT_TX.uppercase() -> {
                handleEventNotification(characteristic.value)
            }
            CuffBleProfile.CHAR_SNAPSHOT.uppercase() -> {
                handleSnapshotNotification(characteristic.value)
            }
        }
    }
}
```

#### 4. Handling Pairing

```kotlin
// Register broadcast receiver for bonding state changes
private val bondStateReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val device = intent.getParcelableExtra<BluetoothDevice>(
            BluetoothDevice.EXTRA_DEVICE
        )

        if (device?.address != targetDevice?.address) return

        when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)) {
            BluetoothDevice.BOND_BONDING -> {
                Log.d(TAG, "Bonding in progress...")
                connectionState.value = CuffConnectionState.AwaitingPairing
            }

            BluetoothDevice.BOND_BONDED -> {
                Log.d(TAG, "Bonding successful")
                // Connection flow will continue in onServicesDiscovered
            }

            BluetoothDevice.BOND_NONE -> {
                Log.d(TAG, "Bonding failed or removed")
                connectionState.value = CuffConnectionState.Error(
                    "Pairing failed or cancelled"
                )
            }
        }
    }
}

// Register in onCreate/onStart
context.registerReceiver(
    bondStateReceiver,
    IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
)
```

**Passkey**: The cuff uses a **fixed passkey: `123456`** (same for all devices). Your app should:

```kotlin
// Use the default passkey
val passkey = CuffBleProfile.DEFAULT_PASSKEY  // "123456"

// Or make it configurable
val passkey = getString(R.string.cuff_passkey)  // In case firmware changes it
```

**Note**: The passkey is configurable via `BLE_FIXED_PASSKEY` in firmware's `include/Config.h`

#### 5. Enabling Notifications

```kotlin
private fun enableNotifications(
    gatt: BluetoothGatt,
    characteristic: BluetoothGattCharacteristic?
) {
    characteristic ?: return

    // Enable local notifications
    if (!gatt.setCharacteristicNotification(characteristic, true)) {
        Log.e(TAG, "Failed to enable notifications for ${characteristic.uuid}")
        return
    }

    // Enable remote notifications by writing to descriptor
    val descriptor = characteristic.getDescriptor(
        UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")  // CCCD
    )

    descriptor?.let {
        it.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        gatt.writeDescriptor(it)
    }
}
```

### Reconnection Strategy

```kotlin
private fun handleDisconnection(status: Int) {
    connectionState.value = when (status) {
        0 -> CuffConnectionState.Disconnected  // Clean disconnect
        8 -> CuffConnectionState.Error("Connection timeout", status)
        19 -> CuffConnectionState.Disconnected  // Remote disconnected
        133 -> CuffConnectionState.Error("GATT error", status)
        else -> CuffConnectionState.Error("Disconnected", status)
    }

    bluetoothGatt?.close()
    bluetoothGatt = null
    commandChar = null
    eventChar = null
    snapshotChar = null
    infoChar = null

    // Attempt reconnection if desired
    if (shouldReconnect && targetDevice != null) {
        scheduleReconnection()
    }
}

private fun scheduleReconnection() {
    connectionState.value = CuffConnectionState.Reconnecting

    val delay = reconnectDelays.getOrElse(reconnectAttempt) { reconnectDelays.last() }
    reconnectAttempt++

    Log.d(TAG, "Scheduling reconnection in ${delay}ms (attempt $reconnectAttempt)")

    scope.launch {
        delay(delay)
        targetDevice?.let { connect(it) }
    }
}

fun disconnect() {
    shouldReconnect = false
    bluetoothGatt?.disconnect()
}
```

## Message Protocol

### Sending Commands

All messages are length-prefixed with a 2-byte little-endian length header.

```kotlin
fun sendCommand(command: DeviceCommand) {
    val commandChar = this.commandChar ?: run {
        Log.e(TAG, "Command characteristic not available")
        return
    }

    val gatt = this.bluetoothGatt ?: run {
        Log.e(TAG, "GATT not connected")
        return
    }

    // Encode protobuf message
    val payload = command.toByteArray()

    if (payload.size > CuffBleProfile.MAX_PAYLOAD_SIZE) {
        Log.e(TAG, "Payload too large: ${payload.size} bytes")
        return
    }

    // Prepare length-prefixed message
    val message = ByteBuffer.allocate(2 + payload.size)
        .order(ByteOrder.LITTLE_ENDIAN)
        .putShort(payload.size.toShort())
        .put(payload)
        .array()

    // Write to characteristic
    commandChar.value = message
    commandChar.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT

    if (!gatt.writeCharacteristic(commandChar)) {
        Log.e(TAG, "Failed to write command")
    }
}
```

### Receiving Events

```kotlin
private fun handleEventNotification(data: ByteArray) {
    if (data.size < CuffBleProfile.LENGTH_PREFIX_BYTES) {
        Log.e(TAG, "Event too short: ${data.size} bytes")
        return
    }

    // Parse length prefix
    val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    val payloadLength = buffer.getShort().toInt() and 0xFFFF

    if (payloadLength != data.size - CuffBleProfile.LENGTH_PREFIX_BYTES) {
        Log.e(TAG, "Length mismatch: expected $payloadLength, got ${data.size - 2}")
        return
    }

    // Extract payload
    val payload = ByteArray(payloadLength)
    buffer.get(payload)

    // Decode protobuf
    try {
        val event = DeviceEvent.parseFrom(payload)
        handleDeviceEvent(event)
    } catch (e: Exception) {
        Log.e(TAG, "Failed to parse event", e)
    }
}

private fun handleDeviceEvent(event: DeviceEvent) {
    when (event.eventCase) {
        DeviceEvent.EventCase.STATUS -> {
            Log.d(TAG, "Status: ${event.status.statusLabel}")

            // Handle connection-related status events
            when (event.status.statusLabel) {
                "ble-connected" -> {
                    // Device acknowledged connection
                }
                "pairing-success" -> {
                    // Pairing completed successfully
                }
                "ble-disconnected-0x13" -> {
                    // Device reports disconnection
                }
            }
        }
        DeviceEvent.EventCase.BOOT -> {
            Log.d(TAG, "Device booted: mode=${event.boot.mode}, fps=${event.boot.fps}")
        }
        DeviceEvent.EventCase.SNAPSHOT -> {
            // Device state snapshot received
            updateDeviceState(event.snapshot)
        }
        DeviceEvent.EventCase.SCAN -> {
            // AprilTag scan data
            handleScanData(event.scan)
        }
        DeviceEvent.EventCase.REP -> {
            // Rep counted
            handleRepEvent(event.rep)
        }
        else -> {
            Log.d(TAG, "Unhandled event: ${event.eventCase}")
        }
    }
}
```

### Reading Characteristics

```kotlin
fun requestSnapshot() {
    val char = snapshotChar ?: return
    bluetoothGatt?.readCharacteristic(char)
}

fun readDeviceInfo(): String? {
    val char = infoChar ?: return null

    return if (bluetoothGatt?.readCharacteristic(char) == true) {
        // Value will be available in onCharacteristicRead callback
        null
    } else {
        null
    }
}
```

## Cuff-Side Helpers

The cuff firmware makes your life easier by:

### 1. **Automatic Advertising Restart**
- If disconnected, the cuff automatically restarts advertising
- No need to tell the device to become discoverable again
- Just scan and reconnect

### 2. **Persistent Bonding**
- Once paired, the cuff remembers your device
- Subsequent connections don't require re-pairing
- Use same passkey across app reinstalls

### 3. **Connection State Events**
- Cuff sends status events when:
  - Client connects: `"ble-connected"`
  - Pairing succeeds: `"pairing-success"`
  - Client disconnects: `"ble-disconnected-0xXX"`
- Subscribe to Event TX to monitor connection from cuff's perspective

### 4. **Watchdog Protection**
- If cuff hangs, it auto-reboots after 30 seconds
- Advertising restarts automatically
- Your app's reconnection logic will handle it

### 5. **Detailed Serial Diagnostics**
- Connect to serial at 115200 baud
- See exactly what's happening on cuff side
- Debug pairing issues easily

## Error Handling

### Common GATT Errors

```kotlin
fun getGattErrorMessage(status: Int): String = when (status) {
    0 -> "Success"
    8 -> "Connection timeout - device may be out of range"
    19 -> "Device disconnected"
    22 -> "Invalid handle - service discovery may have failed"
    133 -> "GATT error - try restarting Bluetooth"
    257 -> "Authentication failure - check passkey"
    else -> "Unknown error: $status"
}
```

### Handling Specific Failures

```kotlin
sealed class CuffError {
    object ServiceNotFound : CuffError()
    object CharacteristicNotFound : CuffError()
    data class PairingFailed(val reason: String) : CuffError()
    data class ConnectionTimeout(val attempts: Int) : CuffError()
    data class GattError(val status: Int) : CuffError()
}

fun handleError(error: CuffError) {
    when (error) {
        is CuffError.ServiceNotFound -> {
            // Service UUID mismatch or device in bad state
            // Solution: Disconnect, clear cache, reconnect
            clearGattCache()
            scheduleReconnection()
        }

        is CuffError.PairingFailed -> {
            // Wrong passkey or user cancelled
            // Solution: Prompt user to retry with correct passkey
            shouldReconnect = false
            showPairingInstructions()
        }

        is CuffError.ConnectionTimeout -> {
            // Device unreachable
            // Solution: Stop trying after max attempts
            if (error.attempts > 5) {
                shouldReconnect = false
                notifyUserDeviceUnreachable()
            }
        }

        is CuffError.GattError -> {
            // Android BLE stack issue
            // Solution: Clear cache and restart Bluetooth
            clearGattCache()
            if (error.status == 133) {
                suggestBluetoothRestart()
            }
        }
    }
}
```

### Clearing GATT Cache (Advanced)

```kotlin
// Sometimes needed to recover from GATT error 133
fun clearGattCache() {
    bluetoothGatt?.let { gatt ->
        try {
            val refresh = gatt.javaClass.getMethod("refresh")
            refresh.invoke(gatt)
            Log.d(TAG, "GATT cache cleared")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to clear GATT cache", e)
        }
    }
}
```

## Complete Example: Simple Connection Manager

```kotlin
class SimpleCuffManager(
    private val context: Context
) {
    private val bluetoothAdapter = BluetoothAdapter.getDefaultAdapter()
    private var gatt: BluetoothGatt? = null

    val connectionState = MutableStateFlow<CuffConnectionState>(
        CuffConnectionState.Disconnected
    )

    val deviceEvents = MutableSharedFlow<DeviceEvent>()

    fun scanAndConnect(onDeviceFound: (CuffDeviceInfo) -> Unit) {
        val scanner = bluetoothAdapter.bluetoothLeScanner
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid.fromString(CuffBleProfile.SERVICE_UUID))
            .build()

        scanner.startScan(listOf(filter), ScanSettings.Builder().build(),
            object : ScanCallback() {
                override fun onScanResult(callbackType: Int, result: ScanResult) {
                    if (result.device.name?.startsWith("cuff-") == true) {
                        extractDeviceInfo(result)?.let { onDeviceFound(it) }
                    }
                }
            }
        )
    }

    fun connect(device: BluetoothDevice) {
        gatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                when (newState) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        gatt.requestMtu(247)
                    }
                    BluetoothProfile.STATE_DISCONNECTED -> {
                        connectionState.value = CuffConnectionState.Disconnected
                    }
                }
            }

            override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
                gatt.discoverServices()
            }

            override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
                val service = gatt.getService(UUID.fromString(CuffBleProfile.SERVICE_UUID))
                val eventChar = service.getCharacteristic(
                    UUID.fromString(CuffBleProfile.CHAR_EVENT_TX)
                )

                enableNotifications(gatt, eventChar)
                connectionState.value = CuffConnectionState.Connected
            }

            override fun onCharacteristicChanged(
                gatt: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic
            ) {
                val data = characteristic.value
                if (data.size >= 2) {
                    val len = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
                        .getShort().toInt()
                    val payload = data.copyOfRange(2, 2 + len)

                    try {
                        val event = DeviceEvent.parseFrom(payload)
                        CoroutineScope(Dispatchers.IO).launch {
                            deviceEvents.emit(event)
                        }
                    } catch (e: Exception) {
                        Log.e("Cuff", "Parse error", e)
                    }
                }
            }
        })
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
    }
}
```

## Best Practices

### 1. **Always Use Exponential Backoff**
```kotlin
val delays = listOf(1000L, 2000L, 5000L, 10000L, 30000L)
```

### 2. **Request Higher MTU Early**
```kotlin
gatt.requestMtu(247)  // Do this before service discovery
```

### 3. **Enable Notifications on Both Event Characteristics**
```kotlin
enableNotifications(gatt, eventChar)
enableNotifications(gatt, snapshotChar)
```

### 4. **Handle Bonding State Changes**
```kotlin
// Register BroadcastReceiver for ACTION_BOND_STATE_CHANGED
```

### 5. **Implement Proper Cleanup**
```kotlin
override fun onDestroy() {
    gatt?.close()
    context.unregisterReceiver(bondStateReceiver)
}
```

### 6. **Use Length-Prefixed Messages**
```kotlin
// Always prepend 2-byte length (little-endian) to payloads
```

### 7. **Monitor Connection Quality**
```kotlin
// Track RSSI and notify user if signal is weak
gatt.readRemoteRssi()
```

## Troubleshooting

### "Service not found" Error
- Wait for `onServicesDiscovered` callback before accessing characteristics
- Don't cache UUIDs incorrectly (case-sensitive on some devices)

### "GATT Error 133"
- Clear GATT cache using reflection
- Suggest user restart Bluetooth
- This is an Android stack issue, not cuff firmware

### Pairing Fails Repeatedly
- Ensure user is entering correct 6-digit passkey from serial monitor
- Check if device is already bonded (remove old bond first)
- Verify Bluetooth permissions are granted

### Disconnects Immediately After Connection
- Check if cuff serial shows "Connection timeout"
- May indicate signal strength issue
- Try connecting closer to device

### No Notifications Received
- Verify CCCD descriptor was written successfully
- Check if `setCharacteristicNotification` returned true
- Ensure notifications are enabled on cuff side (they are by default)

## Testing Checklist

- [ ] Can discover device via service UUID filter
- [ ] Can extract device ID from manufacturer data
- [ ] Can connect and pair with passkey
- [ ] Bonding state persists across app restarts
- [ ] Receives notifications on Event TX characteristic
- [ ] Can send commands successfully
- [ ] Handles clean disconnection gracefully
- [ ] Reconnects automatically after signal loss
- [ ] Handles GATT error 133 with cache clear
- [ ] Shows user-friendly error messages
- [ ] Respects user's "disconnect" action (no auto-reconnect)

## Summary

The cuff firmware is designed to be **as easy as possible** to work with:

1. ✅ **Auto-advertises** after disconnect
2. ✅ **Sends connection events** so you know what's happening
3. ✅ **Auto-recovers** from hangs via watchdog
4. ✅ **Detailed logging** for debugging
5. ✅ **Standard BLE patterns** - no custom quirks
6. ✅ **Length-prefixed messages** - simple protocol
7. ✅ **All characteristics encrypted** - secure by default

Your Kotlin app just needs to:
1. Scan for service UUID
2. Connect with standard GATT callbacks
3. Enable notifications on Event TX and Snapshot
4. Send length-prefixed protobuf commands
5. Handle reconnection with exponential backoff

The cuff handles everything else automatically!

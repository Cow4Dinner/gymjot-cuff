# Quick Start: Connecting to GymJot Cuff from Mobile App

## 30-Second Integration

### 1. Add Dependencies (build.gradle.kts)

```kotlin
dependencies {
    implementation("com.google.protobuf:protobuf-javalite:3.21.12")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")
}
```

### 2. Add Permissions (AndroidManifest.xml)

```xml
<uses-permission android:name="android.permission.BLUETOOTH" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
```

### 3. Copy Constants

```kotlin
object Cuff {
    const val SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    const val RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
    const val TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
    const val INFO = "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"
    const val SNAPSHOT = "6E400005-B5A3-F393-E0A9-E50E24DCCA9E"
}
```

### 4. Scan and Connect

```kotlin
class CuffManager(private val context: Context) {
    private val adapter = BluetoothAdapter.getDefaultAdapter()
    private var gatt: BluetoothGatt? = null

    fun scan(onFound: (BluetoothDevice) -> Unit) {
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid.fromString(Cuff.SERVICE))
            .build()

        adapter.bluetoothLeScanner.startScan(
            listOf(filter),
            ScanSettings.Builder().build(),
            object : ScanCallback() {
                override fun onScanResult(type: Int, result: ScanResult) {
                    if (result.device.name?.startsWith("cuff-") == true) {
                        onFound(result.device)
                    }
                }
            }
        )
    }

    fun connect(device: BluetoothDevice, onEvent: (ByteArray) -> Unit) {
        gatt = device.connectGatt(context, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, s: Int, state: Int) {
                when (state) {
                    BluetoothProfile.STATE_CONNECTED -> {
                        g.requestMtu(247)
                    }
                }
            }

            override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
                g.discoverServices()
            }

            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                val service = g.getService(UUID.fromString(Cuff.SERVICE))
                val tx = service.getCharacteristic(UUID.fromString(Cuff.TX))

                // Enable notifications
                g.setCharacteristicNotification(tx, true)
                val desc = tx.getDescriptor(
                    UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
                )
                desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                g.writeDescriptor(desc)
            }

            override fun onCharacteristicChanged(
                g: BluetoothGatt,
                c: BluetoothGattCharacteristic
            ) {
                val data = c.value
                if (data.size >= 2) {
                    val len = ((data[1].toInt() and 0xFF) shl 8) or
                              (data[0].toInt() and 0xFF)
                    val payload = data.copyOfRange(2, 2 + len)
                    onEvent(payload)
                }
            }
        })
    }
}
```

### 5. Usage

```kotlin
val cuff = CuffManager(context)

// Scan
cuff.scan { device ->
    println("Found: ${device.name}")

    // Connect
    cuff.connect(device) { payload ->
        // Parse protobuf event
        val event = DeviceEvent.parseFrom(payload)
        when (event.eventCase) {
            DeviceEvent.EventCase.REP -> {
                println("Rep #${event.rep.repCount}")
            }
            DeviceEvent.EventCase.SCAN -> {
                println("Distance: ${event.scan.distanceCm} cm")
            }
        }
    }
}
```

## That's It!

The cuff handles:
- ✅ Auto-reconnection on disconnect
- ✅ Advertising restart
- ✅ Connection parameters
- ✅ Watchdog recovery

You just:
1. Scan for service UUID
2. Connect
3. Enable notifications
4. Parse protobuf events

## Full Example with Reconnection

```kotlin
class SimpleCuffClient(private val context: Context) {
    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private var shouldReconnect = true

    val events = MutableSharedFlow<DeviceEvent>()

    fun connect(device: BluetoothDevice) {
        this.device = device
        attemptConnection()
    }

    private fun attemptConnection() {
        gatt = device?.connectGatt(
            context,
            false,
            object : BluetoothGattCallback() {
                override fun onConnectionStateChange(
                    g: BluetoothGatt,
                    status: Int,
                    newState: Int
                ) {
                    when (newState) {
                        BluetoothProfile.STATE_CONNECTED -> {
                            g.requestMtu(247)
                        }
                        BluetoothProfile.STATE_DISCONNECTED -> {
                            if (shouldReconnect) {
                                // Reconnect after 2 seconds
                                Handler(Looper.getMainLooper()).postDelayed({
                                    attemptConnection()
                                }, 2000)
                            }
                        }
                    }
                }

                override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
                    g.discoverServices()
                }

                override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                    val service = g.getService(UUID.fromString(Cuff.SERVICE))
                    val tx = service?.getCharacteristic(UUID.fromString(Cuff.TX))

                    tx?.let {
                        g.setCharacteristicNotification(it, true)
                        val cccd = it.getDescriptor(
                            UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
                        )
                        cccd?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        g.writeDescriptor(cccd)
                    }
                }

                override fun onCharacteristicChanged(
                    g: BluetoothGatt,
                    c: BluetoothGattCharacteristic
                ) {
                    val data = c.value
                    if (data.size < 2) return

                    val len = ((data[1].toInt() and 0xFF) shl 8) or
                              (data[0].toInt() and 0xFF)

                    if (data.size < 2 + len) return

                    val payload = data.copyOfRange(2, 2 + len)

                    try {
                        val event = DeviceEvent.parseFrom(payload)
                        CoroutineScope(Dispatchers.IO).launch {
                            events.emit(event)
                        }
                    } catch (e: Exception) {
                        Log.e("Cuff", "Parse error", e)
                    }
                }
            }
        )
    }

    fun disconnect() {
        shouldReconnect = false
        gatt?.disconnect()
        gatt?.close()
    }
}
```

## Pairing

The cuff requires pairing with a **fixed 6-digit passkey**.

**Passkey: `123456`** (default, same for all devices)

Your app should:
```kotlin
// Hardcode the passkey (it's the same for all cuffs)
val CUFF_PASSKEY = "123456"

// Or make it configurable in case firmware changes it
val passkeyFromConfig = getString(R.string.cuff_passkey)  // "123456" in strings.xml
```

**Note**: The passkey is configurable via `BLE_FIXED_PASSKEY` in the firmware. If you change it, update your app accordingly.

## Sending Commands

```kotlin
fun sendCommand(command: DeviceCommand) {
    val service = gatt?.getService(UUID.fromString(Cuff.SERVICE))
    val rx = service?.getCharacteristic(UUID.fromString(Cuff.RX))

    val payload = command.toByteArray()
    val message = ByteBuffer.allocate(2 + payload.size)
        .order(ByteOrder.LITTLE_ENDIAN)
        .putShort(payload.size.toShort())
        .put(payload)
        .array()

    rx?.value = message
    rx?.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
    gatt?.writeCharacteristic(rx)
}

// Example: Request snapshot
val cmd = DeviceCommand.newBuilder()
    .setSnapshotRequest(SnapshotRequest.getDefaultInstance())
    .build()
sendCommand(cmd)
```

## Common Events

```kotlin
fun handleEvent(event: DeviceEvent) {
    when (event.eventCase) {
        DeviceEvent.EventCase.BOOT -> {
            // Device just booted
            println("Device ready, mode=${event.boot.mode}")
        }

        DeviceEvent.EventCase.STATUS -> {
            // Connection status or state change
            when (event.status.statusLabel) {
                "ble-connected" -> println("Connected confirmed")
                "pairing-success" -> println("Paired successfully")
                "scanning" -> println("Device is scanning for tags")
            }
        }

        DeviceEvent.EventCase.SCAN -> {
            // AprilTag detection
            println("Tag ${event.scan.tagId} at ${event.scan.distanceCm}cm")
        }

        DeviceEvent.EventCase.REP -> {
            // Rep counted
            println("Rep #${event.rep.repCount}")
        }

        DeviceEvent.EventCase.SNAPSHOT -> {
            // Full device state
            println("Mode: ${event.snapshot.mode}")
            println("FPS: ${event.snapshot.targetFps}")
            println("Active tag: ${event.snapshot.activeTagId}")
        }
    }
}
```

## Troubleshooting

### "Device not found"
- Make sure device is powered on
- Check serial monitor shows "BLE ADVERTISING STARTED"
- Verify location permissions are granted

### "Connection failed"
- Device may already be connected to another phone
- Try restarting Bluetooth on phone
- Check if device shows "BLE CLIENT CONNECTED" in serial

### "Pairing failed"
- Use passkey **`123456`** (default for all devices)
- Remove old pairing from phone's Bluetooth settings
- If firmware was customized, check `BLE_FIXED_PASSKEY` value

### "No events received"
- Check if notifications were enabled successfully
- Verify CCCD descriptor write returned success
- Look for "notify failed" in device's serial output

## Next Steps

For complete details, see:
- **[KOTLIN_BLE_INTEGRATION.md](KOTLIN_BLE_INTEGRATION.md)** - Full integration guide
- **[BLE_CONFIG.md](BLE_CONFIG.md)** - BLE profile documentation
- **[README.md](README.md)** - Overall project documentation

## LLM Instructions

When implementing BLE connectivity for GymJot Cuff:

1. **Use the constants** from this guide exactly as shown
2. **Follow the connection flow**: Scan → Connect → MTU → Discover → Enable Notifications
3. **Handle reconnection** with exponential backoff (1s, 2s, 5s, 10s, 30s)
4. **Parse length-prefixed messages**: First 2 bytes are payload length (little-endian)
5. **Don't worry about advertising** - the cuff handles it automatically
6. **Monitor connection events** - the cuff tells you what's happening
7. **Trust the watchdog** - device will auto-recover from hangs

The cuff firmware is designed to be **maximally forgiving**:
- Auto-restarts advertising on disconnect
- Sends status events for debugging
- Handles MTU negotiation automatically
- Recovers from crashes via watchdog
- Logs everything to serial for troubleshooting

Your app just needs to **connect, enable notifications, and parse events**. Everything else is handled!

# Passkey Configuration Update

## Summary

The GymJot Cuff now uses a **fixed passkey** instead of random passkeys, eliminating the need for serial monitor access during pairing.

## Default Passkey

**`123456`** (same for all devices)

## Why Fixed Passkey?

✅ **No serial access required** - Users don't need to connect to serial monitor
✅ **Same for all devices** - Simplified user experience
✅ **Easy mobile integration** - App can hardcode the passkey
✅ **Still secure** - Requires encryption, authentication, and bonding

## Configuration

### Location
[include/Config.h:6](include/Config.h#L6)

```cpp
#define BLE_FIXED_PASSKEY 123456  // Fixed passkey for production (no serial needed)
```

### To Change the Passkey

1. Edit `include/Config.h`:
   ```cpp
   #define BLE_FIXED_PASSKEY 654321  // Your 6-digit code (100000-999999)
   ```

2. Rebuild and flash firmware

3. **Device automatically updates** on next boot (no factory reset needed!)

4. Update mobile app to match

### To Use Random Passkeys (Development)

```cpp
#define BLE_FIXED_PASSKEY 0  // Random passkey, check serial monitor
```

## Mobile App Integration

### Kotlin/Android

```kotlin
object CuffBleProfile {
    const val DEFAULT_PASSKEY = "123456"  // Fixed passkey
}

// Use it during pairing
val passkey = CuffBleProfile.DEFAULT_PASSKEY
```

### strings.xml (Recommended)

```xml
<resources>
    <string name="cuff_passkey">123456</string>
</resources>
```

```kotlin
val passkey = getString(R.string.cuff_passkey)
```

## How It Works

### Firmware Side ([src/DeviceIdentity.cpp:112-132](src/DeviceIdentity.cpp#L112-L132))

1. On boot, check if stored passkey matches `BLE_FIXED_PASSKEY`
2. If mismatch (or first boot), update to `BLE_FIXED_PASSKEY`
3. Store in NVS for persistence
4. Use it for all BLE pairing requests

```cpp
// Automatically updates passkey if it doesn't match
#if defined(BLE_FIXED_PASSKEY) && BLE_FIXED_PASSKEY >= 100000 && BLE_FIXED_PASSKEY <= 999999
    if (hasPass && id.passkey != BLE_FIXED_PASSKEY) {
        needsUpdate = true;
    }
#endif
```

## Serial Output

The passkey is still displayed in serial output for debugging:

```
=== BLE INITIALIZATION ===
Device name: cuff-amber-azure-A1B2C3D4
Device ID: 0x123456789ABCDEF0
Passkey: 123456          ← Confirmed fixed passkey

PAIRING INSTRUCTIONS:
1. Scan for BLE devices on your mobile app
2. Look for device: cuff-amber-azure-A1B2C3D4
3. When prompted, enter passkey: 123456  ← Fixed passkey (same for all devices)
```

## Migration from Random Passkey

If you already have devices with random passkeys:

### Option 1: Automatic Update (Recommended)
1. Flash the updated firmware
2. **Device auto-updates** passkey to `123456` on next boot
3. No user action required

### Option 2: Factory Reset
1. Send factory reset command via BLE
2. Or use: `pio run -t erase`
3. Device generates fresh identity with fixed passkey

## Security Considerations

### Still Secure ✅
- **Encryption**: All characteristics encrypted
- **Authentication**: Required for pairing
- **Bonding**: Pairing info stored securely
- **Protection**: Passkey + encryption prevents casual eavesdropping

### Trade-offs
- ⚠️ **Same passkey for all devices** - If someone knows the firmware, they know the passkey
- ⚠️ **Less unique** - Can't distinguish devices by passkey
- ✅ **Acceptable for consumer devices** - Similar to many Bluetooth headphones, fitness trackers
- ✅ **User-friendly** - No need to manage per-device passkeys

### When to Use Random Passkeys
- High-security environments
- Development/testing (to test pairing flows)
- When you want unique per-device passkeys
- Set `BLE_FIXED_PASSKEY 0`

## Documentation Updated

All documentation now reflects the fixed passkey approach:

- ✅ [BLE_CONFIG.md](BLE_CONFIG.md) - Updated identity section
- ✅ [KOTLIN_BLE_INTEGRATION.md](KOTLIN_BLE_INTEGRATION.md) - Added DEFAULT_PASSKEY constant
- ✅ [QUICKSTART_MOBILE.md](QUICKSTART_MOBILE.md) - Simplified pairing instructions
- ✅ [IMPLEMENTATION_SUMMARY.md](IMPLEMENTATION_SUMMARY.md) - Overview updated

## Example Usage

### User Experience

**Without Fixed Passkey (OLD):**
1. User: "Connect to cuff"
2. App: "Check the device serial monitor for passkey"
3. User: *Needs serial cable, terminal, etc.*
4. User: Enters passkey from serial
5. ❌ Poor experience

**With Fixed Passkey (NEW):**
1. User: "Connect to cuff"
2. App: *Automatically uses passkey `123456`*
3. User: Pairing happens seamlessly
4. ✅ Great experience

## Customization Examples

### Production with Custom Passkey

```cpp
// include/Config.h
#define BLE_FIXED_PASSKEY 987654  // Your brand's passkey
```

Update app:
```kotlin
const val DEFAULT_PASSKEY = "987654"  // Match firmware
```

### Multiple Product Lines

```cpp
// Cuff Pro
#define BLE_FIXED_PASSKEY 111111

// Cuff Lite
#define BLE_FIXED_PASSKEY 222222
```

App can detect product type and use appropriate passkey.

## Testing

### Verify Passkey Update

1. Build and flash firmware
2. Connect serial monitor (115200 baud)
3. Boot device
4. Look for:
   ```
   Passkey: 123456  ← Should show your BLE_FIXED_PASSKEY
   ```

### Test Pairing

1. Open mobile app
2. Scan for device
3. Attempt pairing
4. Use passkey `123456` (or your custom value)
5. Should pair successfully without serial access

## Questions?

- **Why 123456?** Easy to remember, same as many consumer devices
- **Can I change it?** Yes! Edit `BLE_FIXED_PASSKEY` in Config.h
- **Is it secure enough?** Yes, for consumer fitness devices. Encryption + passkey is standard
- **What if I want unique per-device passkeys?** Set `BLE_FIXED_PASSKEY 0`

## Files Modified

- [include/Config.h](include/Config.h#L6) - Added `BLE_FIXED_PASSKEY` define
- [src/DeviceIdentity.cpp](src/DeviceIdentity.cpp#L112-L132) - Auto-update logic
- [BLE_CONFIG.md](BLE_CONFIG.md) - Documentation updates
- [KOTLIN_BLE_INTEGRATION.md](KOTLIN_BLE_INTEGRATION.md) - Mobile integration
- [QUICKSTART_MOBILE.md](QUICKSTART_MOBILE.md) - Quick start updates

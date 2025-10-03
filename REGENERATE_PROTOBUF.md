# Protobuf Regeneration Instructions

## ⚠️ Action Required

The `proto/cuff.proto` file has been updated with a new `ClearBondsCommand`. You **must regenerate** the C headers before compiling.

## What Was Added

```protobuf
message ClearBondsCommand {
  bool confirm = 1;
}

message DeviceCommand {
  oneof command {
    // ... existing commands ...
    ClearBondsCommand clear_bonds = 21;  // NEW
  }
}
```

## How to Regenerate

### Option 1: Python Script (Recommended)

```bash
cd c:\Users\Scott\Desktop\gymjot-cuff
python scripts/generate_proto.py
```

### Option 2: nanopb CLI

```bash
cd c:\Users\Scott\Desktop\gymjot-cuff
nanopb_generator proto/cuff.proto --output-dir=include/proto
```

### Option 3: PlatformIO (if configured)

```bash
cd c:\Users\Scott\Desktop\gymjot-cuff
pio run -t nanopb
```

### Option 4: Manual Docker (if installed)

```bash
cd c:\Users\Scott\Desktop\gymjot-cuff
docker run --rm -v "$(pwd):/workspace" nanopb/protoc \
  --nanopb_out=include/proto proto/cuff.proto
```

## What Gets Generated

The regeneration will update:
- `include/proto/cuff.pb.h` - C struct definitions
- `include/proto/cuff.pb.c` - Encoding/decoding functions

## Compilation Will Fail Without This

If you try to compile without regenerating, you'll see errors like:

```
src/main.cpp:833: error: 'com_gymjot_cuff_DeviceCommand_clear_bonds_tag' was not declared
```

## After Regeneration

1. Verify files are updated:
   ```bash
   ls -l include/proto/cuff.pb.*
   ```

2. Compile firmware:
   ```bash
   pio run -e esp32cam
   ```

3. Upload:
   ```bash
   pio run -e esp32cam -t upload
   ```

## Mobile App Integration

Your Kotlin mobile app will also need the updated proto:

```kotlin
val clearBondsCmd = DeviceCommand.newBuilder()
    .setClearBonds(ClearBondsCommand.newBuilder()
        .setConfirm(true)
        .build())
    .build()

sendCommand(clearBondsCmd)
```

After sending, listen for:
```kotlin
if (event.status.statusLabel == "bonds-cleared") {
    // Device cleared all bonds
    device.removeBond()  // Clear phone-side bond too
}
```

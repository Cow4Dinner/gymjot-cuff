# GymJot ESP32-CAM Firmware

PlatformIO firmware for ESP32-CAM that communicates with the GymJot mobile app.

## Features
- BLE Nordic UART JSON protocol
- Test mode (simulates AprilTag + reps)
- Real AprilTag hooks (compile with -DUSE_APRILTAG=1)
- Modes: Discovery → Scanning → Loiter (low FPS)

## BLE Protocol
**Device → Phone:**
```json
{"type":"status","status":"boot"}
{"type":"scan","tagId":16,"distanceCm":72.4}
{"type":"rep","count":3}
```

**Phone → Device:**
```json
{"cmd":"test","on":true}
{"cmd":"fps","value":8}
{"cmd":"station","id":"A16","name":"Back Squat"}
```

## Build
```
pio run --target upload
pio device monitor
```

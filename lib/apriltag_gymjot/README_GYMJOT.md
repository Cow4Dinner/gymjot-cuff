apriltag fork for gymjot cuff.

Based on upstream apriltag 3.3.0 with the following changes:
- tagCircle49h12.c: mark codedata table as const so it lives in flash on ESP32.

To update:
1. Pull upstream apriltag sources.
2. Copy over to lib/apriltag_gymjot.
3. Reapply the const change in tagCircle49h12.c if still needed.

# Changes in this fork

What Alpas Made changed on top of upstream xiaozhi-esp32, one heading per
day. The product's own changelog is in the Jakomotchi repository.

## 2026-11-04

- Waveshare ESP32-S3 Touch AMOLED 1.75 and 1.75C: Jako's face as an LVGL widget in the board folder (jako_face.c, its header, port header and heart masks), driven by the device state, the server's emotion names, the user's words, alerts, power save and the audio level of every frame played, in place of the emoji, status bar and subtitle. Display gains two no-op virtuals, SetDeviceState and SetAudioLevel; AudioService gains an on_output_level callback with the frame's RMS. No other board changes.

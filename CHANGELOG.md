# Changes in this fork

What Alpas Made changed on top of upstream xiaozhi-esp32, one heading per
day. The product's own changelog is in the Jakomotchi repository.

## 2026-11-05

- Waveshare ESP32-S3 Touch AMOLED 1.75 and 1.75C: Jako's listening ring is the studio's neon #EEFF75 in place of the green #C7F23F (the listening entry of STATE_COLOR in jako_face.c, matching the face engine in the Jakomotchi repository). The thinking, speaking and error colours, the ring's geometry and its timing are unchanged.

## 2026-11-04

- Waking Jako: the 1.75C build entry carries a custom_wake_word object ("hey jako", English, threshold 20) and scripts/build.py understands it (the command recogniser with the English MultiNet7 model; an explicit --wake-word still wins; the values are checked in the generated sdkconfig; tests in scripts/tests). CustomWakeWord::Initialize fails loudly when the model is missing or the phrase is rejected and logs the derived phonemes on success; AfeAudioEngine then runs without a wake word instead of giving up the audio path. AudioService gains SetWakeWordAllowed, one gate for every caller; Application gains a stored voice wake switch (NVS namespace wake, key voice, default on) flipped by the 1.75 board's double click with a sound as the cue, replacing the echo cancellation toggle. The 1.75 board no longer powers off after five idle minutes (dim at 60 s only) and applies its first battery read.
- Waveshare ESP32-S3 Touch AMOLED 1.75 and 1.75C: Jako's face as an LVGL widget in the board folder (jako_face.c, its header, port header and heart masks), driven by the device state, the server's emotion names, the user's words, alerts, power save and the audio level of every frame played, in place of the emoji, status bar and subtitle. Display gains two no-op virtuals, SetDeviceState and SetAudioLevel; AudioService gains an on_output_level callback with the frame's RMS. No other board changes.

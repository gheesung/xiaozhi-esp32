# BOX-3 idle clock face and emoji set

Everything here is board-local to `main/boards/espressif/esp32-s3-box-3`. No stock file outside the
board directory is replaced, so the same tree still builds for other boards.

## What was added

| File | Purpose |
|---|---|
| `main/boards/espressif/esp32-s3-box-3/idle_face.{h,cc}` | LVGL idle widget: fixed-width clock (six digit cells so `HH:MM` never shifts as the seconds tick), NEA weather + PSI, battery, scrolling news ticker, base64 screenshot dump over serial for layout debugging |
| `main/boards/espressif/esp32-s3-box-3/esp_box3_board.cc` | `Box3LcdDisplay::SetupUI()` creates the face last so it is topmost; `UpdateStatusBar()` (the 1 s clock tick) drives `Refresh()`; `GetBatteryLevel()` reads the dock's 18650 |
| `main/boards/espressif/esp32-s3-box-3/sensor_dock.{h,cc}` | ESP32-S3-BOX-3-SENSOR dock: AHT20, battery ADC, IR receive, radar presence, device-side MCP tools |
| `main/display/lvgl_display/lvgl_display.{h,cc}` | `GetStatusExtraText()` hook so the board can append text next to the status-bar clock |
| `sdkconfig.defaults.esp32s3` | LVGL Montserrat 20/28/48 (the clock needs 48) |
| `tools/box3-emoji/gen_emoji.py`, `gifs/` | Generator and the 21 animated 128×128 emoji that replace the stock face set |

## Layout facts worth keeping

- The face is `320x212` at `y=28` (`LV_VER_RES` minus the status bar). It is an ordinary child of
  `lv_screen_active()` created after `emoji_box_`/`preview_image_`/`bottom_bar_`, which is why it
  covers them without touching `lvgl_display.cc`.
- `MAIN_EVENT_CLOCK_TICK` is the only once-per-second hook; the face hides itself whenever
  `GetDeviceState() != kDeviceStateIdle`.
- Weather comes from NEA open data (`api.data.gov.sg`, no key): `air-temperature` station `S80`,
  `2-hour-weather-forecast` area `Sembawang`, `psi`. Fetched on a background task so LVGL never blocks
  on Wi-Fi; refresh 600 s, retry 30 s until the first success.
- The ticker uses a fixed-width clip box holding a `LV_LABEL_LONG_CLIP` label, animated with
  `lv_anim` on `lv_obj_set_x` from `0` to `-text_w - 24` and
  `lv_anim_set_duration(lv_anim_speed_to_time(px_per_second, 0, -text_w - 24))`. The label's own
  `SCROLL_CIRCULAR` ignores the requested speed. Speed is persisted in NVS
  (`Settings("idle_face")`, key `news_speed`) and is retunable at runtime through the device tool
  `self.display.set_news_scroll_speed` (0 = frozen, 6 = calm, 60 = max).
- LVGL's `base_line` is a descent measured *up* from the bottom of the line box, so aligning the
  seconds to the main digits is `translate_y = -(base_line_48 - base_line_20)` = −5 px.

## Flashing

```bash
idf.py build
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 write-flash \
  0x20000 build/xiaozhi.bin 0x410000 build/xiaozhi.bin
```

App-only, into **both** OTA slots: this keeps Wi-Fi credentials and the device↔agent binding, and a
merged image at `0x0` would wipe them.

The emoji set is an *assets* change, not a firmware change: copy `gifs/*.gif` into
`managed_components/78__xiaozhi-fonts/png/xiaozhi-emoji_128/` (the filename stem is the emotion key),
keep `DEFAULT_EMOJI_COLLECTION xiaozhi-emoji_128` in `main/CMakeLists.txt`, regenerate the assets and
flash them to `0x800000`:

```bash
python scripts/build_default_assets.py
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 write-flash 0x800000 build/generated_assets.bin
```

## Deliberately not wired

- **IR transmit is off** (`kIrTransmitEnabled = false` in `sensor_dock.cc`): no RMT TX channel, the
  enable pin is held inactive, and `self.ir.send_*` tools are not registered. Receive-only
  `self.ir.learn` stays available.
- **No touch input and no radar-triggered chat.** The GT911 panel on this unit never reports points,
  and the AT581X radar cannot measure distance; writing its sensitivity silences its output until a
  full power cycle, so the chip is never written. The only working physical input is the stock BOOT
  button.

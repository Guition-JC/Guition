# Changelog

## 0.2.4

- Fix abnormal display output on `esp32_8048s070` by correcting the RGB LCD sync timings: horizontal pulse width/back porch/front porch to 30/16/210 and vertical pulse width/back porch/front porch to 13/10/22.

## 0.2.1

- Add `audio_adc` PDM input on I2S0 for `jc3636f118`, `jc3636k518`, `jc3636k718` and `jc3636w518`, with PDM-to-PCM conversion and default 16 kHz, 16-bit stereo capture.
- Add `audio_dac` standard Philips I2S output on I2S1 for `jc3636f118`, `jc3636k718` and `jc3636w518`, using `chip: internal` and a software dummy codec with default 48 kHz, 16-bit stereo playback.
- Configure active-high amplifier control on GPIO46 for `jc3636k718` and GPIO48 for `jc3636w518`, with an initial low level.
- Correct the `jc1060f570c` PWM backlight pin from GPIO17 to GPIO52.

## 0.2.0

- Add the `jc5454f328` ESP32-S31 board definition to the component and board catalog, replacing the example-local copy.
- Make all 26 board directories independently usable with ESP Board Manager, including board-local Kconfig options, initialization code and required touch support sources.
- Use native ESP Board Manager I2C touch management for the 18 configurable capacitive-touch boards. Keep touch geometry, GPIOs, orientation and IO settings in YAML; native `i2c_addr` entries use left-shifted I2C addresses.
- Retain SPI extensions only for the seven resistive-touch boards. Use software SPI on `esp32_2432s028`, reading pins from peripheral YAML, and ESP-IDF hardware SPI on boards sharing the bus with LCD or SD devices.
- **Breaking change:** Replace the custom touch creation and handle APIs with Board Manager device initialization and `dev_lcd_touch_handles_t.touch_handle`. Applications must select `lcd_touch` for capacitive touch or `lcd_touch_resistive` for resistive touch according to menuconfig, and skip touch initialization in no-touch mode. Configurable touch devices use `init_skip: true` and require explicit initialization.
- Update both initialization examples for the selected touch device and exclude local tests from component release archives.
- Use the `guition` Component Registry namespace for board and driver dependencies, retaining the `Guition-JC/Guition` source repository.
- Add display color selection, configurable MIPI partial draw buffers and optional LVGL refresh statistics to `display_lv_demo_widgets`.

## 0.1.4

- Add the `jc3636f118` ESP32-S31 board definition with a 360×360 ST77916 QSPI display, optional CST816S capacitive touch, PWM backlight and 4-bit SDMMC, including its touch profile and board catalog entry.
- Enable JD9165 LCD V2 on `jc1060p470_i_w_y` with dedicated initialization commands and a horizontal back porch of 136; select V2 by default while retaining V1 support.
- Correct the `jc1060p470_i_w_y` LCD reset pin to GPIO0 and the SDMMC power LDO definition to LDO4.
- Fix the AXS15231B 2.1.0 display on/off callback polarity on `jc3248w535` and mirror the resistive touch Y axis on `jc4827w543`.
- Set the `esp32_4827s043` RGB bounce buffer to 3840 pixels (eight lines) so DMA blocks divide the 272-line frame evenly, and configure three frame buffers on `jc1060f570c`.
- Enable XMC flash bootloader support and brownout reset handling on `esp32_2424s012`; default to a 240 MHz CPU on `esp32_2432s024`, `esp32_2432s028`, `esp32_2432s032`, `esp32_3248s035` and `jc2432w328`.
- **Breaking change:** Remove `guition_sd.h` and the `guition_sd_*` APIs. Applications must use the ESP Board Manager `fs_sdcard` device for SD card lifecycle management and coordinate chip selects, display/touch initialization and touch polling when sharing SPI buses.
- Document the FAT32 requirement for SD cards on the resistive-touch `jc4827w543` variant.

## 0.1.3

- Add the `display_lv_demo_widgets` example with LVGL Widgets, Board Manager display initialization and touch input.
- Register the example at `example/display/display_lv_demo_widgets` for publication to the ESP Component Registry.
- Correct the upstream repository metadata to `Guition-JC/Guition` and the component path to `components/board/guition_boards`.
- Exclude local reference documents, development tools and editor settings from release archives.
- Update the README version, installation command and display example link.

## 0.1.2

- Add the `jc8048f543` ESP32-S31 board definition with the same peripheral, device and SDK configuration as `jc8048f550` for a different physical screen size.
- Include all three ESP32-S31 boards in the board catalog.
- Standardize board YAML indentation, list spacing, inline scalar arrays, hexadecimal I2C addresses and double-quoted strings.
- Preserve configuration values, data types, field order and YAML anchors and aliases during formatting.
- Document YAML formatting conventions and the 24 board configurations, including ESP32-S31.
- Synchronize the component version, README installation command and example dependency at `0.1.2`.

## 0.1.1

- Add target esp32s31

## 0.1.0 package refresh (2026-10-13)

- Rename the component directory to `guition_boards` under `components/boards`.
- Use published `guition-jc/esp_lcd_touch_gsl3680 ==0.1.0` for JC8012.
- Document published `guition-jc/esp_lcd_axs15260 ==1.0.0` for applications using that controller.
- Update registry namespace, repository metadata, package exclusions and local example.
- Preserve all board hardware settings and public C APIs.


## 0.1.0 — migration preview

- Add 17 ESP32/C3/S3 board definitions, configurable touch and shared SPI.
- Preserve the existing hardware-verified ESP32-P4 board parameters.
- Use the external `guition-jc/esp_lcd_touch_gsl3680` 0.1.0 dependency for JC8012.
- Map both JC1060 Arduino SKUs to the existing `jc1060p470` BMGR definition.
- Add a standalone initialization example and registry archive verification.
- Hardware validation of new ESP32/C3/S3 configurations remains pending.

# Board initialization

This minimal ESP-IDF 6.1 consumer initializes LCD, touch and backlight through
Board Manager, then repeatedly fills the screen with red, green, blue, white
and black, displaying each color for one second. The refresh resolution and
pixel format come from the selected board's BMGR configuration. The example
does not use LVGL or initialize audio/SD. Shared SPI boards acquire resources
per device; do not replace this sequence with a bulk
`esp_board_manager_init()` call.

MIPI DSI and RGB displays use two driver-owned frame buffers. Each color is
drawn into the inactive buffer, then submitted as a complete frame; the
previous buffer is reused only after the driver confirms the frame switch.
The board configuration must provide at least two frame buffers. RGB requires
continuous refresh, and ESP32-S3 RGB also requires a bounce buffer.

SPI displays wait for DMA completion before reusing their draw buffer. This
does not synchronize with the panel's scanout; avoiding tearing on SPI panels
requires panel-specific TE synchronization.

Install ESP-IDF 6.1 with the recommended
[ESP-IDF Installation Manager (EIM)](https://docs.espressif.com/projects/idf-im-ui/en/latest/).
Activate the environment in each new terminal. For Bash/Zsh:

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
```

For Fish:

```fish
source ~/.espressif/tools/activate_idf_v6.1.fish
```

After activation, ESP-IDF trusts the `espressif/esp_board_manager` extension
downloaded from the ESP Component Registry. This workflow does not require
setting `IDF_EXTENSION_ALLOW_UNTRUSTED` manually to use `bmgr`.

The manifest downloads `guition/guition_boards` 0.2.2 from the ESP Component
Registry during configuration or build. Copy only this example to a separate
workspace; its dependencies are installed in `managed_components/`.
Run from the copied example directory:

```shell
idf.py set-target esp32p4
idf.py bmgr -b jc8012p4a1
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your device's serial port.

JC8012P4A1C also needs the published `guition/esp_lcd_touch_gsl3680` 0.1.0.
The touch driver is also downloaded from the registry.

For another board choose its matching target and BMGR identifier from
`managed_components/guition__guition_boards/board_catalog.json`
(use `bmgr_board` when supplied). For example,
`esp32_2432s024` uses target `esp32`. ESP32 and ESP32-C3 have no PSRAM.
Run menuconfig to choose the available touch variant; currently the existing
P4 boards retain their native touch configuration and have no C/R/N menu.

When changing IDF versions or chips, clear this example's previous build
and `components/gen_bmgr_codes` before `set-target`, then regenerate with
`bmgr`. Never reuse a board's generated configuration for a different chip.
The bundled partition table fits the supported boards' minimum 4 MB flash;
adjust it for your application if necessary. Older ESP32-P4 silicon requires
the matching revision-family settings in menuconfig. Compilation is not
a hardware test.

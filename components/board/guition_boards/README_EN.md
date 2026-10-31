# Guition Boards

English | [简体中文](README.md)

Some board configurations may be incomplete and will be improved over time.

If you find a problem with a board configuration, please [open an issue](https://github.com/Guition-JC/Guition/issues) in the [Guition repository](https://github.com/Guition-JC/Guition).

ESP Board Manager board support for Guition development boards, published as `guition/guition_boards`.

This component provides board information, peripheral and device configurations, board-specific sdkconfig defaults, and support for displays, touch, backlights, SD cards, and audio. After adding the component, select a board with ESP Board Manager and generate its configuration code to use the corresponding devices in an ESP-IDF application.

Each complete board directory can be copied into another ESP-IDF application and used independently, without installing the entire `guition_boards` component. A board directory contains its initialization source code, touch support, Kconfig options, and the following configuration files:

- `board_info.yaml`: Board identifier, target chip, and manufacturer information.
- `board_peripherals.yaml`: Peripheral configurations, including buses and GPIOs.
- `board_devices.yaml`: Devices, driver dependencies, and initialization parameters.
- `sdkconfig.defaults.board`: Board-specific default configuration.

Board YAML files use two-space indentation, with list items indented one level below their parent field and blank lines between device and peripheral entries. Scalar arrays use inline syntax (such as `[FL, FR]`), and I2C addresses use hexadecimal notation (such as `0x30` and `[0xBA, 0x28]`). Descriptions, manufacturer names, mount paths, and strings requiring quotes use double quotes.

For complete ESP Board Manager usage instructions, see its [component documentation](https://github.com/espressif/esp-board-manager/blob/main/README.md).

## Compatibility

The current version is `0.2.2`. [idf_component.yml](idf_component.yml) defines the dependency requirements:

| Dependency | Version |
| --- | --- |
| ESP-IDF | `>=6.1.0` |
| `espressif/esp_board_manager` | `0.7.3~1` |
| `espressif/esp_lcd_touch` | `1.2.1` |

The component manifest declares ESP32, ESP32-C3, ESP32-S3, ESP32-P4, and ESP32-S31 targets, with 26 board configurations in total. ESP32-S31 boards require an ESP-IDF environment that supports the `esp32s31` target.

## Supported Boards

The tables below describe the configurations provided by this component. Board identifiers can be used directly with `idf.py bmgr -b <board>`. The Touch column lists available hardware variants; select the variant that matches your hardware. `-` means no corresponding device configuration is provided. A board configuration does not imply that the board has been tested on physical hardware.

### ESP32-P4

| Board Identifier | LCD (Interface) | Resolution | Touch | SD Card | Audio |
| --- | --- | --- | --- | --- | --- |
| [jc1060p470](jc1060p470/) | JD9165 (MIPI DSI) | 1024×600 | GT911 | SDMMC | ES8311 (DAC + ADC) |
| [jc1060p470_i_w_y](jc1060p470_i_w_y/) | JD9165 (MIPI DSI) | 1024×600 | GT911 | SDMMC | ES8311 (DAC + ADC) |
| [jc4880p443](jc4880p443/) | ST7701 (MIPI DSI) | 480×800 | GT911 | SDMMC | ES8311 (DAC + ADC) |
| [jc8012p4a1](jc8012p4a1/) | JD9365 (MIPI DSI) | 800×1280 | GSL3680 | SDMMC | ES8311 (DAC + ADC) |

`jc1060p470` and `jc1060p470_i_w_y` correspond to the versions without and with an enclosure, respectively. For JC8012P4A1, select LCD V1 or V2 under `Guition JC8012P4A1 display` in menuconfig; V2 is the default. The JC4880P443 touch entry reflects its actual GT911 factory function. Its YAML chip name and coordinate range still need to be checked against the hardware; see the [board configuration](jc4880p443/board_devices.yaml).

### ESP32-S31

| Board Identifier | LCD (Interface) | Resolution | Touch | SD Card | Audio |
| --- | --- | --- | --- | --- | --- |
| [jc5454f328](jc5454f328/) | AXS15260 (3-wire SPI + RGB) | 540×540 | AXS15260 | SDMMC | I2S DAC + ES7210 ADC |
| [jc8048f543](jc8048f543/) | Generic RGB panel (RGB) | 800×480 | GT911 | SDMMC | ES8389 (DAC + ADC) |
| [jc8048f550](jc8048f550/) | Generic RGB panel (RGB) | 800×480 | GT911 | SDMMC | ES8389 (DAC + ADC) |
| [jc3636f118](jc3636f118/) | ST77916 (QSPI) | 360×360 | CST816S / None | SDMMC | PDM input + I2S output |
| [jc1060f570c](jc1060f570c/) | Generic RGB panel (RGB) | 1024×600 | GT911 | SDMMC | ES8389 (DAC + ADC) |

### ESP32-S3

| Board Identifier | LCD (Interface) | Resolution | Touch | SD Card | Audio |
| --- | --- | --- | --- | --- | --- |
| [esp32_4827s043](esp32_4827s043/) | ST7262 (RGB) | 480×272 | GT911 / XPT2046 / None | SPI | - |
| [esp32_4848s040](esp32_4848s040/) | ST7701 (3-wire SPI + RGB) | 480×480 | GT911 / None | SPI | - |
| [esp32_8048s043](esp32_8048s043/) | ST7262 (RGB) | 800×480 | GT911 / None | SPI | - |
| [esp32_8048s050](esp32_8048s050/) | ST7262 (RGB) | 800×480 | GT911 / None | SPI | - |
| [esp32_8048s070](esp32_8048s070/) | ST7262 (RGB) | 800×480 | GT911 / None | SPI | - |
| [jc3248w535](jc3248w535/) | AXS15231B (QSPI) | 320×480 | AXS15231B / None | SPI | - |
| [jc3636k518](jc3636k518/) | ST77916 (QSPI) | 360×360 | CST816S / None | SDMMC | PDM input |
| [jc3636k718](jc3636k718/) | ST77916 (QSPI) | 360×360 | CST816S / None | SDMMC | PDM input + I2S output |
| [jc3636w518](jc3636w518/) | ST77916 (QSPI) | 360×360 | CST816S / None | SDMMC | PDM input + I2S output |
| [jc4827w543](jc4827w543/) | ST77916 (QSPI) | 480×272 | GT911 / XPT2046 / None | SPI | - |
| [jc8048w550](jc8048w550/) | ST7262 (RGB) | 800×480 | GT911 / None | SPI | - |

### ESP32

| Board Identifier | LCD (Interface) | Resolution | Touch | SD Card |
| --- | --- | --- | --- | --- |
| [esp32_2432s024](esp32_2432s024/) | ST7789 (SPI) | 240×320 | CST820 / XPT2046 / None | SPI |
| [esp32_2432s028](esp32_2432s028/) | ST7789 (SPI) | 240×320 | CST820 / XPT2046 / None | SPI |
| [esp32_2432s032](esp32_2432s032/) | ST7796 (SPI) | 240×320 | GT911 / XPT2046 / None | SPI |
| [esp32_3248s035](esp32_3248s035/) | ST7796 (SPI) | 320×480 | GT911 / XPT2046 / None | SPI |
| [jc2432w328](jc2432w328/) | ST7789 (SPI) | 240×320 | CST820 / XPT2046 / None | SPI |

### ESP32-C3

| Board Identifier | LCD (Interface) | Resolution | Touch | SD Card |
| --- | --- | --- | --- | --- |
| [esp32_2424s012](esp32_2424s012/) | GC9A01 (SPI) | 240×240 | CST816S / None | - |

## Quick Start

### Install and Activate ESP-IDF

We recommend using [ESP-IDF Installation Manager (EIM)](https://docs.espressif.com/projects/idf-im-ui/en/latest/) to download and install ESP-IDF 6.1 and the required toolchains. The commands below use the default installation path on Linux / macOS. For a custom installation location, use the actual activation script path generated by EIM.

Activate the environment each time you open a terminal, using the command for your shell.

Bash / Zsh:

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
```

Fish:

```fish
source ~/.espressif/tools/activate_idf_v6.1.fish
```

After activating the environment, the `espressif/esp_board_manager` extension downloaded from the ESP Component Registry is trusted by ESP-IDF, so this workflow does not require setting `IDF_EXTENSION_ALLOW_UNTRUSTED` manually to use `bmgr`. Run all commands below in an activated terminal.

### Get the LVGL Example from the Component Registry

`idf.py add-dependency` adds a component dependency; it does not also download separately published example projects. To run `display_lv_demo_widgets`, execute the following commands in the directory where you want to store your projects:

```bash
idf.py create-project-from-example "guition/guition_boards=0.2.2:display_lv_demo_widgets"
cd display_lv_demo_widgets
idf.py set-target esp32p4
idf.py bmgr -b jc8012p4a1
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

This workflow uses `jc8012p4a1` as an example. Replace `PORT` with the actual serial port. The project creation command downloads the example source code. During the first configuration or build, the component manager automatically downloads `guition_boards` and other dependencies into `managed_components/`. The example already declares the `guition_boards` dependency with a default version constraint of `"*"`, so the component manager automatically selects a version satisfying the project's constraints; there is no need to add the dependency again. To pin the component version, change the version of `guition/guition_boards` in `main/idf_component.yml` to `"==0.2.2"` before configuring or building.

This command requires version `0.2.2` to be published in the component registry with the `display_lv_demo_widgets` example. If it reports `Cannot find example`, check the version's Examples page and the example name. For usage details, see [Espressif's example project documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html#creating-a-project-from-an-example).

### Add the Component to an Existing Project

Copy this component into your application's `components/guition_boards` directory. Once the component is published in the component registry, you can also add it with:

```bash
idf.py add-dependency "guition/guition_boards==0.2.2"
```

This component already declares the ESP Board Manager dependency. ESP Board Manager generates the driver dependencies required by each board according to the selected configuration.

### Use a Board Independently

To use a board independently, copy its complete directory into your application's `components/` directory. For example, after copying `jc3636k718/`, run the following commands from the application directory:

```bash
idf.py add-dependency "espressif/esp_board_manager==0.7.3~1"
idf.py set-target esp32s3
idf.py bmgr -b jc3636k718
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

The component generated by BMGR compiles the `.c` files in the board directory; do not register the same touch source files separately. The base touch API dependency declared by the board must be retained even when touch is disabled. Configure bus and touch parameters in the board's own YAML files.

### Initialization and Five-Color Display Example

Copy the [board_init example](examples/board_init/README.md) directory to a separate workspace. Run from the copied example directory:

```bash
idf.py set-target esp32p4
idf.py bmgr -b jc8012p4a1
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

During configuration or build, the example downloads `guition/guition_boards` `0.2.2` and other dependencies from the ESP Component Registry into `managed_components/`. No board component copy or local path configuration is required. After initializing the LCD, touch, and backlight, it repeatedly fills the screen with red, green, blue, white, and black, displaying each color for one second. The refresh resolution and pixel format come from the selected board's BMGR configuration. The example does not use LVGL or initialize audio or the SD card.

MIPI DSI / RGB uses two frame buffers: each complete frame is drawn into the inactive buffer, and the previous buffer is reused only after the driver's frame-switch completion event, avoiding writes to the image being scanned out. The board configuration must provide at least two frame buffers; see the [board_init example](examples/board_init/README.md) for synchronization requirements for each interface.

For other boards, replace the target chip and board identifier with the appropriate values. Regenerate the BMGR configuration after switching boards. When switching target chips, use a separate project directory to avoid reusing previously generated configuration. ESP32-S31 is a preview target in ESP-IDF 6.1; its commands require `--preview` after `idf.py` and before the action name, for example, `idf.py --preview bmgr -b jc5454f328` and `idf.py --preview build`.

## Configuration and Usage

- **Touch selection**: For ESP32, ESP32-C3, and ESP32-S3 boards, select capacitive, resistive, or no touch under `Guition touch` in menuconfig. The available options depend on the board's capabilities. ESP32-P4 boards use their native touch configurations.
- **Touch parameters**: Capacitive touch uses BMGR's native `lcd_touch` device. Configure resolution, reset and interrupt GPIOs, orientation, and IO parameters in `lcd_touch.config` in `board_devices.yaml`. `peripherals.i2c_addr` uses I²C addresses shifted left by one bit; for example, write the 7-bit address `0x15` as `[0x2A]`. Resistive touch uses `lcd_touch_resistive.config`, with pins from `board_peripherals.yaml`. ESP32-2432S028 uses a separate software SPI bus. Boards that share lines with the LCD or SD card use ESP-IDF hardware SPI to manage the shared bus. After editing YAML, run `idf.py bmgr -b <board>` and `idf.py build`; add `--preview` after `idf.py` for ESP32-S31.
- **Device initialization**: Boards with multiple touch variants set `init_skip: true` for their touch devices. The application explicitly initializes `lcd_touch` or `lcd_touch_resistive` according to `CONFIG_GUITION_TOUCH_CAPACITIVE` / `CONFIG_GUITION_TOUCH_RESISTIVE`, and skips touch initialization when touch is disabled. Both expose the handle through `dev_lcd_touch_handles_t.touch_handle`. The dependency `matches` in YAML selects driver dependencies according to menuconfig. See the [board_init example](examples/board_init/main/board_init.c) for device initialization selection. Avoid initializing all peripherals at once, which may enable buses that were not selected.
- **Display buffers**: The ESP32 / ESP32-C3 boards in this component have no PSRAM, so applications must use internal RAM buffers. The AXS15231B driver on JC3248W535 requires full-frame submissions; configure the application's refresh mode accordingly.
- **SD cards**: Boards with an SD option enable it through `GUITION_SD_ENABLED`, which is disabled by default. JC5454F328 uses the native `fs_sdcard` device, which the application explicitly initializes, and does not use `GUITION_SD_ENABLED`. Use ESP Board Manager's `fs_sdcard` device to manage mounting, unmounting, and the card handle; the board's device and peripheral configurations supply the parameters. On boards with shared SPI, the application must coordinate chip-select states, LCD / touch initialization order, and touch polling during mounting.
- **SD cards**: Use the FAT32 filesystem.
- **P4 configuration**: For older chip revisions, select the matching revision in menuconfig. The application must provide a partition table matching the Flash capacity; the example includes a partition table.

See [board_catalog.json](board_catalog.json) for the index of boards and touch variants.

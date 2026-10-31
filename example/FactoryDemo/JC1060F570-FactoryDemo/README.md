# JC1060F570C Factory Demo

[中文](README_CN.md) | English

An ESP-IDF factory demo for the Guition JC1060F570C board. It provides a touch-screen desktop with networking, media playback, device settings and hardware tools. The current project configuration targets ESP32-S31 with a 1024 × 600 display and 16 MB flash.

## Requirements

- JC1060F570C board, USB data cable and a computer with access to its serial port.
- ESP-IDF **6.2.0 or later** with ESP32-S31 support and its installed tools. We recommend installing ESP-IDF using Espressif Installation Manager (EIM). The dependency versions are declared in `main/idf_component.yml`; ESP-IDF Component Manager generates `dependencies.lock` when it resolves them.
- Network access for the ESP-IDF Component Manager when dependencies are not already available locally.

## Build

Open a terminal in the project root and activate the EIM-installed ESP-IDF environment. On Linux with Bash:

```bash
source ~/.espressif/tools/activate_idf_master.sh
```

On Linux with Fish:

```fish
source ~/.espressif/tools/activate_idf_master.fish
```

The activation script path depends on the EIM installation and selected ESP-IDF version. Configure a fresh checkout and build:

```bash
idf.py --preview set-target esp32s31
idf.py bmgr -b jc1060f570c
idf.py menuconfig
idf.py build
```

ESP32-S31 is a preview target in ESP-IDF master, so target selection requires `--preview`. The project provides `sdkconfig.defaults`; ESP-IDF generates the local `sdkconfig` during configuration.

Use `idf.py menuconfig` to inspect or change the configuration. The build generates `build/FactoryDemo.bin` along with bootloader, partition table, model, assets and font images. The asset set is selected according to the board configuration.

## Flash and monitor

Connect the board and replace `/dev/ttyACM0` with its actual serial port:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Exit the monitor with `Ctrl+]`. Use the full `flash` command for initial programming and after changing bundled assets or fonts: these are separate partitions and an app-only flash does not update them. The custom partition layout is defined in `partitions.csv`.

## Using the demo

- Navigate the touch-screen desktop to open Clock, Settings, Wi-Fi, Ethernet, Music, Video, Photo, Spectrum and AI Chat, as well as factory test, GPIO, I2C, serial and USB tools.
- In **Wi-Fi**, scan and select a network or start QR-code provisioning. For provisioning, connect the phone to the device's access point and open the provisioning page. If a connection attempt fails, start a new QR-code provisioning session on the device.
- Once the device has an IP address, open `http://<device-ip>/` from the same network for web settings; the weather configuration page is at `/weather`. The settings server yields port 80 while provisioning is active. Use these pages only on a trusted network: they do not require authentication.
- **Settings** provides controls such as language, brightness, volume, time and standby. Standby can display a clock, photos or video.
- For removable media, place files in `music/`, `image/` and `video/` on the SD card. The video player accepts MP4 files containing MJPEG video with AAC or MP3 audio; an MP4 extension alone does not guarantee a supported codec. Music also supports a Classic Bluetooth audio source.

## Project layout

| Path | Contents |
| --- | --- |
| `main/` | Application entry point, desktop, views and web settings |
| `components/` | Board services and application components |
| `managed_components/` | ESP-IDF Component Manager dependencies |
| `spiffs/` | Display-resolution-specific bundled assets |
| `fonts/` | Bundled font and its licensing information |
| `Docs/` | Project documentation in Chinese and English |
| `sdkconfig.defaults` | Default build configuration |

See the [documentation index](Docs/README.en.md) for detailed feature documentation.

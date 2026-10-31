# Clock and custom faces

[中文](clock.md) | [Documentation index](README.en.md)

Open **Clock** from the desktop. Press and hold the clock to open its settings, where you can choose analog, flip, seven-segment or image-digit styles and adjust the time zone. Select **Done** to close settings. The style and time zone are saved.

In analog mode, swipe left or right to change faces. Built-in and SD-card faces are supported; their format and setup are described below. Configure the device time source in system settings.

## Analog faces

In the analog clock view, swipe left for the next face or right for the previous one. The sequence wraps through the drawn face, bundled image faces and SD-card faces. The selection is saved; if the selected resource is unavailable, the drawn face is used.

## Add an SD-card face

Create one directory per face under the directory matching the display resolution:

```text
SD card root/
  clock/
    1024x600/
      classic/
        placement.json
        dial.png
        hour.png
        minute.png
        second.png
        cap.png
```

For the other supported resolution, use `clock/800x480/`. Only the current display resolution is scanned, with face directories sorted by name. You can copy the matching `spiffs/<resolution>/clock/a01/` directory and rename `a01` as a starting point. Reopen Clock after adding the files. The card root is mounted as `/sdcard`; do not create an extra `sdcard` directory on the card. Legacy images placed directly under `clock/` without a placement file are not scanned.

The dial, hour hand and minute hand are required; the second hand and center cap are optional and should be omitted from JSON when unused. File names are specified in JSON and must match case. Supported image extensions are `.png`, `.jpg` and `.jpeg`. Transparent PNG is recommended for hands, with each unrotated hand pointing to twelve o'clock. `preview.png` is not loaded.

## Placement file

This example is for an 800 × 480 screen. The sizes must match the actual images:

```json
{
  "screen": [800, 480],
  "dial_file": "dial.png",
  "dial_size": [440, 440],
  "dial_top_left": [180, 20],
  "screen_pivot": [400, 240],
  "hands": {
    "hour_hand_12": {"file": "hour.png", "size": [32, 165], "pivot": [16, 153]},
    "minute_hand_12": {"file": "minute.png", "size": [30, 207], "pivot": [15, 195]},
    "second_hand_12": {"file": "second.png", "size": [24, 263], "pivot": [12, 203]},
    "center_cap": {"file": "cap.png", "size": [38, 38], "pivot": [19, 19]}
  }
}
```

Coordinates and sizes are integer pixels from the top-left corner. Images are displayed at their supplied size. `screen_pivot` is the common center on screen; each image's `pivot` is local to that image. An unrotated hand begins at `screen_pivot - pivot`. The center cap is aligned to the common pivot and does not rotate. Image dimensions in JSON must match the files; images must not exceed the screen, the dial must fit on screen, and the screen pivot and local pivots must fit within the dial and their respective images. File names must refer to ordinary files in the face directory, without paths, `..`, hidden names or control characters. The placement file is limited to 8 KiB. The scan examines at most 256 directory entries and the catalog holds at most 64 faces, including drawn and bundled faces. Extra legacy placement fields are ignored.

Bundled faces are packaged with the selected resolution's `assets` partition. Rebuild and flash the assets partition after modifying bundled files.

## Availability

The current and neighboring faces may be cached. A face that cannot be loaded or decoded is skipped until Clock is reopened; if the selected face is unavailable, the drawn face is used. Already decoded SD images may remain visible after card removal, but loading another image requires the card.

# Media and spectrum

[中文](media.md) | [Documentation index](README.en.md)

The desktop provides Music, Video, Photos and Spectrum apps. Music, Video and Photos can use bundled assets or files on an SD card. Place SD files directly in the following directories at the card root:

```text
SD card root/
├── music/    audio files
├── video/    video files
└── image/    image files
```

The device mounts the card internally at `/sdcard`; do not create a `sdcard` directory on the card. Insert a readable card before opening the app. If a media list is empty, check that the card is mounted and that the directory and file extensions are correct. A supported extension alone does not guarantee that a file's encoding or dimensions are supported.

## Music

Open **Music** on the desktop and use the play/pause, previous, next and volume controls. The source button switches between SD card and Classic Bluetooth. Local playback reads MP3, WAV and AAC files in `music/` and also includes bundled sample tracks; playback depends on the actual encoding. Volume and mute preferences can be saved and restored at startup.

To use a Bluetooth source, enable Bluetooth in device settings, switch to Classic Bluetooth in Music, then connect and play from a phone or other source device. If there is no sound, check the selected source, Bluetooth connection, volume and mute state.

## Video

Open **Video** to use play/pause, previous, next and volume controls. The player scans bundled video and files with the `.mp4` extension in the SD card's `video/` directory.

Normal playback requires MJPEG video and AAC or MP3 audio in the MP4 container; mono and stereo audio are supported. Video encodings such as H.264 are outside this supported set, and renaming a file to `.mp4` does not convert it. Resolution and individual frame size are also limited by the device's decoding buffers. If playback fails, check the encoding, image dimensions and file integrity.

## Photos

Place JPG, JPEG, PNG, SJPG or SPNG images in `image/` on the SD card and open **Photos** to browse them. The app also reads bundled images. Hidden files and unsafe names are excluded; image dimensions and available decode memory also affect whether an image can be displayed. If an image fails to display, check its format and try a smaller image.

## Microphone spectrum

Open **Spectrum** to view the spectrum of sound captured by the onboard microphone. Long-press the screen to open style settings and select solid bars, dot matrix or blocks. You can also choose linear, visual or logarithmic frequency grouping and configure grid lines. Settings are saved for the next time the app opens.

Spectrum requires an audio input device. If the graph does not respond, check the microphone and audio device and make a sound nearby. Capture resources are released when you leave the app.

Photos and video can also be used for standby; see [Automatic standby and standby media](standby.en.md).

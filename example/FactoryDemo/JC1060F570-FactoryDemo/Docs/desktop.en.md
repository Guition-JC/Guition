# Desktop, display and language

[中文](desktop.md) | [Documentation index](README.en.md)

## Using the desktop

The touch desktop appears after startup. Swipe horizontally to change pages, tap an icon to open an app, and use the app's back or close control to return. The status bar shows information such as time and connection status. A full-screen app first displays its name and icon while it initializes; a fallback mark is shown if the icon is unavailable.

The first page provides Touch, Settings, Wi-Fi, Calculator, Music, Video, Serial, USB storage and I2C tools, with an Ethernet or GPIO entry depending on the build configuration. The second page provides Clock, AI Chat, Spectrum, Photo Album and Factory Test. Available features depend on the board and configuration.

## Startup video and display assets

At startup the device attempts to play a bundled video while preparing the desktop. The desktop appears after playback, or if playback is unavailable or times out. The firmware waits at most eight seconds for playback to finish. JC1060F570C uses assets from `spiffs/1024x600/`, with bundled videos in `video/`. After changing bundled images or videos, rebuild and use the full `idf.py flash` command to update the `assets` partition. If the display is abnormal, check the selected board, display connection and whether the asset partition was fully flashed.

See [Media playback](media.en.md) for video formats and SD-card playback.

## Language and fonts

Choose Chinese or English in **Settings → Language**; the selection is restored after restart. The provisioning page also offers a language switch. Chinese display requires an available Chinese-capable font; if none is available, the interface indicates that only English is supported.

The device normally uses the bundled font in the separate `fonts` partition. It covers common Chinese and English characters, but some Chinese characters may be missing. To use a font with additional characters:

1. Place a `.ttf` file in `fonts/` at the root of the SD card and insert the card.
2. Open **Settings → Font** and select the font. The built-in font can also be selected again.
3. If the font fails to load, check the SD card and file before selecting it again; the device falls back to an available font.

The bundled font is packaged from `fonts/embedded/`. After changing it, rebuild and flash the complete project to update the separate font partition. The font license is in `fonts/LICENSE.txt`.

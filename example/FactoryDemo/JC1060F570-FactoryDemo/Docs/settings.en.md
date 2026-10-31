# Device settings and time

[中文](settings.md) | [Documentation index](README.en.md)

Open **Settings** from the desktop to adjust display, sound, connectivity, time and standby. Persistent settings are restored on subsequent starts. If an operation fails, follow the message on the page and check the associated hardware or network.

## Common settings

- **Display and sound**: adjust backlight, volume, mute and spectrum grid display.
- **Language and connectivity**: select the Chinese or English interface and turn Bluetooth on or off.
- **Fonts**: select the built-in font, or put `.ttf` files in `fonts/` at the SD card root and select one on the page. The built-in font is used if loading fails.
- **Weather**: choose the fields shown in the status bar. Configure the provider, city and key on the weather web page after connecting to a network; see [Network connections and web services](network.en.md).
- **Automatic standby**: choose off, 30 seconds, 1 minute, 2 minutes or 5 minutes. Standby can show a clock, photos or video; select the media and photo interval in settings.

## Time and time zone

The settings page can select the internal clock or the onboard RTC as the time source. If the RTC is unavailable, the page reports it and retains the internal clock. In the clock app, long-press to open clock settings, adjust the time zone and view synchronization and RTC status.

Network time synchronization requires Wi-Fi or Ethernet and access to an SNTP service:

1. Confirm that the device has an IP address.
2. Choose the immediate synchronization action in the time section of Settings.
3. Check synchronization status in clock settings. On failure, check network access, DNS and the SNTP service, then retry.

The current build uses `pool.ntp.org` as its SNTP server; the setting can be inspected in `sdkconfig`. After successful synchronization the program coordinates RTC time according to RTC status. Timekeeping while power is removed depends on the onboard RTC and its power supply.

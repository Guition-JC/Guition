# Automatic standby and standby media

[中文](standby.md) | [Documentation index](README.en.md)

After a period of inactivity on the desktop, the device can enter standby and display a clock, a photo slideshow or silent video. Configure the timeout and content in device **Settings**. Standby content can also be configured through the device's web settings when connected to a network.

## Set automatic standby

1. Open **Settings** from the desktop and find the standby timeout.
2. Select off, 30 seconds, 1 minute, 2 minutes or 5 minutes. The default is 1 minute; the selection is saved.
3. Return to the desktop and wait for the selected idle period. Opening an app or dialog restarts the idle timer; selecting off prevents automatic standby due to inactivity.
4. Touch the screen to wake the device.

## Choose the displayed content

Select and confirm a standby mode in settings:

- **Clock**: display a clock.
- **Photos**: select at least one and up to 64 images, with a slideshow interval of 3, 5, 10 or 30 seconds.
- **Video**: select one video; standby video plays silently.

The selection is saved. The media list includes available bundled assets and SD card files; place SD card images in `image/` at the card root and videos in `video/`. Keep the card readable when selecting or playing SD media. See [Media and spectrum](media.en.md) for image and video format limits.

If an expected file is missing, check that the card is mounted and that its directory name and file format are correct. Files that exceed decoding dimensions or use unsupported encodings may not display or play.

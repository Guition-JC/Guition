# Network connections and web services

[中文](network.md) | [Documentation index](README.en.md)

The device supports Wi-Fi and Ethernet. Once connected and assigned an IPv4 address, its web settings and weather configuration are accessible from a phone or computer on the same network. Find the address in the relevant network app on the device.

## Wi-Fi connection

Open **Wi-Fi** on the desktop, scan for nearby networks, select one and enter its password. Open networks do not require a password. The page shows connection status and IP address and provides disconnect and DHCP lease renewal actions. If connection fails, check the network name, password and signal. Disconnecting does not erase saved credentials.

### Provisioning with a phone

1. Start QR-code provisioning in the Wi-Fi app. The device displays a hotspot QR code, hotspot name and password, and a one-time code for submitting the configuration.
2. Scan the QR code with the phone to join the device hotspot. If the phone does not join automatically, use the displayed network details.
3. Open a web page in the phone's browser to reach the provisioning portal, select the target Wi-Fi network, enter its password and the one-time code, and submit.
4. Wait for the device to connect and obtain an IP address. If the attempt fails, restart QR-code provisioning on the device to create a new session and retry.

The QR code joins the device hotspot; the one-time code authorizes the configuration request. While the provisioning portal owns port 80, ordinary web settings are temporarily unavailable and resume after provisioning ends.

## Ethernet

Connect an Ethernet cable and open **Ethernet** on the desktop to view link status, IPv4 address, subnet mask, gateway and available traffic counters. The link can be up before DHCP assigns an address; the page also offers lease renewal. If no address appears, check the cable, switch or router, and DHCP service.

The Ethernet page also offers TCP and UDP throughput measurement. Once Ethernet has an IPv4 address, enter the peer IPv4 address, port, duration and reporting interval and start; the peer must run a compatible iPerf service. The page shows progress, rates and transferred data.

The current `sdkconfig` enables the internal Ethernet controller with RMII and an IP101 PHY; the application supports one Ethernet port. Inspect the settings under Ethernet Configuration in `idf.py menuconfig`. The current RMII input clock uses GPIO 13, TX_EN 12, TXD0/TXD1 8/9, CRS_DV 15, RXD0/RXD1 19/18, MDC/MDIO 53/10, PHY reset 17, and automatic PHY address detection (-1). When changing the board or wiring, verify these values against the schematic and check for pin conflicts. Rebuild and flash after changing configuration.

## Web settings and weather

1. Connect the device to Wi-Fi or Ethernet and obtain an IP address.
2. From a browser on the same network, open `http://<device-ip>/` for web settings; the device settings page also provides an entry and QR code.
3. Open `http://<device-ip>/weather`, or follow the weather link from web settings.

The weather page supports OpenWeather and Seniverse. Select the provider, enter its API key, choose automatic public-IP location or a manually entered city, and save. Use the refresh action to request weather. For OpenWeather, a manual city can use a city and country code such as `Beijing,CN`; Seniverse accepts a city name or pinyin. Leave an already saved key blank to retain it. The device's weather settings select status-bar fields such as location, conditions, temperature, humidity and wind.

Weather updates require internet access, valid provider credentials and a valid city setting. Public-IP location may be inaccurate; select a city manually if needed. If the web page is unavailable, check network connectivity between the client and device and make sure the provisioning portal is not active.

Web settings and weather endpoints do not require login, and the API key is stored on the device. Use them on trusted networks and do not expose the service to the public internet. For local settings and time synchronization, see [Device settings and time](settings.en.md).

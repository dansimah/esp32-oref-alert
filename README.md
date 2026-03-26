# ESP32 Oref Alert LED

Standalone ESP32-S3 firmware that monitors the Pikud Haoref (Home Front Command) alert API and provides real-time visual alerts through an RGB LED.

## Features

- Polls the official Oref alert API every 2 seconds
- Pre-alert detection (advance warning before siren)
- Adaptive polling: 2s during alerts, 10s when idle, exponential backoff on errors
- Configurable alert zone, LED pin, and time-to-shelter
- Web-based configuration page at `http://oref-alert.local/`
- Improv Wi-Fi setup via ESP Web Tools (no captive portal needed)
- Browser-based firmware installation (no IDE required)

## LED States

| State | Color | Meaning |
|-------|-------|---------|
| Solid Blue | Waiting for Wi-Fi credentials |
| Blinking Blue | Connecting to Wi-Fi |
| Solid Green | OK -- monitoring, no alerts |
| Solid Orange | Pre-alert -- alert expected soon |
| Solid Red | Active alert |

## Hardware

- **ESP32-S3** development board (e.g., ESP32-S3-DevKitC-1)
- Uses the onboard RGB LED (GPIO 48) by default, or connect an external WS2812 NeoPixel to any GPIO

## Install (End Users)

1. Visit the [install page](https://your-username.github.io/esp32-oref-alert/) in Chrome or Edge
2. Connect your ESP32-S3 via USB
3. Click **Install Firmware**
4. Enter your Wi-Fi credentials in the dialog that appears
5. Open `http://oref-alert.local/` to configure your alert zone

## Build from Source

Requires [PlatformIO Core (CLI)](https://docs.platformio.org/en/latest/core/installation/index.html).

```bash
# Install PlatformIO CLI
pip install platformio

# Clone and build
git clone https://github.com/your-username/esp32-oref-alert.git
cd esp32-oref-alert
pio run

# Connect ESP32-S3 via USB, then:
pio run -t upload          # flash firmware
pio run -t uploadfs        # flash config web page (SPIFFS)
pio device monitor         # view serial output
```

The first `pio run` automatically downloads the ESP32-S3 toolchain and all libraries.

## Configuration

After connecting to Wi-Fi, open `http://oref-alert.local/` in your browser:

- **Alert Zone** -- the exact Hebrew name of your Pikud Haoref alert polygon (e.g., `תל אביב - יפו 1`)
- **LED GPIO Pin** -- default 48 (ESP32-S3 onboard LED), change if using an external NeoPixel
- **Time to Shelter** -- the standard Pikud Haoref interval for your area (0, 15, 30, 45, 60, 90, or 180 seconds)

Use the **Test Alert** button to verify your LED wiring.

## API

The firmware polls `https://www.oref.org.il/warningMessages/alert/Alerts.json` with the required Oref headers. The `ALERT_URL` constant in `src/main.cpp` can be changed to a proxy URL if direct access is geo-blocked.

## Project Structure

```
platformio.ini          PlatformIO build configuration
src/main.cpp            Firmware source
data/index.html         Device config web page (SPIFFS)
docs/index.html         ESP Web Tools install page (GitHub Pages)
docs/manifest.json      ESP Web Tools firmware manifest
```

## License

MIT

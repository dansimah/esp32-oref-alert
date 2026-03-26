
# 🚨 Pikud Haoref Standalone ESP32 Alert Sensor - Project Specification

## 1. Project Overview

A standalone, universally installable firmware for ESP32 microcontrollers that provides real-time physical visual alerts based on the Pikud Haoref (Home Front Command) API. The device functions as a plug-and-play appliance: users flash the firmware via a web browser, connect to a captive portal, and configure their specific hardware and location settings without writing any code.

## 2. Software Stack & Dependencies

The firmware will be written in C++ using the Arduino framework.

* **WiFiManager (by tzapu):** Handles the captive portal and AP mode.
* **Preferences:** Handles Non-Volatile Storage (NVS) to save user configurations across reboots.
* **ArduinoJson:** Parses the API payload.
* **Adafruit_NeoPixel:** Drives WS281x addressable LEDs.
* **HTTPClient & WiFiClientSecure:** Manages the HTTPS GET requests.

## 3. Data Architecture & NVS Storage

The device must store four custom parameters in flash memory using the `Preferences` library.

| Parameter | Type | Default Value | Description |
| --- | --- | --- | --- |
| `zone_name` | String | `"תל אביב - יפו 1"` | Exact Hebrew spelling of the alert polygon. |
| `led_pin` | Integer | `48` | The GPIO pin connected to the LED. |
| `led_type` | Integer/Bool | `1` (NeoPixel) | `0` for Standard PWM LED, `1` for NeoPixel. |
| `tts_seconds` | Integer | `90` | Time-to-Shelter countdown duration in seconds. |

## 4. Captive Portal (Setup Phase)

On first boot, or if Wi-Fi connection fails, the device spins up an Access Point (e.g., `Alert-Sensor-Setup`). The `WiFiManager` portal must be modified using `WiFiManagerParameter` to include:

1. **Wi-Fi SSID & Password** (Default functionality).
2. **Alert Zone Field:** Text input for the Hebrew zone string.
3. **LED Pin Field:** Text input (converted to integer on save).
4. **LED Type Selection:** A dropdown or radio toggle (Standard vs. NeoPixel).
5. **Time-to-Shelter Selection:** A dropdown with standard Pikud Haoref intervals (`0`, `15`, `30`, `45`, `60`, `90`, `180`).

*Important:* In the `setup()` function, read NVS *before* initializing the LED. Dynamically allocate the `Adafruit_NeoPixel` object or `pinMode()` based on the retrieved `led_type` and `led_pin` variables.

## 5. Core Logic & State Machine

### A. The Polling Engine (Network Loop)

Runs asynchronously every **2000 milliseconds**.

* **Endpoint:** `https://www.oref.org.il/WarningMessages/alert/alerts.json`
* **Required Headers:** * `Referer: https://www.oref.org.il/12481-he/Pikud.aspx`
* `X-Requested-With: XMLHttpRequest`


* **Parsing Logic:** Check HTTP response code. If `200 OK` and payload length is > 5 characters, deserialize JSON. Scan the `data` array for an exact string match with `zone_name`.
* **State Update:**
* If match found: Set `isAlertActive = true`. If this is a new alert (was previously false), record `alertStartTime = millis()`.
* If no match found / empty payload: Set `isAlertActive = false`.



### B. The Visual State Machine (Hardware Loop)

Runs continuously and evaluates the current state of `isAlertActive`.

1. **STATE: IDLE (`isAlertActive == false`)**
* LED is completely OFF.


2. **STATE: COUNTDOWN (`isAlertActive == true` AND elapsed time < `tts_seconds`)**
* *Condition:* `(millis() - alertStartTime) < (tts_seconds * 1000)`
* *Action:* LED flashes rapidly in bright red (e.g., 200ms ON, 200ms OFF). Represents the active run-to-shelter phase.


3. **STATE: WAITING (`isAlertActive == true` AND elapsed time >= `tts_seconds`)**
* *Condition:* `(millis() - alertStartTime) >= (tts_seconds * 1000)`
* *Action:* LED shifts to a solid, dim red (or a very slow, breathing pulse). Represents that the impact window has passed, but the siren/threat is still actively reported by the API.



## 6. Network Resilience & Geo-Blocking Constraints

Because the device will be operating from Palestine, direct requests to the Akamai WAF might be blocked depending on the local ISP's routing.

* **HTTP 403 Handling:** If the `HTTPClient` returns a `403 Forbidden` or timeouts repeatedly, the code must implement an exponential backoff (e.g., wait 5 seconds, then 10 seconds) rather than hammering the server, which will result in a permanent IP ban.
* **Proxy Fallback:** The firmware should be designed so the `endpoint_url` can easily be swapped from the official `oref.org.il` domain to a custom proxy URL (like a Cloudflare Worker) that routes traffic through an Israeli IP if direct access fails.

## 7. Deployment Architecture (ESP Web Tools)

Do not distribute source code to end users. Distribute compiled binaries via a static webpage hosted on GitHub Pages.

1. **Binaries:** Compile `firmware_esp32.bin` and `firmware_esp32_s3.bin`.
2. **Manifest:** Create a `manifest.json` that maps the chip family to the correct `.bin` file.
3. **Webpage:** Create an `index.html` containing the `<script type="module" src="https://unpkg.com/esp-web-tools@9.0.3/dist/web/install-button.js?module"></script>` snippet. Users plug in via USB and click "Install" directly from Chrome/Edge.

---

Would you like me to draft the specific JSON structure for the `manifest.json` file so you have the exact syntax ready for the web deployment?
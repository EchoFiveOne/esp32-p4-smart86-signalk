# SignalK Instrument Panel for Waveshare ESP32-P4

A touchscreen [SignalK](https://signalk.org/) marine instrument display built on the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm)
("Smart 86 Box").

> **Fork note:** this `instruments` branch of
> [EchoFiveOne/esp32-p4-smart86-signalk](https://github.com/EchoFiveOne/esp32-p4-smart86-signalk) extends
> [VesselSense/esp32-p4-smart86-signalk](https://github.com/VesselSense/esp32-p4-smart86-signalk) with the
> Instruments app (configurable pages, wind rose tile, NZ tides, sun/moon), panel-wide day/night mode,
> JD9365/ST7703 display auto-detection, Signal K device-token authentication, a Startup App setting and
> several reliability fixes. Developed with AI assistance (GitHub Copilot) and tested on hardware.

## Hardware

| | |
|---|---|
| **Board** | [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) |
| **Display** | 4" IPS 720x720 MIPI-DSI, ST7703/JD9365DA detected at boot |
| **Touch** | GT911 capacitive (I2C) |
| **WiFi** | ESP32-C6 coprocessor via SDIO ([esp-hosted](https://github.com/espressif/esp-hosted)) |
| **Flash / PSRAM** | 16 MB / 8 MB |

## Screenshots

Screenshots are rendered by the headless [LVGL](https://lvgl.io/) simulator at the display's native 720x720 resolution.

| Wind Rose | Autopilot |
|:---------:|:---------:|
| ![Wind Rose](simulator/screenshots/windrose.png) | ![Autopilot](simulator/screenshots/autopilot.png) |

| Settings | WiFi Settings | Launcher |
|:--------:|:-------------:|:--------:|
| ![Settings](simulator/screenshots/settings.png) | ![WiFi](simulator/screenshots/wifi.png) | ![Launcher](simulator/screenshots/launcher_connected.png) |

## Apps

| App | Description |
|-----|-------------|
| **Instruments** | i70/Triton2-style multi-function display: any Signal K paths as large numbers in configurable grids (1–16 tiles) on swipeable pages, with custom labels/units, min/max, trend sparklines, alarms, themes, a wind rose tile, NZ tides (LINZ) with projected depth, sun/solunar graphs, and a web editor at `http://<panel-ip>/`. Opens automatically at power-on by default. See [docs/INSTRUMENTS.md](docs/INSTRUMENTS.md). |
| **Wind Rose** | Now the `panel.wind.rose` tile in Instruments (no longer a separate launcher app; the original app source is kept for the simulator). |
| **Autopilot** | Autopilot controller with scrolling compass, wind bar, and engage/disengage/mode/adjust controls via the SignalK Autopilot API. |
| **Settings** | Timezone (Auto from GPS position with DST by default, or manual), unit preferences, Startup App (which app opens at power-on: Home screen, Instruments, Autopilot, WiFi or Settings; default Instruments), and Display: brightness plus automatic dawn/dusk night mode (red filter, dimming) for all apps. Persisted in NVS. |
| **WiFi Settings** | On-device WiFi configuration with network scanning, autocomplete, and saved network management (two-tap Forget on the connected network and on saved networks). |

## Features

- **Zero configuration networking** — finds SignalK servers automatically via mDNS
- **On-device WiFi setup** — no compile-time credentials needed
- **Live data** — WebSocket subscription with observer pattern for efficient UI updates
- **Unit conversion** — user-selectable display units (knots/m/s, feet/meters, etc.)
- **Device authentication** — SignalK access request pairing with NVS token persistence
- **Audio feedback** — click, engage/disengage sweeps, and invalid-press buzz via ES8311 codec
- **True wind derivation** — computes TWS/TWA from AWS + AWA + STW when server doesn't provide it
- **Headless simulator** — validate UI changes on desktop without hardware

## Prerequisites

- [ESP-IDF v5.5.6](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32p4/get-started/index.html)
- Python 3.10+ (macOS: use Homebrew, not the Xcode system Python)
- CMake (`brew install cmake` on macOS)

```sh
# Install ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && git checkout v5.5.6 && ./install.sh esp32p4

# Add shell alias
echo 'alias get_idf=". $HOME/esp/esp-idf/export.sh"' >> ~/.zshrc
```

See [docs/SETUP.md](docs/SETUP.md) for detailed setup instructions and [docs/BUILD.md](docs/BUILD.md) for build system reference.

## Build and Flash

```sh
get_idf            # activate ESP-IDF environment
python -m pip install --upgrade esp-bmgr-assist
idf.py reconfigure # download managed board dependencies on a fresh checkout
idf.py bmgr -b esp32_p4_wifi6_touch_lcd_4b
idf.py build
idf.py -p /dev/cu.usbmodem* flash   # flash via USB-C (Linux: /dev/ttyUSB0)
```

Or use the Makefile:

```sh
make build
make flash PORT=/dev/cu.usbmodem*
```

Dependencies are declared in [`main/idf_component.yml`](main/idf_component.yml).
Run `idf.py reconfigure` once before `idf.py bmgr` on a fresh checkout so the
component manager downloads the Waveshare board definition.

## First Boot

1. The launcher shows an amber SignalK logo — WiFi is not configured yet
2. Open the **WiFi Settings** app, select your network, enter the password
3. Credentials are saved to NVS and used automatically on subsequent boots
4. The device discovers the SignalK server via mDNS and connects
5. The launcher logo turns green — data is flowing

The ESP32-C6 WiFi coprocessor supports **2.4 GHz only**. The device and SignalK server must be on the same network.

## Simulator

The simulator compiles the actual firmware UI code as a native desktop binary using LVGL 8.x. No hardware needed.

```sh
# First time — downloads LVGL (~30s)
cmake -B simulator/build -S simulator -DCMAKE_BUILD_TYPE=Release -Wno-dev

# Render all apps
simulator/render.sh

# Render a specific app
simulator/render.sh windrose
simulator/render.sh autopilot
simulator/render.sh wifi
simulator/render.sh settings
simulator/render.sh browser
```

Output: `simulator/screenshots/*.png` at 720x720.

See [docs/SIMULATOR.md](docs/SIMULATOR.md) for architecture details.

## Dev Server

For development, run a local [SignalK server](https://github.com/SignalK/signalk-server) with sample data:

```sh
cd ../signalk-server
node bin/signalk-server --sample-nmea0183-data --sample-n2k-data --override-timestamps
```

The device discovers it automatically via mDNS. See [docs/DEV_SERVER.md](docs/DEV_SERVER.md) for the full workflow.

## Project Structure

```
main/                        Entry point, BSP init, WiFi, mDNS, launcher overlay
components/
  signalk_client/            WebSocket client, InstrumentStore, observer pattern
  instruments_app/           Configurable multi-page instrument grid + web editor
  wind_rose_app/             Heading compass rose with wind + corner panels
  autopilot_app/             Autopilot controller with SignalK API control
  settings_app/              Timezone and unit preference picker
  wifi_settings_app/         On-device WiFi configuration UI
  signalk_auth/              Device authentication and API calls
  wifi_manager/              WiFi scan, connect, NVS credential persistence
  unit_config/               Unit conversion engine and NVS preferences
  audio_feedback/            Button sounds via ES8311 codec
simulator/                   Headless LVGL renderer for desktop UI validation
docs/                        Setup, build, simulator, autopilot, and API reference
```

## Documentation

| Document | Description |
|----------|-------------|
| [docs/SETUP.md](docs/SETUP.md) | Prerequisites, build/flash workflow, first boot |
| [docs/BUILD.md](docs/BUILD.md) | Component dependencies, sdkconfig, build notes |
| [docs/DEV_SERVER.md](docs/DEV_SERVER.md) | Local SignalK server setup and connection |
| [docs/SIMULATOR.md](docs/SIMULATOR.md) | Headless LVGL simulator architecture |
| [docs/AUTOPILOT_UI.md](docs/AUTOPILOT_UI.md) | Autopilot display design and API integration |
| [docs/INSTRUMENTS.md](docs/INSTRUMENTS.md) | Instruments dashboard app, web editor and HTTP API |
| [docs/FONTS.md](docs/FONTS.md) | Custom LVGL font generation with lv_font_conv |
| [docs/signalk-paths.md](docs/signalk-paths.md) | Example SignalK paths and unit conversions |

## Built With

- [ESP-IDF v5.5.6](https://github.com/espressif/esp-idf) — Espressif IoT Development Framework
- [LVGL 8.4](https://lvgl.io/) — Light and Versatile Graphics Library
- [esp-ui (esp-brookesia) v0.2](https://github.com/espressif/esp-ui) — Phone-style launcher shell
- [SignalK](https://signalk.org/) — Open marine data standard
- [Waveshare BSP](https://github.com/waveshareteam/Waveshare-ESP32-components) — Board support package
- [Waveshare ESP Board Manager](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B) — LCD controller detection

## License

MIT

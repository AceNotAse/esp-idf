# Home Assistant Rotary Knob (CrowPanel 1.28" ESP32-S3)

Custom firmware that turns the **Elecrow CrowPanel 1.28" HMI ESP32 Rotary Display** into a desk or wall
controller for Home Assistant. Turn the knob to pick a device, press to switch it on or off, and hold
to dim it, change the volume or set the thermostat.

> **The short version:** the knob works like a TV remote that talks to Home Assistant over your Wi-Fi.
> Each time you press it, it sends Home Assistant a small web request such as "toggle light.living_room",
> the same way the HA app does. Every few seconds it also asks HA what state everything is in, so the
> screen stays correct when someone uses a wall switch or the phone app.

## What it does

| Action | Browse mode (default) | Adjust mode (blue ring) |
|---|---|---|
| Turn the knob | Previous / next device | Change brightness, volume, position or temperature |
| Press | Toggle on/off, run a scene or script, or **lock** a lock | Done, go back to browse mode |
| Hold (0.6 s) | Enter adjust mode, or **unlock** a lock | Done, go back to browse mode |
| Swipe left / right on screen | Next / previous device | n/a |
| Tap the screen | Same as pressing the knob | Same as pressing the knob |

- **The ring on screen** shows the level, or full/empty for on/off. **Amber** means on, **grey** means off,
  **red** means the device is offline or the entity ID doesn't exist.
- **The 5 LEDs** around the knob follow the same colours. For lights they also follow the brightness.
- **The screen and LEDs turn off** after 60 s idle. The first touch or turn only wakes the screen, so you
  can't toggle something by accident in the dark.
- Locks only unlock on a long press, so a stray tap can't open your door.

Supported entity types: `light`, `switch`, `input_boolean`, `fan`, `cover`, `media_player`, `climate`,
`lock`, `scene`, `script`. Other entities still get a plain `homeassistant.toggle` on press.

## How it works

```
 ┌──────────── CrowPanel knob ────────────┐            ┌──── Home Assistant ────┐
 │ encoder.c  knob + button (PCNT)        │            │                        │
 │     │ events                           │   Wi-Fi    │  REST API              │
 │ ui.c       screen + what each input    │  HTTP(S)   │  GET  /api/states/...  │
 │     │ "toggle #2" / "set #2 to 70%"    │ ─────────► │  POST /api/services/.. │
 │ ha_client.c  Wi-Fi, polling, service   │ ◄───────── │  (Bearer token auth)   │
 │              calls, JSON parsing       │   JSON     │                        │
 │ board.c    LCD, touch, LEDs, backlight │            └────────────────────────┘
 └────────────────────────────────────────┘
```

It uses Home Assistant's built-in **REST API**, so you don't need an MQTT broker, an add-on or an
integration installed in HA, only a token.

## Hardware

| Part | Chip | Pins (ESP32-S3 GPIO) |
|---|---|---|
| 1.28" round LCD 240×240 | GC9A01 (SPI) | SCLK 10, MOSI 11, CS 9, DC 3, RST 14, backlight 46 |
| Touch screen | CST816D (I2C 0x15) | SDA 6, SCL 7, INT 5, RST 13 |
| Rotary encoder | n/a | A 45, B 42, push 41 |
| LED ring | 5× WS2812 | 48 |
| Power enable | n/a | GPIO 1 and 2 driven high; GPIO 40 power LED off |

The pins come from Elecrow's wiki. You can change any of them in `menuconfig` if your board revision differs.

---

## Step 1: Get a Home Assistant token

1. In Home Assistant, click your **user name** (bottom left).
2. Open the **Security** tab and scroll down to **Long-lived access tokens**.
3. Click **Create token**, name it `rotary-knob` and **copy the token now** (HA only shows it once).

Then collect the **entity IDs** you want on the knob. You can find them under
**Settings → Devices & services → Entities**, for example `light.living_room` or `switch.coffee_maker`.

> **Tip:** use HA's IP address (`http://192.168.1.50:8123`) rather than `homeassistant.local`. Not every
> network resolves `.local` names reliably for small devices.

## Step 2: Install ESP-IDF (one time)

This project lives inside this ESP-IDF fork, so the framework is already here.

**Linux / macOS**
```bash
cd ~/esp-idf                # wherever you cloned this repo
./install.sh esp32s3        # downloads the compiler, ~10 minutes, one time only
. ./export.sh               # run this in every new terminal before using idf.py
```

**Windows:** install the [ESP-IDF Windows installer](https://dl.espressif.com/dl/esp-idf/), choose
"use an existing ESP-IDF directory" and point it at this repo. Then use the "ESP-IDF PowerShell" shortcut it creates.

## Step 3: Configure

```bash
cd projects/ha_rotary_knob
idf.py menuconfig
```

Use the arrow keys to open **HA Rotary Knob**, then:

| Menu | Setting | Example |
|---|---|---|
| Wi-Fi | SSID / password | Your **2.4 GHz** network. The ESP32-S3 can't join 5 GHz. |
| Home Assistant | URL | `http://192.168.1.50:8123` |
| Home Assistant | Long-lived access token | The token from step 1 |
| Home Assistant | Entities | `light.living_room,switch.coffee_maker,climate.thermostat` |
| Behaviour | Screen timeout, long-press time, knob direction | Defaults are fine to start |

Press **S** to save, then **Q** to quit.

> **Security note:** these settings are saved in the `sdkconfig` file, which then contains your Wi-Fi
> password and HA token. `.gitignore` already excludes it, so **don't** force-add it to git. If a token
> ever leaks, delete it in HA's Security tab and create a new one.

## Step 4: Build and flash

Plug the knob into your computer with a USB-C **data** cable (some cables only carry power).

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # Linux. macOS: /dev/cu.usbmodem*  Windows: COM3 etc.
```

`monitor` shows the live log. Exit it with **Ctrl+]**. A healthy boot looks like this:

```
I (512) ha: entity 0: light.living_room
I (530) ha: connecting to Wi-Fi "myssid"
I (2840) ha: Wi-Fi connected, IP 192.168.1.87
I (5210) ui: toggle entity 0
I (5212) ha: POST /api/services/light/toggle {"entity_id":"light.living_room"}
```

If the port isn't found, or the flash won't start: hold the **BOOT** button, tap **RESET**, release
BOOT, then run the flash command again.

> Flashing replaces Elecrow's demo firmware. If you ever want it back, Elecrow provides it on the
> product wiki.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Text is mirrored | Panel orientation | menuconfig → Board pins → turn off **Mirror display horizontally** |
| Picture upside down | Panel orientation | Turn on **Mirror display vertically** |
| Taps land in the wrong place | Touch orientation | Toggle **Mirror touch X / Y** |
| Knob goes the wrong way | Encoder wiring | Behaviour → **Reverse knob direction** |
| One click skips 2 items, or needs 2 clicks | Encoder resolution | Behaviour → **Encoder counts per click** (try 2 or 4) |
| Status says "Connecting to Wi-Fi..." forever | Wrong SSID/password or 5 GHz network | Check the log for the disconnect reason |
| Status says "HA unreachable" | Wrong URL/port, HA down, VLAN/firewall | From another device on the **same network/VLAN** as the knob, try opening the URL. Check that IoT VLAN → HA:8123 is allowed. |
| Status says "Bad HA token" | Token typo or deleted | Make a new token and paste it again |
| A device shows "Not found" | Entity ID typo | Copy the exact ID from HA's Entities page |
| A light can't be dimmed | It's an on/off-only light | Expected. Long press does nothing for it. |

**Network tip:** if your smart-home gear sits on its own IoT VLAN, the knob needs TCP to HA on port 8123
(or 443 if you use HTTPS through a reverse proxy). Polling sends one small request per entity every 3 s
by default, which is negligible traffic.

## Customising

| I want to... | Edit |
|---|---|
| Change colours | `COLOR_*` defines at the top of `main/ui.c` |
| Change the layout or font sizes | `ui_start()` in `main/ui.c` |
| Change what press / hold does for a device type | `call_service()` in `main/ha_client.c` |
| Support a new domain (e.g. `vacuum`) | Add it to `ha_domain_t` (`ha_client.h`) and `s_domain_names`, then handle it in `apply_state_json_locked()` and `call_service()` |
| Change the LED ring behaviour | End of `ui_redraw()` in `main/ui.c` |
| Refresh faster or slower | menuconfig → Home Assistant → **State refresh interval** |

## Project layout

```
ha_rotary_knob/
├── CMakeLists.txt         project definition
├── sdkconfig.defaults     board defaults (ESP32-S3, fonts, partition table)
├── partitions.csv         flash layout (one 3 MB app)
├── dependencies.lock      exact versions of the downloaded libraries
└── main/
    ├── Kconfig.projbuild  everything you see under "HA Rotary Knob" in menuconfig
    ├── idf_component.yml  external libraries: LVGL, GC9A01, CST816S, LED strip, cJSON
    ├── main.c             start-up order
    ├── board.c/.h         LCD, touch, LEDs, backlight, LVGL task
    ├── encoder.c/.h       rotary knob + button → events
    ├── ha_client.c/.h     Wi-Fi + Home Assistant REST API
    └── ui.c/.h            screen layout and what each input does
```

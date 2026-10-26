# PocketChat

An offline group chat room that runs entirely on one ESP32-S3 board. The board creates its own WiFi network; anyone who joins it gets the chat page automatically (captive portal), picks a username and talks to everyone else connected. No internet, no app install: the client is a web page served by the board, so it works on Windows, Android, iOS and Linux.

A small OLED shows the board's status and pops up a notification for every new message.

## Features

- **Self-hosted WiFi** (SoftAP) with optional password, up to 8 devices
- **Captive portal**: a wildcard DNS server plus an HTTP redirect opens the chat as soon as a phone joins the network. `http://pocket.local` (mDNS) also works on Windows (tested) and usually on iOS
- **Real-time chat over WebSocket**: one message is fanned out to every connected client
- **Usernames** (1-24 characters, duplicates rejected), join/leave notices and a live online list
- **Last 30 messages** replayed to people who join late (kept in RAM, lost on reboot)
- **Heartbeat and auto-reconnect**: dead connections are dropped by the board, and the page reconnects by itself and re-joins with the same name
- **OLED display** (SSD1306, 128x64): boot splash and progress bar, three rotating pages (status, online users, system), and a pop-up for each new message (sender plus the first 35 characters, then `...`)
- **Optional home-WiFi mode** (AP+STA): the board can also join your router, so devices on your LAN can reach the chat at the board's LAN IP (written but not tested on hardware yet)

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3-WROOM-1-N16R8 dev board | 16 MB flash, 8 MB PSRAM. GPIO35-37 are used by the PSRAM and must stay free |
| 0.96" I2C OLED, SSD1306, 128x64 | 4 pins: GND, VDD, SCK, SDA |
| 4 female-to-female jumper wires | A breadboard is not needed |

### Wiring

| OLED | ESP32-S3 |
|---|---|
| VDD | 3V3 (not 5V) |
| GND | GND |
| SDA | GPIO 8 |
| SCK (SCL) | GPIO 9 |

The BOOT button (GPIO 0) flips to the next display page, or dismisses a message pop-up. Press it during normal operation only; holding it while resetting puts the board into upload mode.

## Setup

### 1. Arduino IDE

- ESP32 board package **2.0.17**
- Board: **ESP32S3 Dev Module**
- Flash Size: **16MB**
- PSRAM: **OPI PSRAM**
- Partition Scheme: **16M Flash (3MB APP/9.9MB FATFS)**
- USB CDC On Boot: **Disabled**
- Upload through the USB-C port labelled UART, serial monitor at **115200**

### 2. Libraries (Library Manager)

| Library | Author |
|---|---|
| WebSockets | Markus Sattler (`Links2004/arduinoWebSockets`) |
| ArduinoJson, version 7 or newer | Benoit Blanchon |
| U8g2 | oliver (`olikraus/u8g2`) |

WiFi, WebServer, DNSServer, ESPmDNS and Wire ship with the ESP32 core.

*Optional:* the WebSockets library accepts 5 simultaneous clients by default. To raise it, change `WEBSOCKETS_SERVER_CLIENT_MAX` in the library's `WebSocketsServer.h` (this project was tested with 15) and restart the IDE. The sketch reads the value at compile time, so it works with any setting.

### 3. Credentials

```
cd PocketChat
cp secrets.h.example secrets.h      # then edit secrets.h
```

`secrets.h` holds the board's WiFi name and password (and, optionally, your home WiFi). It is in `.gitignore`. Without it the sketch still compiles, but with an **open** network and no home-WiFi mode.

### 4. Upload and use

1. Open `PocketChat/PocketChat.ino` and upload it.
2. Join the **PocketChat** WiFi network. The chat page should open by itself; otherwise browse to `http://192.168.4.1` (use `http://`, not `https://`) or `http://pocket.local`.
3. Pick a name and chat. Use a second device to see the fan-out.

## Display

| Page | Content |
|---|---|
| 1/3 PocketChat | WiFi name, IP, users online, total messages |
| 2/3 Online | Names of connected users (first four, then "+N more") |
| 3/3 System | Uptime, free RAM, peak users, WiFi devices |

Pages rotate every 5 seconds. A new message interrupts for 3 seconds with a pop-up. The OLED font is ASCII only, so other characters (for example Persian) show as `?` on the display; the web page itself handles them fine.

## Protocol

Messages are JSON over WebSocket on port 81.

| Direction | Message | Meaning |
|---|---|---|
| client to board | `{"type":"join","name":"Ali"}` | Join with this name |
| client to board | `{"type":"msg","text":"hi"}` | Send a chat message (max 300 characters) |
| board to client | `{"type":"joined","name":"Ali"}` | Join accepted (history follows) |
| board to client | `{"type":"error","text":"..."}` | Rejected, for example duplicate name |
| board to all | `{"type":"msg","from":"Ali","text":"hi"}` | Chat message |
| board to all | `{"type":"sys","text":"Ali joined"}` | System notice |
| board to all | `{"type":"users","list":["Ali","Sara"]}` | Online users |

## Load test

`tools/load_test.py` opens N virtual users from your computer, sends 20 messages from the first one and measures how long each takes to reach everyone (all bots share one clock, so this is the fan-out latency).

```
pip install websockets
python tools/load_test.py 5
```

Results from one run (bots from a single laptop, plus two real browser clients connected at the same time):

| Bots | Delivered | Avg latency | Max latency |
|---|---|---|---|
| 3 | 60/60 | 9.3 ms | 12.5 ms |
| 5 | 100/100 | 11.2 ms | 18.4 ms |
| 10 | 200/200 | 14.3 ms | 22.4 ms |
| 11 | 220/220 | 15.1 ms | 26.5 ms |

- Nothing was lost, and latency grows about 0.6-0.8 ms per extra user.
- 12 and 15 bots failed to connect. With the two real clients that means roughly **13 simultaneous WebSocket connections** is the practical ceiling, even with the library limit raised to 15. This is a measurement, not a verified explanation; limited sockets in the network stack are the likely cause.
- Free heap: about 282 KB idle and about 250 KB with 13 connections. It returned to about 270 KB after each run, with no sign of a leak.
- These bots all come from one WiFi device. The 8-device WiFi limit was not stress-tested with real hardware.

## Limitations and security

- **Plain HTTP and WS, no login.** Apart from the WiFi password, there is no authentication, no encryption inside the chat and no rate limiting. Anyone on the network can read messages and flood the room.
- **Do not forward its ports to the internet.** The board's web server handles one request at a time and is not built to face the public internet.
- With home-WiFi mode enabled, everyone on your LAN can reach the chat too.
- History lives in RAM only.
- `pocket.local` is unreliable on Android; use the captive portal or the IP address there.

## Ideas for later

- A room password and per-user rate limiting
- microSD card for persistent history and file sharing
- A native client (Flutter) for desktop and mobile

## Repository layout

```
PocketChat/
  PocketChat.ino        the firmware (server and the embedded web page)
  secrets.h.example     copy to secrets.h and fill in
tools/
  load_test.py          WebSocket load and latency test
```

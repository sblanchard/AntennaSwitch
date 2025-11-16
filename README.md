📡 FlexPilot ESP32 Antenna Switch
WiFi + MQTT + Web Dashboard | Compatible With FlexRadio / FlexPilot Automation

This project implements a network-controlled antenna switch using an ESP32, ULN2803A transistor driver, and SIP reed relays.

It integrates perfectly with:

FlexPilot

Log4OM (CAT via Hamlib in your app)

Node-RED

Home Assistant

Any MQTT automation system

The firmware provides:

✔ Web-based control panel

✔ MQTT command/state topics

✔ Stable relay control (no auto-revert)

✔ Safe “Hold Last State” logic

✔ mDNS discovery

✔ Designed for silent low-RF-loss switching

🔧 Features

🌐 Web Interface


<img width="592" height="503" alt="Screenshot 2025-11-16 133157" src="https://github.com/user-attachments/assets/37c74df9-aef8-4fdc-83c1-388c9641c3e6" />

Buttons for Antenna 1, Antenna 2, All Off

Auto-refreshing state

Works on mobile and desktop

📡 MQTT Integration

Control from FlexPilot

Retained state messages

Topics:

flexpilot/antennaSwitch/cmd

flexpilot/antennaSwitch/state

🧠 Safe Behavior

If WiFi or MQTT disconnect:
➡ Relays do NOT change
➡ ESP32 holds last known state
➡ Zero risk of switching during TX

🧲 Hardware-Optimized

Uses ULN2803A darlington array to drive relay coils

Drives 5V reed relays safely from ESP32 logic

Designed for external RF relay board switching

🛜 mDNS Support

Access via:

http://flexpilot-switch.local

🖼️ Screenshot (placeholder)

You can add screenshot later:

![Web UI](docs/ui.png)

🛠️ Hardware Required

ESP32 (ESP32-C3, ESP32-WROOM, etc.)

ULN2803A driver IC

Hailge SIP-1A05 5V reed relays

450–500 Ω coil resistance

5V buck converter (for coil power)

Shared GND between ESP32 and ULN2803A

🔌 Wiring Diagram (ASCII)
SIP-1A05 Reed Relay (4-pin)
Pin 1 – RF NO
Pin 2 – Coil -
Pin 3 – Coil +
Pin 4 – RF COM

ULN2803A → ESP32 → Relays
ESP32 GPIO16 ─── 1B (ULN input)
                1C (ULN output) ─── Relay1 Coil -

ESP32 GPIO17 ─── 2B
                2C ─── Relay2 Coil -

Power
Relay Coil + ─── 5V
ULN COM     ─── 5V
ULN GND     ─── ESP32 GND


Important: ESP32 3.3V must not power the coils.

📦 Firmware Installation (PlatformIO)

Install PlatformIO

Create new project:

Board: ESP32 Dev Module (or esp32-c3-devkitc-02)

Framework: Arduino

Add dependency:

lib_deps =
    knolleary/PubSubClient


Upload src/main.cpp

🧪 MQTT Command Reference
Command Topic
flexpilot/antennaSwitch/cmd

Commands
Payload	Action
1	Select antenna 1
2	Select antenna 2
0	All off
off	All off
State Topic
flexpilot/antennaSwitch/state


Payload is:

1
2
off


State is retained to ensure persistence after reboot.

🌐 REST API
Set antenna
/set?ant=1
/set?ant=2
/set?ant=0

Get state
/state


Returns JSON:

{"antenna": 1}

📡 mDNS Hostname

The device is reachable at:

http://flexpilot-switch.local

📂 Structure
/src/main.cpp
/platformio.ini
/README.md
/docs/ui.png
/docs/wiring.png

🚀 Future Enhancements

OTA firmware updates

4-relay version (4-position switch)

FlexRadio auto-band-switch logic

Integration with SWR sensors

Home Assistant auto-discovery

FlexPilot-native panel

Rule engine:

"If band = 40m → Antenna 2"

"If SWR > 3 → Fallback antenna"

🏆 License

MIT License

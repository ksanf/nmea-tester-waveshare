# Web interface

The embedded browser console provides remote control of the tester's operating
modes and settings. It supports one controlling browser session. The HTML, CSS,
JavaScript, icons, and fonts used by the page require no external downloads.

## Connect

1. Enable Wi-Fi on the tester. It is off after each power-on.
2. Join the tester's access point, or connect both devices to the same network
   using the tester's STA mode.
3. Read the address on the main screen's **WI-FI ON** button and open
   `http://<device-address>/`. The default AP address is `http://192.168.4.1/`.
4. Select **Connect to instrument**.

In STA mode the address appears after DHCP completes. If the connection is lost,
the Wi-Fi button shows the connection state instead of keeping an old address.
Opening the page alone does not take control or start an operating mode.

HTTP uses port 80; the page communicates with the device through WebSocket
`/ws`. The console is intended for a trusted local network. These connections
are unencrypted and have no separate user login. Change the default AP password
before using it outside a controlled bench network.

## Control ownership and reconnecting

While the browser owns control, the LCD shows a globe, a **Return control**
button, and four activity indicators:

| Interface | TX | RX |
|---|---|---|
| RS-485 | Yellow | Green |
| CAN | Blue | White |

Only the return button accepts touch input. The operating mode continues while
the ordinary local controls are suspended. Entering browser control does not
stop an active transmitter or bridge. Returning restores the screen for the
mode that is actually running.

The browser sends a heartbeat every five seconds. Closing its connection or
missing heartbeats for 15 seconds starts reconnect waiting. The device keeps
the current operating mode; it restores touch control 60 seconds after the last
successful connection or heartbeat if the browser has not resumed its session.

The LCD **Return control** button and browser **Return to touch control** button
release control immediately. Previously queued commands and the old session
token cannot reclaim a released session. Select **Connect to instrument** again
to start a new session. A second browser cannot displace an active controller.

The token is held in the device's RAM and that tab's `sessionStorage`. Reloading
the same tab can resume its session within the reconnect window. Commands whose
outcome became unknown at disconnection are not automatically resubmitted as
new actions. Check the current device state before repeating a transmission.

## Modes and navigation

Mode tabs appear on the left on larger screens and in a horizontal strip on
small screens.

| Tab or view | Functions |
|---|---|
| Receiver | NMEA/HEX stream, pause and clear display, instrument readings and freshness, transfer of received values into templates |
| Receiver: AIS targets | Decoded AIS targets with pagination; decoding continues independently of the LCD view |
| Generator | GPS/GYRO/LOG/ECHO/WEATHER groups, independent rates, live output above the group controls, manual transmission, and template editing |
| RS485 Bridge | Bidirectional RS-485 traffic, counters, HEX input, and existing Telnet/UDP bridge routes |
| NMEA 2000 | CAN speed, observed devices, PGN details, and traffic log |
| SAILOR | Antenna status and Mini-C terminal |
| Settings | NMEA version, LCD theme/orientation, Wi-Fi AP/STA settings, scan/connect/disconnect, and memory information |

Selecting Receiver, Generator, RS485 Bridge, NMEA 2000, or SAILOR starts the
corresponding mode. Selecting an already running mode does not restart it.
The active view and confirmed running mode are shown separately; a transition
waits for the command acknowledgement and updated device state.

**Stop instrument** stops the current mode but leaves its tab open. The tab then
offers a start button. Settings, AIS subviews, and Templates do not change the
running mode. Reconnecting restores the view of the device state without sending
a mode-start command.

## Generator and templates

Open **Templates** from any section, or use a generator group's **Edit** button.
Opening the editor does not stop transmission. Use the individual group switches
or **Stop all groups** to stop it.

The editor exposes the template fields, rates, sentence switches, talker IDs,
`$`/`!` prefixes, checksums, and ROT motion controls. It submits changed fields
rather than replacing an entire stale snapshot. This preserves concurrent clock
and motion updates. Drafts survive closing the editor or switching groups; a
late acknowledgement does not overwrite newer edits.

Coordinates in instrument views use degrees and decimal minutes. Template
inputs use NMEA `DDMM.MMMM` latitude and `DDDMM.MMMM` longitude with separate
hemisphere fields. Invalid values are rejected before applying the edit.

The selected NMEA version affects the implemented sentence formatting described
in the [main README](README.md#version-profiles). Generated traffic is visible
in the Generator log and goes out through RS-485; it is not automatically
broadcast on Telnet or UDP.

## Antenna status

The SAILOR view displays:

- C/N0 in dBHz and signal bars;
- antenna serial number and coordinates;
- current ocean, network registration, protocol, and TDM channel.

The current ocean is derived from the antenna's registered network. It is
separate from the preferred-ocean setting and may be unavailable when not
registered. Network status is requested approximately every five seconds.
Each group becomes stale independently after 15 seconds or loss of the binary
service. After reconnect, each group needs a new response before it is marked
current. Missing values remain unavailable; cached values are not presented as
live readings.

Automatic status requests are read-only. There are no Login, Logout, Scan,
link-test, messaging, or distress controls in this status panel. The terminal
is a separate command channel: commands entered there are passed to the antenna.
See [TT6006 service profile](docs/tt6006-profile.txt) for implementation scope.

## Terminal and logs

The SAILOR terminal provides a basic 80×24 VT100 screen with up to 2,000 earlier
lines retained in the browser. It handles cursor movement and erase operations;
it does not emulate terminal color attributes.

Select the terminal to type, or enter a command in the form and select its line
ending: **CR**, **CR+LF**, or **None**. **Esc** and **Ctrl+C** buttons send those
keys. Scrolling up pauses following new output so it does not pull the viewport
away from the text being read. **Latest output** returns to the current output.
**Shift+Page Up**, **Shift+Page Down**, **Shift+Home**, and **Shift+End** navigate
locally. **Clear display** clears the browser terminal history.

Ordinary terminal screen-clear sequences retain scrollback; an explicit history
clear or terminal reset can remove it. The history is kept in the current page,
not permanently on the tester. Reloading the page does not recover its old
scrollback.

During web control, the browser is the terminal input owner. Returning control
restores the prior UART/Telnet owner. Unsubmitted input is discarded on
disconnect; the bridge does not replay input across an antenna reconnect.

Receiver, Generator, and RS485 Bridge display histories are separate and limited
to 300 lines per mode. Device queues are also bounded. A prolonged disconnect or
slow reader can lose earlier output; these views are diagnostic logs, not a
lossless capture system. Clearing a display does not clear an external device.

## Appearance and network settings

**Day** and **Night** select the browser palette and are stored locally in the
browser. They are independent of the instrument LCD's **Metal**, **Color**, and
**B/W** themes and its 180-degree rotation setting.

Wi-Fi settings can change the network connection currently carrying the page.
If the address changes, open the new address shown on the tester. A blank new
password field keeps the existing password; use the explicit open-network
checkbox to remove it. An AP or STA settings group is saved as one NVS record.
A storage error is reported without applying that failed save to RAM or the
running radio. A later radio-apply error can occur after settings were saved.

Turning Wi-Fi off closes browser access. The ownership timeout then restores
touch control unless it has already been released with the LCD button.

## Implementation and limits

- `main/app/app_controller.c` serializes commands, mode changes, and ownership.
- `main/web/web_session.c` implements the ownership and timeout state machine.
- `main/web/web_server.c` serves the embedded page and `/ws`, checks Origin, and
  copies outgoing data into bounded queues through a separate dispatcher.
- `main/web/web_templates.c` validates types, ranges, coordinates, and dates.
- Navigation, AIS, and transport runtimes are independent of their local views.

State is published at up to five updates per second, and logs are batched.
These display rates do not set the UART/CAN protocol processing rates. The
browser terminal screen and history run on the browser; large device snapshots
and queues use PSRAM where configured. A single controlling session does not
mean a single HTTP socket: page requests and WebSocket transport have separate
connections.

CMake uses `main/web/build_assets.py` to generate deterministic gzip data from
`main/web/index.html` in the build directory. The page is embedded in the
application image; no separate SPIFFS web image or Node.js build is required.

For portable C checks, browser tests, and the HTTP dispatcher regression, see
[Tests](README.md#tests). Automated tests use simulated device or platform
interfaces and do not replace testing actual UART/CAN traffic, network loss,
resource usage, or prolonged operation on hardware.

# Updating the tester

Target: **Waveshare ESP32-S3 Touch LCD 5, 800×480, 16 MiB flash**.
Other panels and flash layouts are not interchangeable.

## What to download

The [firmware folder](../firmware/README.md) contains only:

- `NmeaTesterUpdater.exe`: portable Windows program with a matching application,
  bootloader and partition table embedded;
- `nmea-tester.bin`: unmodified ESP-IDF application image;
- `README.md`: quick instructions, source-build identity, SHA-256 and license notice.

Downloading the EXE is sufficient for the included firmware version. Use
**Browse** to select another compatible application BIN; **Use built-in**
returns to the included image. A raw application BIN is not a complete flash
dump. For initial USB installation the EXE supplies the remaining boot assets.
Old `.nmeafw` files are accepted for compatibility but are no longer required.

No ZIP, installer, .NET, Python or ESP-IDF is needed on the user's PC.
Windows 10/11 x64 are the primary targets. Windows 7 SP1 x64 compatibility has
not been verified on that OS. A USB serial driver may be needed.

## USB: install, update or recover

1. Connect the tester with a data-capable USB cable. Close serial monitors and
   detach it from WSL if it is attached there.
2. Select **USB** and the tester's COM port. Keep the built-in image or choose
   a compatible application BIN.
3. Click **Detect / Flash**. Detection restarts the tester into its loader and
   reads the chip, flash size, protection state and installed firmware layout.
4. Read the proposed action and selected version. Confirm only if this is the
   correct tester. No flash write occurs before confirmation.
5. Keep power connected until verification finishes. Check the running version
   in **About** after restart.

If automatic connection fails, hold **BOOT**, press **RESET**, release **BOOT**,
and retry. The program does not burn eFuses or enable Secure Boot/encryption;
protected devices are rejected.

With a compatible, intact installation, **UPDATE** writes the inactive OTA slot
and retains the current image as a fallback. With blank flash, an older or
incompatible layout, or damaged boot data, it proposes **INSTALL / RECOVER**.
Recovery replaces the application, bootloader, partition table and OTA selection.
No full-flash backup is saved. The NVS region is not deliberately erased, but
settings from an unknown layout cannot be guaranteed.

An I/O error or short read stops detection; it is never treated as blank flash.
An image awaiting startup verification is a separate state: allow the device to
boot normally before another update. Selecting an older version shows a
downgrade warning. Declining confirmation writes nothing and restarts the tester.

**Migration from older public firmware:** the old `1.2.6` build label used a
factory application partition and predates this updater format. It is not the
same numbering scheme as releases `1.1`, `1.2`, `1.3`, `1.4`. Make the first
transition using USB; the automatic plan will propose installation.

## Wi-Fi update

1. Enable Wi-Fi and connect the PC to the tester's AP or the same STA network.
2. Return browser control to the LCD, then open **Settings → Firmware update**
   on the tester. This explicitly stops the working mode.
3. Select **Wi-Fi** in the Windows program and enter the tester's IP address.
4. Keep the built-in image or select a BIN, then click **Detect / Flash**.
5. Check the current and selected versions, confirm, and wait for verification
   and restart. Wi-Fi is off after every restart; this is intentional.

The updater chooses port 80 for normal version queries and port 8080 for writes.
Entering the IP alone is enough. Wi-Fi cannot install a blank device or replace
an incompatible partition layout: those operations require USB.

## Read the version without stopping work

With firmware 1.3 or later, enable Wi-Fi, enter the IP and click
**Read device version**. Opening Firmware update on the LCD is unnecessary.
The normal `GET /api/v1/info` service on port 80 is read-only and does not claim
control or interrupt RX, TX, CAN, Telnet, or browser operation.

## Failure handling and limits

The Wi-Fi write service only exists while locally opened in update mode.
Idle update mode expires after ten minutes; one upload is limited to two minutes.
Size, board, layout and settings metadata are checked before starting an erase.
The upload must pass SHA-256 and ESP image validation before selecting the new slot.
Interrupted or rejected uploads do not select the incomplete image.

A new OTA image must pass essential initialization plus UI/controller startup
checks before it is confirmed. A 30-second pending-boot deadline bounds this
check. Rollback needs a previously bootable slot; initial installation has none.
These checks are not a full diagnostic of CAN, attached antennas or other equipment.

Initial USB installation is not atomic across power loss. If interrupted, retry
through BOOT/RESET and USB. Cancellation is deferred during boot-critical writes;
do not unplug the tester or close the program while those writes finish.

Images are unsigned. SHA-256 detects corruption, not publisher authenticity.
Use trusted downloads and a trusted local network: HTTP has no encryption or
separate update authentication. Nothing here is a navigation-safety certification.

## Firmware source builds

The public repository includes the instrument firmware and its host/browser
tests, but not Windows updater application sources or its packaging tools.
See [Build, flash, and monitor](../README.md#build-flash-and-monitor).

`VERSION` controls the firmware number in tenths: `1.4`, `1.5`, …, `1.9`, `2.0`.
It is separate from both the Windows updater version and NMEA protocol profiles.
A compatible `build/nmea-tester.bin` from ESP-IDF can be selected directly in
the prebuilt updater; no custom wrapper is needed.

The layout in `partitions.csv` is:

| Partition | Offset | Size |
|---|---:|---:|
| NVS | `0x9000` | `0x6000` |
| PHY init | `0xF000` | `0x1000` |
| OTA slot 0 | `0x10000` | `0x600000` |
| OTA slot 1 | `0x610000` | `0x600000` |
| OTA selection | `0xC10000` | `0x2000` |
| Storage | `0xC12000` | `0x3EE000` |

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is required. When reusing an old local
`sdkconfig`, update it or use the fresh-build instructions in the main README.
`idf.py flash` is a developer installation operation; it also rewrites initial
OTA selection, unlike an ordinary inactive-slot update.

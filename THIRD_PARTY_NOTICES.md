# Third-party notices

NMEA Tester contains or derives small portions of source code from the
projects listed below. These notices do not change the MIT license applied to
the original NMEA Tester source code.

## Espressif RGB/LVGL port

`main/lvgl_port/waveshare_lvgl_port.c` and
`main/lvgl_port/waveshare_lvgl_port.h` are adapted from Espressif example
code.

- Copyright: 2023-2024 Espressif Systems (Shanghai) CO LTD
- License: Apache License 2.0
- License text: [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt)

The files retain their original SPDX copyright and license identifiers.

## esp32_can API lineage

`main/can_module/driver/can_driver.c` and `can_driver.h` are an ESP-IDF TWAI
reimplementation with API and design lineage from Collin Kidder's
[`esp32_can`](https://github.com/collin80/esp32_can) library.

- Copyright: 2018 Collin Kidder
- License: MIT License
- License text: [`LICENSES/ESP32-CAN-MIT.txt`](LICENSES/ESP32-CAN-MIT.txt)

## LVGL Unscii font

`main/ui/lv_font_unscii_16.c` is a generated and horizontally condensed
variant of the built-in Unscii font shipped with LVGL 8.4.0. LVGL is licensed
under the MIT license. The generated file retains an MIT SPDX identifier and
documents the local transformation.

## Managed components

ESP-IDF Component Manager downloads the following dependencies during
configuration; their source is not vendored in this repository:

- LVGL (`lvgl/lvgl`), MIT license;
- ESP LCD Touch and the GT911 driver (`espressif/esp_lcd_touch*`), Apache-2.0.

See `main/idf_component.yml` and `dependencies.lock` for the exact resolved
versions. Each downloaded component includes its own license and notices.

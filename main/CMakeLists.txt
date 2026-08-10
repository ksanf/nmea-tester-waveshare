idf_component_register(
    SRCS
        # Application entry point
        app_main.c

        # Local Waveshare LVGL port
        lvgl_port/waveshare_lvgl_port.c

        # UI screens
        ui/screens/screen_init.c
        ui/screens/screen_ui.c
        ui/screens/screen_settings.c
        ui/screens/screen_wifi.c

        # UI widgets
        ui/dialog_ui.c
        ui/ui_theme.c
        ui/ui_nmea_widgets.c
        ui/ui_baud_selector.c
        ui/instrument_panel.c
        ui/nmea_log.c
        ui/lv_font_unscii_16.c

        # Touch driver
        touch/touch_driver.c

        # RS-485
        rs485/rs485_driver.c
        rs485/rs485_hex_dump.c
        rs485/rs485_rx_stream.c
        rs485/rs485_parser.c
        rs485/rs485_simui.c
        rs485/rs485_engine.c
        rs485/rs485_transmit.c
        rs485_bridge/rs485_bridge.c
        AISdecoder/aisdecoder_ui.c
        AISdecoder/aisdecoder_parse.c
        AISdecoder/aisdecoder_decode.c

        # CAN
        can_module/driver/can_driver.c
        can_module/screen_can.c
        can_module/nm2k_module/nm2k_module.c
        can_module/nm2k_module/nm2k_decoder.c
        can_module/nm2k_module/nm2k_decoder_fluid_level.c
        can_module/nm2k_module/nm2k_decoder_rudder.c
        can_module/nm2k_module/nm2k_decoder_gps.c
        can_module/nm2k_module/nm2k_decoder_ais.c
        can_module/nm2k_module/nm2k_decoder_nav.c
        can_module/nm2k_module/nm2k_decoder_engine.c
        can_module/bridge_can_module/bridge_can_module.c
        can_module/bridge_can_module/protocol_handler.c
        can_module/bridge_can_module/buffer_manager.c
        can_module/driver/serial_comm.c

        # Hardware-independent TT-6194 to TT-3027 protocol core
        can_module/protocol/src/fsm.c
        can_module/protocol/src/L5_service.c
        can_module/protocol/src/strategy_map.c
        can_module/protocol/src/l2_link.c
        can_module/protocol/src/l3l4_transport.c
        can_module/protocol/src/L5_terminal.c
        can_module/protocol/src/tlv_codec.c
        can_module/protocol/src/fsm_actions.c

        # NMEA 2000 driver
        can_module/n2k_driver/src/n2k_addr_claim.c
        can_module/n2k_driver/src/n2k_device_list.c
        can_module/n2k_driver/src/n2k_group_func.c
        can_module/n2k_driver/src/n2k_iso11783.c
        can_module/n2k_driver/src/n2k_iso_ack.c
        can_module/n2k_driver/src/n2k_pgn.c
        can_module/n2k_driver/src/n2k_product.c
        can_module/n2k_driver/src/n2k_transport.c

        # NMEA editor
        nmea_editor/nmea_editor.c
        nmea_editor/nmea_motion.c
        nmea_editor/nmea_wire.c
        nmea_editor/nmea_regen.c
        nmea_editor/nmea_templates.c
        nmea_editor/nmea_version.c
        nmea_editor/nmea_version_nvs.c
        nmea_editor/numeric_editor.c
        nmea_editor/nmea_tabs_gps.c
        nmea_editor/nmea_tabs_gyro.c
        nmea_editor/nmea_tabs_log.c
        nmea_editor/nmea_tabs_echo.c
        nmea_editor/nmea_tabs_weather.c

        # System modules
        system/nmea_clock.c
        system/rtc_driver.c
        system/nvs_rw.c
        system/telnet_router.c
        system/telnet_server.c
        system/udp_nmea_server.c
        system/board_waveshare.c
        system/wifi_ap.c

        # Wi-Fi manager
        wifi/wifi_manager.c

    INCLUDE_DIRS
        .
        config
        system
        touch
        rs485
        rs485_bridge
        AISdecoder
        can_module
        can_module/nm2k_module
        can_module/bridge_can_module
        can_module/driver
        can_module/protocol/include
        can_module/n2k_driver/include
        ui
        ui/screens
        nmea_editor
        wifi

    PRIV_REQUIRES
        lvgl__lvgl
        # espressif__esp_lvgl_port is replaced by the local copy in lvgl_port/.
        espressif__esp_lcd_touch_gt911
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        esp_timer
        esp_app_format
)

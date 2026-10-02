PROJECT_ROOT := ../..
BUILD_DIR := build
PORT_ACTIVITY_TARGET := $(BUILD_DIR)/test_port_activity
NMEA_TARGET := $(BUILD_DIR)/test_nmea_regen
NMEA_WIRE_TARGET := $(BUILD_DIR)/test_nmea_wire
HEX_TARGET := $(BUILD_DIR)/test_rs485_hex_dump
RX_STREAM_TARGET := $(BUILD_DIR)/test_rs485_rx_stream
AIS_TARGET := $(BUILD_DIR)/test_ais_decode
ROTATE_TARGET := $(BUILD_DIR)/test_framebuffer_rotate
LOG_MARKUP_TARGET := $(BUILD_DIR)/test_nmea_log_markup
WEB_SESSION_TARGET := $(BUILD_DIR)/test_web_session
NAVIGATION_TARGET := $(BUILD_DIR)/test_navigation_model
AIS_RUNTIME_TARGET := $(BUILD_DIR)/test_ais_runtime
CAN_RING_TARGET := $(BUILD_DIR)/test_can_byte_ring
SAILOR_TELEMETRY_TARGET := $(BUILD_DIR)/test_sailor_telemetry
SAILOR_STATUS_TARGET := $(BUILD_DIR)/test_sailor_status_format
TT6006_PROFILE_TARGET := $(BUILD_DIR)/test_tt6006_profile
SAILOR_SESSION_TARGET := $(BUILD_DIR)/test_sailor_ndp_session
SAILOR_PAGES_TARGET := $(BUILD_DIR)/test_sailor_ndp_pages
WEB_TEMPLATES_TARGET := $(BUILD_DIR)/test_web_templates
NUMERIC_VALIDATION_TARGET := $(BUILD_DIR)/test_numeric_editor_validation
RTC_CLOCK_TARGET := $(BUILD_DIR)/test_nmea_clock_rtc
CPU_CLOCK_TARGET := $(BUILD_DIR)/test_nmea_clock_cpu
NM2K_IDENTITY_TARGET := $(BUILD_DIR)/test_nm2k_identity
TELNET_RX_TARGET := $(BUILD_DIR)/test_telnet_rx
CAN_LINK_WORKER_TARGET := $(BUILD_DIR)/test_can_link_worker
WIFI_MANAGER_TARGET := $(BUILD_DIR)/test_wifi_manager
# Use the same cJSON source as the firmware, without a system package dependency.
IDF_PATH ?= $(shell python3 -c 'import json; print(json.load(open("$(PROJECT_ROOT)/build/project_description.json"))["idf_path"])' 2>/dev/null)
CJSON_DIR ?= $(IDF_PATH)/components/json/cJSON

CC ?= cc
CPPFLAGS += -I$(CURDIR)/stubs -I$(PROJECT_ROOT)/main -I$(PROJECT_ROOT)/main/config
CFLAGS += -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test

all: $(PORT_ACTIVITY_TARGET) $(NMEA_TARGET) $(NMEA_WIRE_TARGET) $(HEX_TARGET) $(RX_STREAM_TARGET) $(AIS_TARGET) $(ROTATE_TARGET) $(LOG_MARKUP_TARGET) $(WEB_SESSION_TARGET) $(NAVIGATION_TARGET) $(AIS_RUNTIME_TARGET) $(CAN_RING_TARGET) $(SAILOR_TELEMETRY_TARGET) $(SAILOR_STATUS_TARGET) $(SAILOR_PAGES_TARGET) $(SAILOR_SESSION_TARGET) $(TT6006_PROFILE_TARGET) $(WEB_TEMPLATES_TARGET) $(NUMERIC_VALIDATION_TARGET) $(RTC_CLOCK_TARGET) $(CPU_CLOCK_TARGET) $(NM2K_IDENTITY_TARGET) $(TELNET_RX_TARGET) $(CAN_LINK_WORKER_TARGET) $(WIFI_MANAGER_TARGET)

test: $(PORT_ACTIVITY_TARGET) $(NMEA_TARGET) $(NMEA_WIRE_TARGET) $(HEX_TARGET) $(RX_STREAM_TARGET) $(AIS_TARGET) $(ROTATE_TARGET) $(LOG_MARKUP_TARGET) $(WEB_SESSION_TARGET) $(NAVIGATION_TARGET) $(AIS_RUNTIME_TARGET) $(CAN_RING_TARGET) $(SAILOR_TELEMETRY_TARGET) $(SAILOR_STATUS_TARGET) $(SAILOR_PAGES_TARGET) $(SAILOR_SESSION_TARGET) $(TT6006_PROFILE_TARGET) $(WEB_TEMPLATES_TARGET) $(NUMERIC_VALIDATION_TARGET) $(RTC_CLOCK_TARGET) $(CPU_CLOCK_TARGET) $(NM2K_IDENTITY_TARGET) $(TELNET_RX_TARGET) $(CAN_LINK_WORKER_TARGET) $(WIFI_MANAGER_TARGET)
	$(PORT_ACTIVITY_TARGET)
	$(NMEA_TARGET)
	$(NMEA_WIRE_TARGET)
	$(HEX_TARGET)
	$(RX_STREAM_TARGET)
	$(AIS_TARGET)
	$(ROTATE_TARGET)
	$(LOG_MARKUP_TARGET)
	$(WEB_SESSION_TARGET)
	$(NAVIGATION_TARGET)
	$(AIS_RUNTIME_TARGET)
	$(CAN_RING_TARGET)
	$(SAILOR_TELEMETRY_TARGET)
	$(SAILOR_STATUS_TARGET)
	$(SAILOR_PAGES_TARGET)
	$(SAILOR_SESSION_TARGET)
	$(TT6006_PROFILE_TARGET)
	$(WEB_TEMPLATES_TARGET)
	$(NUMERIC_VALIDATION_TARGET)
	$(RTC_CLOCK_TARGET)
	$(CPU_CLOCK_TARGET)
	$(NM2K_IDENTITY_TARGET)
	$(TELNET_RX_TARGET)
	$(CAN_LINK_WORKER_TARGET)
	$(WIFI_MANAGER_TARGET)

$(NMEA_TARGET): test_nmea_regen.c \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_regen.c \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_motion.c \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_version.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -lm -o $@

$(NMEA_WIRE_TARGET): test_nmea_wire.c \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_wire.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(HEX_TARGET): test_rs485_hex_dump.c $(PROJECT_ROOT)/main/rs485/rs485_hex_dump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(RX_STREAM_TARGET): test_rs485_rx_stream.c \
	$(PROJECT_ROOT)/main/rs485/rs485_rx_stream.c \
	$(PROJECT_ROOT)/main/rs485/rs485_hex_dump.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(AIS_TARGET): test_ais_decode.c \
	$(PROJECT_ROOT)/main/AISdecoder/aisdecoder_decode.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=gnu11 -Wall -Wextra -Werror $^ -lm -o $@

$(ROTATE_TARGET): test_framebuffer_rotate.c \
	$(PROJECT_ROOT)/main/lvgl_port/framebuffer_rotate.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(LOG_MARKUP_TARGET): test_nmea_log_markup.c \
	$(PROJECT_ROOT)/main/ui/nmea_log_markup.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(WEB_SESSION_TARGET): test_web_session.c \
	$(PROJECT_ROOT)/main/web/web_session.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(NAVIGATION_TARGET): test_navigation_model.c \
	$(PROJECT_ROOT)/main/app/navigation_model.c \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_wire.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=gnu11 -Wall -Wextra -Werror -pthread $^ -lm -o $@

$(AIS_RUNTIME_TARGET): test_ais_runtime.c \
	$(PROJECT_ROOT)/main/AISdecoder/aisdecoder_runtime.c \
	$(PROJECT_ROOT)/main/AISdecoder/aisdecoder_parse.c \
	$(PROJECT_ROOT)/main/AISdecoder/aisdecoder_decode.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=gnu11 -Wall -Wextra -Werror -pthread $^ -lm -o $@

$(CAN_RING_TARGET): $(PROJECT_ROOT)/main/can_module/tests/test_can_byte_ring.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(NUMERIC_VALIDATION_TARGET): test_numeric_editor_validation.c $(PROJECT_ROOT)/main/nmea_editor/numeric_editor_validation.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) test_numeric_editor_validation.c -o $@

$(WEB_TEMPLATES_TARGET): test_web_templates.c \
	$(PROJECT_ROOT)/main/web/web_templates.c \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_templates.c \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_motion.c \
	$(CJSON_DIR)/cJSON.c \
	$(PROJECT_ROOT)/main/web/web_templates.h \
	$(PROJECT_ROOT)/main/nmea_editor/nmea_templates.h \
	$(wildcard stubs/templates/*.h stubs/templates/freertos/*.h) stubs/esp_err.h | $(BUILD_DIR)
	$(CC) -I$(CURDIR)/stubs/templates $(CPPFLAGS) -I$(CJSON_DIR) \
		-include $(CURDIR)/stubs/templates/compat.h \
		-std=gnu11 -Wall -Wextra -Werror -pthread $(filter %.c,$^) -lm -o $@

$(BUILD_DIR):
	mkdir -p $@

$(PORT_ACTIVITY_TARGET): test_port_activity.c $(PROJECT_ROOT)/main/system/port_activity.c $(PROJECT_ROOT)/main/system/port_activity.h $(PROJECT_ROOT)/main/ui/activity_pulse.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(filter %.c,$^) -o $@

$(SAILOR_TELEMETRY_TARGET): $(PROJECT_ROOT)/main/can_module/tests/test_sailor_telemetry.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/sailor_telemetry.c \
	$(PROJECT_ROOT)/main/can_module/protocol/include/sailor_telemetry.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(PROJECT_ROOT)/main/can_module/protocol/include $(CFLAGS) $(filter %.c,$^) -lm -o $@

$(SAILOR_PAGES_TARGET): $(PROJECT_ROOT)/main/can_module/tests/test_sailor_ndp_pages.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/sailor_ndp_pages.c \
	$(PROJECT_ROOT)/main/can_module/protocol/include/sailor_ndp_pages.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(PROJECT_ROOT)/main/can_module/protocol/include $(CFLAGS) $(filter %.c,$^) -o $@

$(SAILOR_SESSION_TARGET): $(PROJECT_ROOT)/main/can_module/tests/test_sailor_ndp_session.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/sailor_ndp_session.c \
	$(PROJECT_ROOT)/main/can_module/protocol/include/sailor_ndp_session.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(PROJECT_ROOT)/main/can_module/protocol/include $(CFLAGS) $(filter %.c,$^) -o $@

$(TT6006_PROFILE_TARGET): tt6006_profile/test_tt6006_profile.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/fsm.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/sailor_ndp_session.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/sailor_telemetry.c \
	$(wildcard tt6006_profile/*.h tt6006_profile/freertos/*.h) \
	$(wildcard $(PROJECT_ROOT)/main/can_module/protocol/include/*.h) | $(BUILD_DIR)
	$(CC) -I$(CURDIR)/tt6006_profile $(CPPFLAGS) -I$(PROJECT_ROOT)/main/can_module/protocol/include $(CFLAGS) $(filter %.c,$^) -lm -o $@

$(SAILOR_STATUS_TARGET): $(PROJECT_ROOT)/main/can_module/tests/test_sailor_status_format.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/sailor_status_format.c \
	$(PROJECT_ROOT)/main/can_module/protocol/include/sailor_status_format.h \
	$(PROJECT_ROOT)/main/can_module/protocol/include/sailor_telemetry.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(PROJECT_ROOT)/main/can_module/protocol/include $(CFLAGS) $(filter %.c,$^) -o $@

$(RTC_CLOCK_TARGET) $(CPU_CLOCK_TARGET): test_nmea_clock.c \
	$(PROJECT_ROOT)/main/system/nmea_clock.c \
	$(PROJECT_ROOT)/main/system/nmea_clock.h \
	$(wildcard clock_stubs/*.h clock_stubs/config/*.h clock_stubs/nmea_editor/*.h clock_stubs/freertos/*.h) | $(BUILD_DIR)
	$(CC) -I$(CURDIR)/clock_stubs $(CPPFLAGS) -pthread -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Werror \
		-Dtime=clock_test_time -Dsettimeofday=clock_test_settimeofday \
		-DNMEA_CLOCK_FROM_RTC=$(if $(filter $(CPU_CLOCK_TARGET),$@),0,1) $(filter %.c,$^) -o $@

$(NM2K_IDENTITY_TARGET): test_nm2k_identity.c \
	$(PROJECT_ROOT)/main/can_module/nm2k_module/nm2k_identity.c \
	$(PROJECT_ROOT)/main/can_module/nm2k_module/nm2k_identity.h \
	stubs/nm2k_identity/esp_mac.h stubs/esp_err.h | $(BUILD_DIR)
	$(CC) -I$(CURDIR)/stubs/nm2k_identity $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@

$(TELNET_RX_TARGET): test_telnet_rx.c $(PROJECT_ROOT)/main/system/telnet_rx.c \
	$(PROJECT_ROOT)/main/system/telnet_rx.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) -o $@

$(CAN_LINK_WORKER_TARGET): test_can_link_worker.c \
	$(PROJECT_ROOT)/main/can_module/protocol/src/l2_link.c \
	$(PROJECT_ROOT)/main/can_module/bridge_can_module/protocol_poll_worker.c \
	$(PROJECT_ROOT)/main/can_module/bridge_can_module/protocol_poll_worker.h \
	$(wildcard can_link_worker_stubs/*.h can_link_worker_stubs/freertos/*.h) | $(BUILD_DIR)
	$(CC) -I$(CURDIR)/can_link_worker_stubs -I$(PROJECT_ROOT)/main/can_module/protocol/include \
		-I$(PROJECT_ROOT)/main/can_module/bridge_can_module $(CFLAGS) -pthread $(filter %.c,$^) -o $@

$(WIFI_MANAGER_TARGET): test_wifi_manager.c $(PROJECT_ROOT)/main/wifi/wifi_manager.c \
	$(PROJECT_ROOT)/main/wifi/wifi_manager.h \
	$(wildcard wifi_manager/*.h wifi_manager/config/*.h wifi_manager/freertos/*.h) \
	stubs/esp_err.h stubs/templates/compat.h | $(BUILD_DIR)
	$(CC) -I$(CURDIR)/wifi_manager $(CPPFLAGS) -std=gnu11 -Wall -Wextra -Werror test_wifi_manager.c -o $@

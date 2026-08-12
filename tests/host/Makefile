PROJECT_ROOT := ../..
BUILD_DIR := build
NMEA_TARGET := $(BUILD_DIR)/test_nmea_regen
NMEA_WIRE_TARGET := $(BUILD_DIR)/test_nmea_wire
HEX_TARGET := $(BUILD_DIR)/test_rs485_hex_dump
RX_STREAM_TARGET := $(BUILD_DIR)/test_rs485_rx_stream
AIS_TARGET := $(BUILD_DIR)/test_ais_decode
ROTATE_TARGET := $(BUILD_DIR)/test_framebuffer_rotate

CC ?= cc
CPPFLAGS += -I$(CURDIR)/stubs -I$(PROJECT_ROOT)/main -I$(PROJECT_ROOT)/main/config
CFLAGS += -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test

all: $(NMEA_TARGET) $(NMEA_WIRE_TARGET) $(HEX_TARGET) $(RX_STREAM_TARGET) $(AIS_TARGET) $(ROTATE_TARGET)

test: $(NMEA_TARGET) $(NMEA_WIRE_TARGET) $(HEX_TARGET) $(RX_STREAM_TARGET) $(AIS_TARGET) $(ROTATE_TARGET)
	$(NMEA_TARGET)
	$(NMEA_WIRE_TARGET)
	$(HEX_TARGET)
	$(RX_STREAM_TARGET)
	$(AIS_TARGET)
	$(ROTATE_TARGET)

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

$(BUILD_DIR):
	mkdir -p $@

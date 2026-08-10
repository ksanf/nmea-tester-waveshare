PROJECT_ROOT := ../..
BUILD_DIR := build
NMEA_TARGET := $(BUILD_DIR)/test_nmea_regen
NMEA_WIRE_TARGET := $(BUILD_DIR)/test_nmea_wire
HEX_TARGET := $(BUILD_DIR)/test_rs485_hex_dump
RX_STREAM_TARGET := $(BUILD_DIR)/test_rs485_rx_stream

CC ?= cc
CPPFLAGS += -I$(CURDIR)/stubs -I$(PROJECT_ROOT)/main
CFLAGS += -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test

all: $(NMEA_TARGET) $(NMEA_WIRE_TARGET) $(HEX_TARGET) $(RX_STREAM_TARGET)

test: $(NMEA_TARGET) $(NMEA_WIRE_TARGET) $(HEX_TARGET) $(RX_STREAM_TARGET)
	$(NMEA_TARGET)
	$(NMEA_WIRE_TARGET)
	$(HEX_TARGET)
	$(RX_STREAM_TARGET)

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

$(BUILD_DIR):
	mkdir -p $@

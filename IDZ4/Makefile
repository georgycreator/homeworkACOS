# Makefile — сборка IDZ4 без CMake (делает то же, что CMakeLists.txt).
#   make                       обычная сборка в build/
#   make BUILD=build-asan EXTRA_CFLAGS="-fsanitize=address,undefined -O1" LDFLAGS="-fsanitize=address,undefined"
#                              сборка с санитайзерами (проверка памяти)
CC       ?= gcc
CFLAGS   ?= -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
ALL_CFLAGS = $(CFLAGS) $(EXTRA_CFLAGS) -D_POSIX_C_SOURCE=200809L -Iinclude
BUILD    ?= build

COMMON  := util proto net log io colors signals grid artillery side config
COMMON_OBJ := $(patsubst %,$(BUILD)/common/%.o,$(COMMON))

BINS := $(BUILD)/idz4_server $(BUILD)/idz4_side $(BUILD)/idz4_artillery $(BUILD)/idz4_observer

all: $(BINS)

$(BUILD)/common/%.o: src/common/%.c | $(BUILD)/common
	$(CC) $(ALL_CFLAGS) -c $< -o $@
$(BUILD)/server/%.o: src/server/%.c | $(BUILD)/server
	$(CC) $(ALL_CFLAGS) -c $< -o $@
$(BUILD)/client/%.o: src/client/%.c | $(BUILD)/client
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/common $(BUILD)/server $(BUILD)/client:
	mkdir -p $@

$(BUILD)/idz4_server: $(BUILD)/server/server.o $(BUILD)/server/battle.o $(COMMON_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)
$(BUILD)/idz4_side: $(BUILD)/client/side_main.o $(BUILD)/client/client_common.o $(COMMON_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)
$(BUILD)/idz4_artillery: $(BUILD)/client/artillery_main.o $(BUILD)/client/client_common.o $(COMMON_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)
$(BUILD)/idz4_observer: $(BUILD)/client/observer_main.o $(BUILD)/client/client_common.o $(COMMON_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf build build-asan

.PHONY: all clean

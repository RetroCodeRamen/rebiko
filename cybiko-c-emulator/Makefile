# Host build. Does not flash a T-Deck.
CC      ?= gcc
CFLAGS  ?= -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Isrc -Isrc/radio -Itests -I../cybiko-lilygo/cybiko-emu -O2 -g
CORE := \
	src/core/memory.c src/core/hd66421.c src/core/timer8.c src/core/timer16.c \
	src/core/rtc.c src/core/speaker.c src/core/keyboard.c src/core/h8s_cpu.c \
	src/core/address_bus.c src/core/cfs.c src/core/emulator.c
RADIO := src/radio/radio_link.c src/radio/radio_coproc.c src/radio/radio_sim.c src/radio/sci2_xt.c src/radio/radio_lora.c
KEYMAP := ../cybiko-lilygo/cybiko-emu/tdeck_keymap.c

CORE_TESTS := memory cfs keyboard timer8 timer16 speaker hd66421 h8s_cpu sci2 cyid
OUT := build

.PHONY: test harness clean

test: $(CORE_TESTS:%=$(OUT)/test_%) $(OUT)/test_radio $(OUT)/test_sci0 $(OUT)/test_lora $(OUT)/test_tdeck_keymap
	@fail=0; \
	for t in $(CORE_TESTS:%=$(OUT)/test_%) $(OUT)/test_radio $(OUT)/test_sci0 $(OUT)/test_lora $(OUT)/test_tdeck_keymap; do \
		echo "== $$t"; \
		$$t || fail=1; \
	done; \
	exit $$fail

harness: $(OUT)/radio_harness
	$(OUT)/radio_harness

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_%: tests/test_%.c $(CORE) $(RADIO) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_$*.c $(CORE) $(RADIO)

# The Classic's SCI0 (sci0_v1.c) is built only by the T-Deck's V1 core
$(OUT)/test_sci0: tests/test_sci0.c src/radio/sci0_v1.c $(RADIO) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_sci0.c src/radio/sci0_v1.c $(RADIO)

$(OUT)/test_tdeck_keymap: tests/test_tdeck_keymap.c $(KEYMAP) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tests/test_tdeck_keymap.c $(KEYMAP)

$(OUT)/radio_harness: tools/radio_harness.c $(RADIO) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/radio_harness.c $(RADIO)

# SDL2 frontend. --radio /dev/ttyUSB0 uses a Heltec modem.
$(OUT)/cybiko-emu: src/linux/main.c src/radio/radio_serial_phy.c $(CORE) $(RADIO) | $(OUT)
	$(CC) $(CFLAGS) $$(pkg-config --cflags sdl2) -o $@ src/linux/main.c src/radio/radio_serial_phy.c $(CORE) $(RADIO) $$(pkg-config --libs sdl2)

$(OUT)/modem_tool: tools/modem_tool.c src/radio/radio_serial_phy.c $(RADIO) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/modem_tool.c src/radio/radio_serial_phy.c $(RADIO)

$(OUT)/two_boot: tools/two_boot.c $(CORE) $(RADIO) | $(OUT)
	$(CC) $(CFLAGS) -o $@ tools/two_boot.c $(CORE) $(RADIO)

clean:
	rm -rf $(OUT)

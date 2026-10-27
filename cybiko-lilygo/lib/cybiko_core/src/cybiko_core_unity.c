/*
 * Unity build: all core files compiled as a single translation unit.
 * This allows the compiler to inline hot functions across files
 * (e.g., bus_read16 into h8s_cpu_step) which eliminates function call
 * overhead on the critical path — critical for ESP32 performance.
 */
#ifdef ESP_PLATFORM
#pragma GCC optimize ("O3")
#endif
#include "core/memory.c"
#include "core/hd66421.c"
#include "core/timer8.c"
#include "core/timer16.c"
#include "core/rtc.c"
#include "core/speaker.c"
#include "core/keyboard.c"
#include "core/at45db041.c"
#include "core/cfs.c"
#include "core/uart_v1.c"
#include "core/h8s_cpu.c"
#include "core/address_bus.c"
#include "core/emulator.c"

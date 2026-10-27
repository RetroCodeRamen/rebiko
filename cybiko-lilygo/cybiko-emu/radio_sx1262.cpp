#include "radio_sx1262.h"

#include <Arduino.h>
#include <SPI.h>
#include <esp_mac.h>
#include <esp_random.h>

#include "tdeck_pins.h"

static cybiko_lora_t link_t;
static bool started;
static uint32_t device_id;

#ifndef CYBIKO_RADIO_TX
#define CYBIKO_RADIO_TX 0
#endif

int cybiko_sx1262_begin(bool (*bus_free)(void)) {
    cybiko_sx1262_pins_t pins = {};
    pins.cs = PIN_RADIO_CS;
    pins.rst = PIN_RADIO_RST;
    pins.busy = PIN_RADIO_BUSY;
    pins.dio1 = PIN_RADIO_DIO1;
    pins.hold_high[0] = PIN_TFT_CS;
    pins.hold_high[1] = PIN_SD_CS;
    pins.spi = &SPI;
    pins.bus_free = bus_free;
    cybiko_lora_profile_t prof;
    cybiko_lora_profile_default(&prof);
    started = false;
    int rc = cybiko_sx1262_phy_begin(&pins, &prof);
    if (rc != 0)
        return rc;
    /* CyID 0x4C + low 24 bits of the ESP32 MAC, so two boards running the
     * same flash.bin differ. main.cpp patches the guest flash to match:
     * the co-processor reports the link sender id to CyOS as the source. */
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    device_id = 0x4C000000u | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    cybiko_lora_init(&link_t, cybiko_sx1262_phy(), device_id, CYBIKO_RADIO_TX != 0);
    cybiko_lora_seed_msg_id(&link_t, esp_random());
    started = true;
    return 0;
}

uint32_t cybiko_sx1262_device_id(void) { return device_id; }
uint8_t cybiko_sx1262_chip_status(void) { return cybiko_sx1262_phy_stats()->status; }
uint16_t cybiko_sx1262_device_errors(void) { return cybiko_sx1262_phy_stats()->device_errors; }
const char *cybiko_sx1262_last_error(void) { return cybiko_sx1262_phy_error(); }

cybiko_radio_transport_t *cybiko_sx1262_transport(void) {
    return started ? &link_t.base : NULL;
}

const cybiko_lora_t *cybiko_sx1262_link(void) { return started ? &link_t : NULL; }
void cybiko_sx1262_on_control(void (*fn)(void *, const uint8_t *, size_t, uint32_t), void *user) {
    link_t.on_control = fn;
    link_t.control_user = user;
}

void cybiko_sx1262_pump(void) {
    if (started)
        link_t.base.pump(&link_t.base);
}

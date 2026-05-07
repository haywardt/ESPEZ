#include <ESPEZ.h>

#define NETWORK_ID   ((uint64_t)0xCAFE << 24)
#define TOPIC_BUTTON (NETWORK_ID | 0x01)

#define BOOT_BUTTON 0  // GPIO 0 on ESP32-S2

ESPEZNode node;

void setup() {
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    node.begin(NETWORK_ID, 16, 6);
}

void loop() {
    uint8_t val = digitalRead(BOOT_BUTTON) == LOW ? 1 : 0;
    node.publish(TOPIC_BUTTON, &val, sizeof(val));
    node.loop();
    delay(20);
}

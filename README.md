# ESPEZ
A simple mesh Pub/Sub instrumentation Network.
# ESPEZ - Minimal Mesh Protocol for ESP-NOW

ESPEZ is a lightweight publish/subscribe mesh network for ESP32 and ESP8266 devices. It requires no pairing, no routing tables, and no complex configuration.

## Features

- Publish/subscribe messaging across multiple devices
- Automatic mesh flooding with duplicate suppression
- Optional elastic mode for self-adapting relay behavior
- No guaranteed delivery — send state, not change
- Topics are 40-bit identifiers (not strings)
- Payload up to 230 bytes

## Quick Start

```cpp
#include <ESPEZ.h>

ESPEZNode node(false);  // false = no relay

const uint64_t TOPIC_TEMP = 0x54454D50;  // "TEMP"

void setup() {
    node.begin();
}

void loop() {
    int temp = analogRead(34);
    node.publish(TOPIC_TEMP, (uint8_t*)&temp, sizeof(temp));
    delay(1000);
    node.loop();
}

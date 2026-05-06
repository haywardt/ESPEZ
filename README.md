# ESPEZ

A lightweight publish/subscribe mesh network for ESP32 and ESP8266 devices. No pairing. No routing tables. No infrastructure.

Any node can publish to a topic. Any node can subscribe to a topic. The mesh handles the rest.

---

## Why ESPEZ Exists

In many applications, the easiest solution is to use multiple ESP. If you need more IO pins, just add ESPs. Need more processing power? Add an ESP. components need to be in different locations? Add an ESP. If you want it a controller communication, you have to design the protocol and physical layer yourself. WiFi needs an access point. Bluetooth range is limited. LoRa requires additional hardware.

ESP-NOW solves the radio problem. ESPEZ solves the networking problem — giving you publish/subscribe messaging across multiple devices without managing connections, pairing tables, or routing logic.

**Good fits:**
- Sensor clusters in a factory or laboratory
- Environmental monitoring across a building
- Robot swarms and multi-controller systems
- Boat telemetry
- Home automation without WiFi
- Any project where multiple controllers would be desirable.

**Not a good fit:**
- Applications requiring guaranteed delivery of every packet
- High-frequency control loops with tight timing requirements
- Streaming audio or video

---

## How It Works

ESPEZ hijacks the sender's MAC address field to carry the topic (5 bytes) and sequence number (1 byte). Every broadcast frame contains exactly 6 bytes of overhead in a field that ESP-NOW already transmits. The payload follows as normal.

Each node maintains a hash table of recently seen messages. Different nodes use different hash salts, so a collision on one node rarely causes a collision on another. The mesh self-heals.

Key behaviors:
- Local delivery always occurs if the node is subscribed to the topic — the hash table never blocks local reception
- Retransmission only occurs if the node has relaying enabled and the message hash bit was not already set
- Different nodes use different hash salts, so collisions don't cascade through the mesh

---

## Quick Start

```cpp
#include <ESPEZ.h>

ESPEZNode node(false);  // false = endpoint only, no relay

const uint64_t TOPIC_TEMP = 0x54454D5000;

void setup() {
    node.begin();
}

void loop() {
    int temp = analogRead(34);
    node.publish(TOPIC_TEMP, (uint8_t*)&temp, sizeof(temp));
    delay(1000);
    node.loop();
}
```

---

## API Reference

### Creating a Node

```cpp
ESPEZNode node(bool enableRelay);
```

`enableRelay = true` — Node acts as a mesh relay, forwarding every valid message it receives.  
`enableRelay = false` — Node is an endpoint only. Publishes and subscribes but does not retransmit.

### Initialization

```cpp
node.begin();
```

Call once in `setup()`. Initializes ESP-NOW, configures the broadcast peer, and prepares the hash table.

### Publishing

```cpp
node.publish(uint64_t topic, const uint8_t* payload, size_t len);
```

Topics are 40-bit values (0 to 2^40−1). Payload is limited to ~230 bytes. The sequence number is managed automatically and rolls over from 255 to 0.

### Subscribing

```cpp
node.subscribe(uint64_t topic);
```

Only messages matching a subscribed topic are delivered to the callback.

### Receiving Messages

```cpp
node.onMessage(void (*callback)(uint64_t topic, const uint8_t* payload, size_t len));
```

Registers a callback invoked whenever a subscribed message arrives.

### Event Loop

```cpp
node.loop();
```

Must be called frequently from `loop()`. Handles incoming messages, duplicate suppression, and hash table maintenance.

### Diagnostics

```cpp
uint32_t getErrorCount();
void resetErrorCount();
```

ESPEZ maintains a count of detected errors (checksum failures, out-of-sequence messages). Diagnostic only — no corrective action is taken.

---

## Configuration

All configuration is done at compile time in `ESPEZ.h`:

```cpp
#define ESPEZ_HASH_SIZE 256      // Bits in duplicate table (default 256)
#define ESPEZ_CLEAR_INTERVAL 1   // Seconds between hash table clears (default 1)
```

| Hash Size | RAM Used |
|-----------|----------|
| 256 bits  | 32 bytes |
| 512 bits  | 64 bytes |
| 1024 bits | 128 bytes |

Larger hash tables reduce false duplicates. Shorter clear intervals make the mesh more responsive to new messages.

---

## Examples

### Publish-Only Node

```cpp
#include <ESPEZ.h>

ESPEZNode sensor(false);

const uint64_t TOPIC_TEMPERATURE = 0x54454D5000;

void setup() {
    sensor.begin();
}

void loop() {
    int temp = analogRead(34);
    uint8_t payload[2] = {temp >> 8, temp & 0xFF};
    sensor.publish(TOPIC_TEMPERATURE, payload, 2);
    delay(1000);
    sensor.loop();
}
```

### Subscribe-Only Node

```cpp
#include <ESPEZ.h>

ESPEZNode display(false);

const uint64_t TOPIC_TEMPERATURE = 0x54454D5000;

void onMessage(uint64_t topic, const uint8_t* payload, size_t len) {
    if (topic == TOPIC_TEMPERATURE && len == 2) {
        int temp = (payload[0] << 8) | payload[1];
        Serial.print("Temperature: ");
        Serial.println(temp);
    }
}

void setup() {
    Serial.begin(115200);
    display.begin();
    display.subscribe(TOPIC_TEMPERATURE);
    display.onMessage(onMessage);
}

void loop() {
    display.loop();
}
```

### Relay Node

```cpp
#include <ESPEZ.h>

ESPEZNode relay(true);

void setup() {
    relay.begin();
}

void loop() {
    relay.loop();
}
```

---

## Design Guidelines

### Send State, Not Change

ESPEZ makes no guarantee of delivery. Packets will be lost.

```cpp
// Wrong — a single lost packet causes permanent drift
int delta = currentThrottle - lastThrottle;
node.publish(TOPIC_THROTTLE, &delta, 1);

// Right — a lost packet means one stale cycle, corrected by the next
int throttle = readThrottle();
node.publish(TOPIC_THROTTLE, &throttle, 1);
```

Every message should be a complete snapshot of current state, not a delta from the previous state.

### Safety-Critical Systems: Use Timeouts

For fire, gas, and alarm systems, broadcast state continuously and treat silence as failure.

```cpp
// Sender: broadcast state on every cycle
void loop() {
    uint8_t payload[1] = {gasDetected ? 1 : 0};
    node.publish(TOPIC_GAS, payload, 1);
    delay(2000);
    node.loop();
}

// Receiver: alarm if no message arrives within 3x the send interval
void loop() {
    node.loop();
    if (millis() - lastGasPacket > 6000) {
        activateWarning("GAS SENSOR OFFLINE");
        gasAlarm = true;  // Fail-safe
    }
    if (gasAlarm) {
        soundHorn();
        activateVentilation();
    }
}
```

If you're not sure, alarm. A lossy protocol becomes fail-safe when silence triggers action.

### Network Isolation

ESPEZ uses broadcast addressing. Two ESPEZ meshes within radio range will see each other's traffic. In shared spaces, use a network ID:

```cpp
#define NETWORK_ID 0xCAFE
#define TOPIC_TEMP  ((uint64_t)NETWORK_ID << 24) | 0x01
#define TOPIC_FIRE  ((uint64_t)NETWORK_ID << 24) | 0x02
```

Human-readable topics like `0x54454D50` ("TEMP") are public channels. If you don't control every ESPEZ device within radio range, use a network ID.

### Diagnostics Topic

ESPEZ reserves topic `0x0000000000` for network diagnostics. Subscribe to it to receive health information from all nodes in the mesh.

```cpp
node.subscribe(0x0000000000);
node.onMessage(onDiagnostic);
```

A mesh that can't diagnose itself is a black box.

---

## Scaling

With ESP-NOW at 1 Mbps and typical 10-byte payloads:

- **Relay nodes:** ~500 before channel saturation
- **Receive-only nodes:** Unlimited
- **Update rate per node:** 10–20 Hz typical

---

## Golden Rules

- Send state, not change
- Use timeouts for fail-safe operation
- Add a network ID in shared spaces
- Subscribe to the diagnostics topic
- Enable relays only when needed

---

*The protocol was originally developed for a twin-screw yacht with two helm stations, a generator room, and a wastewater treatment plant — a challenging environment with steel bulkheads and long distances. The yacht was lost to Hurricane Sally before the project was complete. The code survived.*

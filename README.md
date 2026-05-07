# ESPEZ

A lightweight publish/subscribe mesh network for ESP32 and ESP8266 devices. No pairing. No routing tables. No infrastructure. Works on all ESPs that support ESPNow.

Any node can publish to a topic. All nodes are subscribed to all topics. The mesh handles the rest.

---

## Installation

1. Download or clone this repository.
2. Copy the folder to your Arduino libraries directory (typically `~/Arduino/libraries/ESPEZ/`), or install from a `.zip` via **Sketch → Include Library → Add .ZIP Library** in the Arduino IDE.
3. Include the library in your sketch with `#include <ESPEZ.h>`.

No additional dependencies are required. The ESP-NOW and WiFi libraries are included with the [arduino-esp32](https://github.com/espressif/arduino-esp32) and [arduino-esp8266](https://github.com/esp8266/Arduino) board packages.

---

## Why ESPEZ Exists

In many applications, the easiest solution is to use multiple ESPs. If you need more IO pins, just add ESPs. Need more processing power? Add an ESP. components need to be in different locations? Add more devices and let them talk.

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

## Quick Start

Three sketches that work together: a publisher reading the built-in button, a relay forwarding messages through the mesh, and a receiver controlling the built-in LED. All three use the same network ID and channel.

Ready-to-flash versions are in the `examples/` directory. The example receiver targets boards with a WS2812B RGB LED (e.g. Waveshare ESP32-C3-Zero) and requires the [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) library. The example relay adds help channel diagnostics on Serial at 115200 baud.

These roles are not exclusive — any node can publish, receive, and relay simultaneously. The sketches are separated here for clarity.

### Publisher — built-in button

```cpp
#include <ESPEZ.h>

#define NETWORK_ID   ((uint64_t)0xCAFE << 24)
#define TOPIC_BUTTON (NETWORK_ID | 0x01)

ESPEZNode node;

void setup() {
    pinMode(0, INPUT_PULLUP);  // BOOT button, active low
    node.begin(NETWORK_ID, 16, 6);
}

void loop() {
    uint8_t val = digitalRead(0) == LOW ? 1 : 0;
    node.publish(TOPIC_BUTTON, &val, sizeof(val));
    node.loop();
    delay(100);
}
```

### Relay

```cpp
#include <ESPEZ.h>

#define NETWORK_ID ((uint64_t)0xCAFE << 24)

ESPEZNode node;

void setup() {
    node.begin(NETWORK_ID, 16, 6);
}

void loop() {
    node.loop(RELAY_ON);
}
```

### Receiver — built-in LED

```cpp
#include <ESPEZ.h>

#define NETWORK_ID   ((uint64_t)0xCAFE << 24)
#define TOPIC_BUTTON (NETWORK_ID | 0x01)

ESPEZNode node;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    node.begin(NETWORK_ID, 16, 6);
    node.onMessage([](uint64_t topic, const uint8_t* payload, size_t len) {
        if (topic == TOPIC_BUTTON && len >= 1) {
            digitalWrite(LED_BUILTIN, payload[0] ? HIGH : LOW);
        }
    });
}

void loop() {
    node.loop();
}
```

---

## How It Works

ESPEZ hijacks the sender's MAC address field to carry the topic (5 bytes) and sequence number (1 byte). Every broadcast frame contains exactly 6 bytes of overhead in a field that ESP-NOW already transmits.

Each relay node maintains a hash table of recently seen messages. Before a message is relayed, its hash is compared to this table. If a hash table match is found, the message is suppressed and not relayed — preventing duplicate messages from cascading through the mesh. Different nodes use different hash salts, so a hash table match on one node rarely causes a match on another.

Key behaviors:
- Local delivery always occurs if the network ID and checksum are valid — the hash table never blocks local reception
- Relaying only occurs if the node has relaying enabled and no hash table match is detected
- Different nodes use different hash salts, so two messages that generate the same value on one relay don't on the others

---

## The Publish Function

Combines the network ID, the topic ID, and the sequence number into the MAC address field. Computes the checksum and sends the message via ESP-NOW.

## The Receive Function

Triggered by `on_espnow_receive`. Checks the network ID and checksum — if invalid, sends a help message. Otherwise passes the message to the relay function and invokes the `on_espez_receive` callback. Also checks the sequence number and generates a help message if any packets were dropped. This check cannot be done by the relay function because a message may take different paths depending on hash salts in relays.

All nodes receive all topics. There is no subscribe function — the receiver has to handle incoming data anyway, so filtering it twice serves no purpose.

## The Relay Function

Receives the message from the receive function. Computes the hash and checks whether it matches a recent message. If it does, the message is dropped and the duplicate counter is incremented. Otherwise the MAC is reconstructed and the message is retransmitted via ESP-NOW.

## Help Messages

Help messages use topic `0x0000000000`. `payload[0]` is always the message type.

| Constant | `payload[0]` | `payload[1]` | Description |
|---|---|---|---|
| `ESPEZ_HELP_NODEID` | `0x01` | — | Node joined the mesh. `payload[1..6]` = hardware MAC. |
| `ESPEZ_HELP_FOREIGN` | `0x02` | — | Packet received whose network ID doesn't match this node's. |
| `ESPEZ_HELP_BADCHECK` | `0x03` | — | Packet received with an invalid CRC. |
| `ESPEZ_HELP_SEQSKIP` | `0x04` | gap | Sequence gap detected on a topic. Value = number of missing packets. |
| `ESPEZ_HELP_DUPS` | `0x05` | 255 | Duplicate count exceeded 255 in the last hash table clear interval. This means more than 255 messages were suppressed in one second — the hash table may be too small for the traffic level. Increase `ESPEZ_HASH_SIZE` or decrease `ESPEZ_CLEAR_INTERVAL` to address this. |

Help messages can be used to:
- Filter by minimum RSSI
- Change network channels
- Enable or disable specific relays
- Guide node or antenna placement

---

## API Reference

### Creating a Node

```cpp
ESPEZNode node();
```

### Initialization

```cpp
node.begin(uint64_t networkID = 0, uint8_t networkBits = 0, uint8_t channel = 0, bool longRange = false);
```

Call once in `setup()`. Initializes ESP-NOW, configures the broadcast peer, and prepares the hash table. Pass your network ID and the number of high-order bits it occupies to enable network ID filtering — packets whose high-order bits don't match are rejected and trigger a foreign-network help message. All nodes in a mesh must be on the same WiFi channel; pass `channel` (1–13) to set it explicitly, or leave it at 0 to use the current channel.

Pass `LR_ON` (or `true`) for `longRange` to enable ESP-NOW Long Range mode. This uses Espressif's proprietary LR PHY, which improves range and wall penetration at the cost of throughput. **All nodes in the mesh must use the same setting** — a node in LR mode cannot communicate with a node in standard mode. LR mode is ESP32 only; the parameter is ignored on ESP8266.

`begin()` immediately broadcasts a `ESPEZ_HELP_NODEID` help message containing the node's hardware MAC address. Any node subscribed to `ESPEZ_TOPIC_HELP` will receive this and can use it to detect when a new node joins the mesh.

### Publishing

```cpp
node.publish(uint64_t topic, const uint8_t* payload, size_t len);
```

Network and topic combined are 40-bit values (0 to 2^40−1). Payload is limited to ~248 bytes. The checksum is 2 bytes. The sequence number is managed automatically and rolls over from 255 to 0.

### Subscribing

There is no subscribe function. All nodes receive all topics. Filter by topic inside the `onMessage` callback.

### Receiving Messages

```cpp
node.onMessage(void (*callback)(uint64_t topic, const uint8_t* payload, size_t len));
```

Registers a callback invoked whenever a message arrives.

### Event Loop

```cpp
node.loop(relay_enable);
```

Must be called frequently from `loop()`. Handles incoming messages, hash table match detection, and hash table maintenance. Enables or disables relaying. The constants `RELAY_ON` and `RELAY_OFF` are provided for readability; they are equivalent to `true` and `false`. See also `LR_ON` / `LR_OFF` for the `begin()` long range parameter.

### Diagnostics

Subscribe to topic `0x0000000000` to receive health events from all nodes in the mesh.

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

Larger hash tables reduce false hash table matches. Shorter clear intervals make the mesh more responsive to new messages.

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

ESPEZ uses broadcast addressing. Two ESPEZ meshes within radio range will see each other's traffic. Always use a network ID.

The network ID occupies the high-order bits of the 40-bit topic field; the topic ID occupies the low-order bits. When a packet arrives, the node compares the high-order bits against its own network ID and rejects any packet that doesn't match. A larger network ID has more possible values, so a foreign network is less likely to share it — detection is more reliable. A smaller network ID leaves more room for topics but increases the chance of a collision with a foreign network's ID.

```cpp
// 16-bit network ID — 65536 possible networks, 24-bit topic space (16M topics)
#define NETWORK_ID       ((uint64_t)0xCAFE << 24)
#define TOPIC_TEMP        NETWORK_ID | 0x000001
#define TOPIC_FIRE        NETWORK_ID | 0x000002

// 1-bit network ID — minimal isolation, 39-bit topic space (512B topics)
#define NETWORK_ID       ((uint64_t)1 << 39)
#define TOPIC_TEMP        NETWORK_ID | 0x0000000001
#define TOPIC_FIRE        NETWORK_ID | 0x0000000002
```

### Channel Selection

ESP-NOW operates on 2.4 GHz only and all nodes must be on the same channel. Channels 1, 6, and 11 are the non-overlapping choices in the 2.4 GHz band — pick one of these to minimise interference from surrounding WiFi networks.

**ESPEZ without WiFi** — pass your chosen channel to `begin()` and use it on every node:

```cpp
node.begin(NETWORK_ID, 16, 6);   // all nodes on channel 6
```

**ESPEZ with WiFi STA** — when a STA connects to an AP, the radio silently shifts to the AP's channel, regardless of what was passed to `begin()`. All ESPEZ nodes must therefore connect to APs on the same channel, or the mesh will fragment. Do not pass a channel to `begin()` in this case; let the AP's channel take effect:

```cpp
WiFi.begin(ssid, password);      // AP determines the channel
node.begin(NETWORK_ID, 16, 0);   // channel 0: inherits AP's channel
```

**ESPEZ with WiFi AP** — configure the AP channel before calling `begin()`, then inherit it:

```cpp
WiFi.softAP(ssid, password, 6);  // fix AP to channel 6
node.begin(NETWORK_ID, 16, 0);   // channel 0: inherits channel 6
```

### Diagnostics Topic

ESPEZ reserves topic `0x0000000000` for network diagnostics. Subscribe to it to receive health information from all nodes in the mesh.

```cpp
node.onMessage([](uint64_t topic, const uint8_t* payload, size_t len) {
    if (topic == ESPEZ_TOPIC_HELP) onDiagnostic(payload, len);
});
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
- Always use a network ID
- Subscribe to the diagnostics topic
- Enable relays only when needed

---

*The protocol was originally developed for a twin-screw yacht with two helm stations, a generator room, and a wastewater treatment plant — a challenging environment with steel bulkheads and long cable runs.*

# ESPEZ

A lightweight publish/subscribe mesh network for ESP32 and ESP8266 devices. No pairing. No routing tables. No infrastructure. Works on all ESPs that support ESPNow.

Any node can publish to a topic. All nodes are subscribed to all topics. The mesh handles the rest.

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

Help messages use topic `0x0000000000`. The payload indicates the event type:

- Node ID broadcast
- Foreign network detected
- Skipped sequence number
- Bad checksum
- Duplicate count since last hash table clear

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
node.begin();
```

Call once in `setup()`. Initializes ESP-NOW, configures the broadcast peer, and prepares the hash table.

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

Must be called frequently from `loop()`. Handles incoming messages, hash table match detection, and hash table maintenance. Enables or disables relaying.

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

The network ID and topic share 40 bits. How you split them depends on how many topics you need:

```cpp
// Few topics — use a large network ID for strong isolation
#define NETWORK_ID 0xCAFEBABE00  // 32-bit network ID, 8 bits for topics (256 topics)
#define TOPIC_TEMP  NETWORK_ID | 0x01
#define TOPIC_FIRE  NETWORK_ID | 0x02

// Many topics — use a 1-bit network ID to maximize topic space
#define NETWORK_ID  ((uint64_t)1 << 39)           // 1-bit network ID
#define TOPIC_TEMP  NETWORK_ID | 0x0000000001
#define TOPIC_FIRE  NETWORK_ID | 0x0000000002
```

Use a larger network ID when isolation matters more than topic count. Use a smaller one when you need a large topic space.

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
- Always use a network ID
- Subscribe to the diagnostics topic
- Enable relays only when needed

---

*The protocol was originally developed for a twin-screw yacht with two helm stations, a generator room, and a wastewater treatment plant — a challenging environment with steel bulkheads and long cable runs.*

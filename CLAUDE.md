# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESPEZ is an Arduino library for ESP32 and ESP8266 that implements a lightweight publish/subscribe mesh network over ESP-NOW (Wi-Fi without an access point). It was built for a twin-screw yacht with multiple helm stations. No pairing, routing tables, or infrastructure — pure broadcast.

## Building and testing

This is an Arduino library. There is no build system, test suite, or linter. To compile:

1. Copy the repository to `~/Arduino/libraries/ESPEZ/` (or use the Arduino IDE library manager)
2. Include in a sketch with `#include <ESPEZ.h>`
3. Compile via the Arduino IDE or `arduino-cli compile --fqbn esp32:esp32:<board> <sketch>`

Changes are verified by flashing to hardware and observing behaviour on the `ESPEZ_TOPIC_HELP` diagnostic channel (topic `0x0000000000`).

## Architecture

The entire library is two files: `src/ESPEZ.h` and `src/ESPEZ.cpp`, exposing a single class `ESPEZNode`.

### Encoding trick — the MAC field as a carrier

ESP-NOW sends a 6-byte sender MAC with every frame. ESPEZ exploits this by temporarily spoofing the device MAC before each `publish()` call, packing the 5-byte topic ID and a 1-byte sequence number into those 6 bytes. This encodes routing metadata with zero payload overhead. The real MAC is restored immediately after sending.

### Data flow

```
publish()  →  spoof MAC (topic + seq)  →  ESP-NOW broadcast
receive()  →  CRC check  →  network ID filter  →  seq gap check
           →  local callback  →  optional relay (duplicate-filtered)
```

### Duplicate suppression

Each node derives a per-node salt from its hardware MAC at `begin()`. Incoming messages are hashed with FNV-1a XOR'd with that salt and stored in a fixed-size bit array (`ESPEZ_HASH_SIZE`, default 256 bits). The table clears on a timer (`ESPEZ_CLEAR_INTERVAL`, default 1 s). Different salts per node reduce false duplicate matches across the mesh. Local delivery is never blocked by the hash table — only relaying is gated.

### Help / diagnostics channel

Topic `ESPEZ_TOPIC_HELP` (`0x0000000000`) carries diagnostic payloads. Help messages are enqueued inside the ESP-NOW ISR callback and drained in `loop()` because ESP-NOW transmission is not ISR-safe. Message types: `NODEID`, `FOREIGN`, `BADCHECK`, `SEQSKIP`, `DUPS`.

### Dual ESP-IDF support

`_handleReceive()` has two implementations guarded by `#if ESP_IDF_VERSION_MAJOR >= 5`: the v5 callback signature changed its parameter types. Both code paths must be kept in sync when modifying receive logic.

## Key constraints and limits

- Maximum payload: 248 bytes (`ESPEZ_MAX_PAYLOAD`)
- Sequence tracking: 8 concurrent topics per node (`_seqNums` array, hardcoded)
- Mesh saturation: ~500 relay nodes before ESP-NOW airtime fills up
- MAC spoofing requires the Wi-Fi interface to be in station mode before `begin()` is called
- Topics must be chosen from the same network ID byte (upper byte of the 5-byte topic); messages with a mismatched network ID are silently dropped and reported on the help channel as `FOREIGN`

#pragma once
#include <Arduino.h>

#ifndef ESPEZ_HASH_SIZE
#define ESPEZ_HASH_SIZE 256         // Bits in duplicate filter
#endif

#ifndef ESPEZ_CLEAR_INTERVAL
#define ESPEZ_CLEAR_INTERVAL 1      // Seconds between hash table clears
#endif

#define ESPEZ_TOPIC_HELP 0x0000000000ULL
#define ESPEZ_MAX_PAYLOAD 248

#define RELAY_ON  true
#define RELAY_OFF false

// Help message types (payload byte 0)
#define ESPEZ_HELP_NODEID   0x01    // Node ID broadcast
#define ESPEZ_HELP_FOREIGN  0x02    // Foreign network / bad checksum detected
#define ESPEZ_HELP_SEQSKIP  0x03    // Sequence gap detected (payload byte 1 = gap size)
#define ESPEZ_HELP_DUPS     0x04    // Duplicates since last clear (payload byte 1 = count)

typedef void (*ESPEZCallback)(uint64_t topic, const uint8_t *payload, size_t len);

class ESPEZNode {
public:
    ESPEZNode();
    void begin();
    void publish(uint64_t topic, const uint8_t *payload, size_t len);
    void onMessage(ESPEZCallback callback);
    void loop(bool relay_enable = false);

private:
    static ESPEZNode *_instance;

    uint8_t  _mac[6];                          // Original hardware MAC
    uint8_t  _hashTable[ESPEZ_HASH_SIZE / 8];  // Duplicate filter bitmap
    uint32_t _hashSalt;                        // Per-node salt derived from MAC
    uint8_t  _seqNum;                          // Outgoing sequence counter
    uint8_t  _lastSeq;                         // Last incoming seq (best-effort)
    bool     _seqInit;
    unsigned long _lastClear;
    uint32_t _dupCount;
    bool     _relaying;
    ESPEZCallback _callback;

    void     _handleReceive(const uint8_t *srcMac, const uint8_t *data, int len);
    void     _relay(const uint8_t *srcMac, const uint8_t *data, int len);
    bool     _hashCheck(const uint8_t *srcMac, const uint8_t *data, int len);
    void     _hashSet(const uint8_t *srcMac, const uint8_t *data, int len);
    uint32_t _hash(const uint8_t *srcMac, const uint8_t *data, int len);
    uint16_t _checksum(const uint8_t *srcMac, const uint8_t *data, int len);
    void     _setMac(const uint8_t *mac);
    void     _restoreMac();
    void     _sendHelp(uint8_t type, uint8_t value);

    static void _onReceive(const uint8_t *mac, const uint8_t *data, int len);
};

#pragma once
#include <Arduino.h>

// Detect ESP-IDF v5+ (Arduino ESP32 board package >=3.0.0)
#ifdef ESP32
#  include <esp_idf_version.h>
#  if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#    define ESPEZ_IDF5 1
#    include <esp_now.h>    // needed for esp_now_recv_info_t in declaration below
#  endif
#endif

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
#define ESPEZ_HELP_NODEID   0x01    // Node ID broadcast (bytes 1-6 = hardware MAC)
#define ESPEZ_HELP_FOREIGN  0x02    // Foreign network ID detected
#define ESPEZ_HELP_BADCHECK 0x03    // Bad checksum
#define ESPEZ_HELP_SEQSKIP  0x04    // Sequence gap detected (payload byte 1 = gap size)
#define ESPEZ_HELP_DUPS     0x05    // Duplicates since last clear (payload byte 1 = count)

typedef void (*ESPEZCallback)(uint64_t topic, const uint8_t *payload, size_t len);

class ESPEZNode {
public:
    ESPEZNode();
    void begin(uint64_t networkID = 0, uint8_t networkBits = 0, uint8_t channel = 0);
    void publish(uint64_t topic, const uint8_t *payload, size_t len);
    void onMessage(ESPEZCallback callback);
    void loop(bool relay_enable = false);

private:
    static ESPEZNode *_instance;

    uint8_t  _mac[6];                          // Original hardware MAC
    uint8_t  _hashTable[ESPEZ_HASH_SIZE / 8];  // Duplicate filter bitmap
    uint32_t _hashSalt;                        // Per-node salt derived from MAC
    uint64_t _networkID;                       // Expected high-order bits
    uint64_t _networkMask;                     // Mask for network ID bits
    uint8_t  _seqNum;                          // Outgoing sequence counter
    unsigned long _lastClear;
    uint32_t _dupCount;
    volatile bool _relaying;
    ESPEZCallback _callback;

    // Per-topic sequence tracking (8 most-recently-seen topics, round-robin eviction)
    struct _SeqEntry { uint8_t topic[5]; uint8_t seq; bool valid; };
    static const uint8_t _SEQ_TRACKS = 8;
    _SeqEntry _seqTable[_SEQ_TRACKS];
    uint8_t   _seqEvict;

    // Help messages are queued in the receive callback and sent from loop()
    struct _HelpMsg { uint8_t type; uint8_t value; };
    static const uint8_t _HELP_QUEUE_SIZE = 8;
    _HelpMsg         _helpQueue[_HELP_QUEUE_SIZE];
    volatile uint8_t _helpHead;
    volatile uint8_t _helpTail;

    void     _handleReceive(const uint8_t *srcMac, const uint8_t *data, int len);
    void     _relay(const uint8_t *srcMac, const uint8_t *data, int len);
    void     _seqCheck(const uint8_t *topicBytes, uint8_t seq);
    bool     _hashCheck(const uint8_t *srcMac, const uint8_t *data, int len);
    void     _hashSet(const uint8_t *srcMac, const uint8_t *data, int len);
    uint32_t _hash(const uint8_t *srcMac, const uint8_t *data, int len);
    uint16_t _checksum(const uint8_t *srcMac, const uint8_t *data, int len);
    void     _setMac(const uint8_t *mac);
    void     _restoreMac();
    void     _sendHelp(uint8_t type, uint8_t value);

#ifdef ESPEZ_IDF5
    static void _onReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
#else
    static void _onReceive(const uint8_t *mac, const uint8_t *data, int len);
#endif
};

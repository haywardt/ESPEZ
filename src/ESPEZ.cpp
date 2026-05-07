#include "ESPEZ.h"

#ifdef ESP32
#  include <WiFi.h>
#  include <esp_now.h>
#  include <esp_wifi.h>
#else
#  include <ESP8266WiFi.h>
extern "C" {
#  include <espnow.h>
#  include <user_interface.h>
}
#endif

static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

ESPEZNode *ESPEZNode::_instance = nullptr;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ESPEZNode::ESPEZNode()
    : _hashSalt(0), _networkID(0), _networkMask(0),
      _seqNum(0), _seqEvict(0),
      _lastClear(0), _dupCount(0), _relaying(false), _callback(nullptr),
      _helpHead(0), _helpTail(0) {
    memset(_mac,       0, sizeof(_mac));
    memset(_hashTable, 0, sizeof(_hashTable));
    memset(_seqTable,  0, sizeof(_seqTable));
    memset(_helpQueue, 0, sizeof(_helpQueue));
}

void ESPEZNode::begin(uint64_t networkID, uint8_t networkBits, uint8_t channel) {
    _instance = this;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

#ifdef ESP32
    if (channel > 0) esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_get_mac(WIFI_IF_STA, _mac);
#else
    wifi_get_macaddr(STATION_IF, _mac);
#endif

    // Each node gets a unique salt from its hardware MAC so duplicate-filter
    // hashes land on different bits across devices.
    _hashSalt = ((uint32_t)_mac[2] << 24) | ((uint32_t)_mac[3] << 16) |
                ((uint32_t)_mac[4] <<  8) |  _mac[5];

    if (networkBits > 0 && networkBits <= 40) {
        _networkMask = ((1ULL << networkBits) - 1) << (40 - networkBits);
        _networkID   = networkID & _networkMask;
    }

#ifdef ESP32
    esp_now_init();
    {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, BROADCAST, 6);
        peer.channel = channel;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
    esp_now_register_recv_cb(_onReceive);
#else
    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_add_peer(const_cast<uint8_t *>(BROADCAST),
                     ESP_NOW_ROLE_COMBO, channel, nullptr, 0);
    esp_now_register_recv_cb(reinterpret_cast<esp_now_recv_cb_t>(_onReceive));
#endif

    // Broadcast node ID: [ESPEZ_HELP_NODEID, mac[0..5]]
    uint8_t id[7] = {ESPEZ_HELP_NODEID,
                     _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]};
    publish(ESPEZ_TOPIC_HELP, id, 7);

    _lastClear = millis();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ESPEZNode::publish(uint64_t topic, const uint8_t *payload, size_t len) {
    if (len > ESPEZ_MAX_PAYLOAD) len = ESPEZ_MAX_PAYLOAD;

    // Pack topic (5 bytes) + sequence (1 byte) into source MAC field.
    uint8_t srcMac[6] = {
        static_cast<uint8_t>((topic >> 32) & 0xFF),
        static_cast<uint8_t>((topic >> 24) & 0xFF),
        static_cast<uint8_t>((topic >> 16) & 0xFF),
        static_cast<uint8_t>((topic >>  8) & 0xFF),
        static_cast<uint8_t>( topic        & 0xFF),
        _seqNum++
    };

    // Packet layout: [crc_hi, crc_lo, user_payload...]
    uint8_t pkt[2 + ESPEZ_MAX_PAYLOAD];
    uint16_t crc = _checksum(srcMac, payload, len);
    pkt[0] = crc >> 8;
    pkt[1] = crc & 0xFF;
    memcpy(pkt + 2, payload, len);

    _setMac(srcMac);
#ifdef ESP32
    esp_now_send(BROADCAST, pkt, len + 2);
#else
    esp_now_send(const_cast<uint8_t *>(BROADCAST), pkt, len + 2);
#endif
    _restoreMac();
}

void ESPEZNode::onMessage(ESPEZCallback callback) {
    _callback = callback;
}

void ESPEZNode::loop(bool relay_enable) {
    _relaying = relay_enable;

    unsigned long now = millis();
    if (now - _lastClear >= static_cast<unsigned long>(ESPEZ_CLEAR_INTERVAL * 1000)) {
        if (_dupCount > 255) {
            _sendHelp(ESPEZ_HELP_DUPS, 255);
        }
        _dupCount = 0;
        memset(_hashTable, 0, sizeof(_hashTable));
        _lastClear = now;
    }

    // Drain help messages queued from callback context (and from above)
    while (_helpTail != _helpHead) {
        _HelpMsg m = _helpQueue[_helpTail];
        _helpTail = (_helpTail + 1) % _HELP_QUEUE_SIZE;
        uint8_t payload[2] = {m.type, m.value};
        publish(ESPEZ_TOPIC_HELP, payload, 2);
    }
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

#ifdef ESPEZ_IDF5
void ESPEZNode::_onReceive(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len) {
    if (_instance) _instance->_handleReceive(recv_info->src_addr, data, len);
}
#else
void ESPEZNode::_onReceive(const uint8_t *mac, const uint8_t *data, int len) {
    if (_instance) _instance->_handleReceive(mac, data, len);
}
#endif

void ESPEZNode::_handleReceive(const uint8_t *srcMac,
                               const uint8_t *data, int len) {
    if (len < 2) return;

    // Validate: CRC covers srcMac + user payload (data[2..])
    uint16_t rxCrc   = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint16_t calcCrc = _checksum(srcMac, data + 2, len - 2);
    if (rxCrc != calcCrc) {
        _sendHelp(ESPEZ_HELP_BADCHECK, 0);
        return;
    }

    uint64_t topic = (static_cast<uint64_t>(srcMac[0]) << 32) |
                     (static_cast<uint64_t>(srcMac[1]) << 24) |
                     (static_cast<uint64_t>(srcMac[2]) << 16) |
                     (static_cast<uint64_t>(srcMac[3]) <<  8) |
                      srcMac[4];
    uint8_t seq = srcMac[5];

    // Reject packets whose network ID doesn't match ours (help topic is always accepted)
    if (topic != ESPEZ_TOPIC_HELP && _networkMask != 0 && (topic & _networkMask) != _networkID) {
        _sendHelp(ESPEZ_HELP_FOREIGN, 0);
        return;
    }

    // Per-topic sequence gap detection
    if (topic != ESPEZ_TOPIC_HELP)
        _seqCheck(srcMac, seq);

    // Local delivery always happens — hash table never blocks reception
    if (_callback)
        _callback(topic, data + 2, len - 2);

    // Relay only if enabled and not a duplicate
    if (_relaying) {
        if (_hashCheck(srcMac, data, len)) {
            _dupCount++;
        } else {
            _hashSet(srcMac, data, len);
            _relay(srcMac, data, len);
        }
    }
}

void ESPEZNode::_relay(const uint8_t *srcMac, const uint8_t *data, int len) {
    _setMac(srcMac);
#ifdef ESP32
    esp_now_send(BROADCAST, data, len);
#else
    esp_now_send(const_cast<uint8_t *>(BROADCAST),
                 const_cast<uint8_t *>(data), len);
#endif
    _restoreMac();
}

// ---------------------------------------------------------------------------
// Per-topic sequence tracking
// ---------------------------------------------------------------------------

void ESPEZNode::_seqCheck(const uint8_t *topicBytes, uint8_t seq) {
    for (uint8_t i = 0; i < _SEQ_TRACKS; i++) {
        if (_seqTable[i].valid &&
            memcmp(_seqTable[i].topic, topicBytes, 5) == 0) {
            uint8_t gap = seq - _seqTable[i].seq - 1;
            if (gap > 0) _sendHelp(ESPEZ_HELP_SEQSKIP, gap);
            _seqTable[i].seq = seq;
            return;
        }
    }
    // First packet from this topic — find an empty slot or evict round-robin
    uint8_t slot = _seqEvict;
    bool foundEmpty = false;
    for (uint8_t i = 0; i < _SEQ_TRACKS; i++) {
        if (!_seqTable[i].valid) { slot = i; foundEmpty = true; break; }
    }
    if (!foundEmpty) _seqEvict = (_seqEvict + 1) % _SEQ_TRACKS;
    _seqTable[slot].valid = true;
    memcpy(_seqTable[slot].topic, topicBytes, 5);
    _seqTable[slot].seq = seq;
}

// ---------------------------------------------------------------------------
// Hash / duplicate filter
// ---------------------------------------------------------------------------

uint32_t ESPEZNode::_hash(const uint8_t *srcMac,
                          const uint8_t *data, int len) {
    // FNV-1a over srcMac + first 4 payload bytes, XOR'd with per-node salt
    uint32_t h = 2166136261UL ^ _hashSalt;
    for (int i = 0; i < 6; i++) { h ^= srcMac[i]; h *= 16777619UL; }
    int n = len < 4 ? len : 4;
    for (int i = 0; i < n; i++) { h ^= data[i];   h *= 16777619UL; }
    return h % ESPEZ_HASH_SIZE;
}

bool ESPEZNode::_hashCheck(const uint8_t *srcMac,
                           const uint8_t *data, int len) {
    uint32_t idx = _hash(srcMac, data, len);
    return (_hashTable[idx / 8] >> (idx % 8)) & 1;
}

void ESPEZNode::_hashSet(const uint8_t *srcMac,
                         const uint8_t *data, int len) {
    uint32_t idx = _hash(srcMac, data, len);
    _hashTable[idx / 8] |= 1u << (idx % 8);
}

// ---------------------------------------------------------------------------
// Checksum (CRC-16/CCITT)
// ---------------------------------------------------------------------------

uint16_t ESPEZNode::_checksum(const uint8_t *srcMac,
                              const uint8_t *data, int len) {
    auto step = [](uint16_t crc, uint8_t b) -> uint16_t {
        crc ^= static_cast<uint16_t>(b) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        return crc;
    };
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < 6; i++) crc = step(crc, srcMac[i]);
    for (int i = 0; i < len; i++) crc = step(crc, data[i]);
    return crc;
}

// ---------------------------------------------------------------------------
// MAC management
// ---------------------------------------------------------------------------

void ESPEZNode::_setMac(const uint8_t *mac) {
#ifdef ESP32
    esp_wifi_set_mac(WIFI_IF_STA, const_cast<uint8_t *>(mac));
#else
    wifi_set_macaddr(STATION_IF, const_cast<uint8_t *>(mac));
#endif
}

void ESPEZNode::_restoreMac() {
    _setMac(_mac);
}

// ---------------------------------------------------------------------------
// Help messages — enqueued here, published from loop()
// ---------------------------------------------------------------------------

void ESPEZNode::_sendHelp(uint8_t type, uint8_t value) {
    uint8_t next = (_helpHead + 1) % _HELP_QUEUE_SIZE;
    if (next != _helpTail) {
        _helpQueue[_helpHead] = {type, value};
        _helpHead = next;
    }
}

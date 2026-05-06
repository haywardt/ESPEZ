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
      _seqNum(0), _lastSeq(0), _seqInit(false),
      _lastClear(0), _dupCount(0), _relaying(false), _callback(nullptr) {
    memset(_mac,       0, sizeof(_mac));
    memset(_hashTable, 0, sizeof(_hashTable));
}

void ESPEZNode::begin(uint64_t networkID, uint8_t networkBits) {
    _instance = this;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

#ifdef ESP32
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
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
    esp_now_register_recv_cb(_onReceive);
#else
    esp_now_init();
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_add_peer(const_cast<uint8_t *>(BROADCAST),
                     ESP_NOW_ROLE_COMBO, 1, nullptr, 0);
    esp_now_register_recv_cb(reinterpret_cast<esp_now_recv_cb_t>(_onReceive));
#endif

    // Broadcast our node ID so the mesh knows we exist
    uint8_t id[6];
    memcpy(id, _mac, 6);
    publish(ESPEZ_TOPIC_HELP, id, 6);

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
        if (_dupCount > 0) {
            _sendHelp(ESPEZ_HELP_DUPS,
                      _dupCount > 255 ? 255 : static_cast<uint8_t>(_dupCount));
            _dupCount = 0;
        }
        memset(_hashTable, 0, sizeof(_hashTable));
        _lastClear = now;
    }
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

void ESPEZNode::_onReceive(const uint8_t *mac, const uint8_t *data, int len) {
    if (_instance) _instance->_handleReceive(mac, data, len);
}

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

    // Reject packets whose network ID doesn't match ours
    if (_networkMask != 0 && (topic & _networkMask) != _networkID) {
        _sendHelp(ESPEZ_HELP_FOREIGN, 0);
        return;
    }

    // Best-effort sequence gap detection (single stream; not per-sender)
    if (topic != ESPEZ_TOPIC_HELP && _seqInit) {
        uint8_t gap = static_cast<uint8_t>(seq - _lastSeq - 1);
        if (gap > 0)
            _sendHelp(ESPEZ_HELP_SEQSKIP, gap);
    }
    _lastSeq  = seq;
    _seqInit  = true;

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
// Help messages
// ---------------------------------------------------------------------------

void ESPEZNode::_sendHelp(uint8_t type, uint8_t value) {
    uint8_t payload[2] = {type, value};
    publish(ESPEZ_TOPIC_HELP, payload, 2);
}

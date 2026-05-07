#include <ESPEZ.h>

#define NETWORK_ID ((uint64_t)0xCAFE << 24)

ESPEZNode node;

void setup() {
    Serial.begin(115200);

    node.begin(NETWORK_ID, 16, 6);
    node.onMessage([](uint64_t topic, const uint8_t* payload, size_t len) {
        if (topic != ESPEZ_TOPIC_HELP || len < 1) return;
        switch (payload[0]) {
            case ESPEZ_HELP_NODEID:
                Serial.printf("NODE  %02x:%02x:%02x:%02x:%02x:%02x\n",
                    payload[1], payload[2], payload[3],
                    payload[4], payload[5], payload[6]);
                break;
            case ESPEZ_HELP_FOREIGN:  Serial.println("FOREIGN packet");          break;
            case ESPEZ_HELP_BADCHECK: Serial.println("BAD checksum");             break;
            case ESPEZ_HELP_SEQSKIP:  Serial.printf("SEQSKIP gap=%d\n", payload[1]); break;
            case ESPEZ_HELP_DUPS:     Serial.printf("DUPS suppressed=%d\n", payload[1]); break;
        }
    });
}

void loop() {
    node.loop(RELAY_ON);
}

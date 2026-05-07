#include <ESPEZ.h>
#include <Adafruit_NeoPixel.h>

#define NETWORK_ID   ((uint64_t)0xCAFE << 24)
#define TOPIC_BUTTON (NETWORK_ID | 0x01)

#define LED_PIN   10
#define LED_COUNT 1

ESPEZNode node;
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);

static bool buttonOn = false;
static bool helpReceived = false;
static unsigned long helpAt = 0;

void setup() {
    led.begin();
    led.clear();
    led.show();

    node.begin(NETWORK_ID, 16, 6);
    node.onMessage([](uint64_t topic, const uint8_t* payload, size_t len) {
        if (topic == ESPEZ_TOPIC_HELP) {
            helpAt = millis();
            helpReceived = true;
        } else if (topic == TOPIC_BUTTON && len >= 1) {
            buttonOn = payload[0] != 0;
        }
    });
}

void loop() {
    node.loop();

    if (helpReceived && millis() - helpAt < 500) {
        led.setPixelColor(0, led.Color(32, 0, 0));   // red: help event
    } else if (buttonOn) {
        led.setPixelColor(0, led.Color(0, 32, 0));   // green: button held
    } else {
        led.clear();
    }
    led.show();
}

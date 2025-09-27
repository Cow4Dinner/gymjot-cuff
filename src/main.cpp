#include <Arduino.h>
#include <NimBLEDevice.h>
#include "Config.h"

static bool testMode = TEST_MODE_DEFAULT;
static int repCount = 0;
static unsigned long lastTagSeen = 0;

NimBLEServer* pServer;
NimBLECharacteristic* pTx;

void sendJson(const String& msg) {
    if (pTx->getSubscribedCount() > 0) {
        pTx->setValue(msg);
        pTx->notify();
    }
    Serial.println("-> " + msg);
}

void processCommand(const String& cmd) {
    if (cmd.indexOf("test") >= 0) {
        testMode = cmd.indexOf("true") >= 0;
        sendJson("{\"type\":\"status\",\"status\":\"testMode\"}");
    }
}

class RxCallback: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) override {
        std::string val = pCharacteristic->getValue();
        if (!val.empty()) processCommand(String(val.c_str()));
    }
};

void setupBLE() {
    NimBLEDevice::init("ESP32CamStation");
    pServer = NimBLEDevice::createServer();
    NimBLEService* service = pServer->createService(SERVICE_UUID);

    NimBLECharacteristic* pRx = service->createCharacteristic(
        CHAR_RX_UUID, NIMBLE_PROPERTY::WRITE
    );
    pTx = service->createCharacteristic(
        CHAR_TX_UUID, NIMBLE_PROPERTY::NOTIFY
    );
    pRx->setCallbacks(new RxCallback());

    service->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    NimBLEDevice::startAdvertising();
}

void setup() {
    Serial.begin(115200);
    setupBLE();
    sendJson("{\"type\":\"status\",\"status\":\"boot\"}");
}

void loop() {
    if (testMode) {
        static unsigned long last = 0;
        if (millis() - last > 1000) {
            last = millis();
            static bool goingUp = true;
            static float pos = 0;
            pos += goingUp ? 10 : -10;
            if (pos >= 100) { goingUp = false; }
            if (pos <= 0) {
                goingUp = true;
                repCount++;
                sendJson("{\"type\":\"rep\",\"count\":" + String(repCount) + "}");
            }
            sendJson("{\"type\":\"scan\",\"tagId\":16,\"distanceCm\":" + String(pos) + "}");
        }
    }
    delay(10);
}

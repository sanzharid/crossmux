#include "S3xyLink.h"

#if S3XY_BUTTON

#include <HalPowerManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <atomic>
#include <cstdio>

namespace s3xy {

namespace {

constexpr char kDeviceName[] = "ENH_BTN";
constexpr char kServiceUuid[] = "00003d46-87d2-479e-7e45-8551415a6de1";
constexpr char kNotifyUuid[] = "00003d50-87d2-479e-7e45-8551415a6de1";
constexpr char kIdUuid[] = "00003d49-87d2-479e-7e45-8551415a6de1";
constexpr uint32_t kGapMs = 5;  // spacing between notifications of one press

NimBLECharacteristic* notifyChar = nullptr;
std::atomic<bool> running{false};
std::atomic<bool> connected{false};
std::atomic<bool> subscribed{false};
std::atomic<uint32_t> events{0};

void notifyBytes(const uint8_t* data, const size_t len) {
  if (!subscribed || !notifyChar) return;
  notifyChar->notify(data, len);
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    connected = true;
    LOG_INF("S3XY", "Commander connected");
    // The Commander starts pairing; ask for it ourselves as well so an
    // already-bonded Commander re-encrypts immediately.
    NimBLEDevice::startSecurity(info.getConnHandle());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    connected = false;
    subscribed = false;
    LOG_INF("S3XY", "Commander disconnected (reason %d)", reason);
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    LOG_INF("S3XY", "Pairing %s (bonded=%d)", info.isEncrypted() ? "ok" : "failed", info.isBonded());
  }
};

class NotifyCallbacks final : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, const uint16_t subValue) override {
    subscribed = (subValue & 0x0001) != 0;
    LOG_INF("S3XY", "Notifications %s", subscribed ? "on" : "off");
  }
};

class IdCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    const NimBLEAttValue value = chr->getValue();
    const uint8_t* data = value.data();
    const size_t len = value.size();
    if (len == 1 && data[0] == 0xB6) {  // Commander init / mode query
      const uint8_t reply[] = {0xC7, 0x00, 0x01};
      notifyBytes(reply, sizeof(reply));
    } else if (len == 1 && data[0] == 0xA1) {  // removed in the S3XY app
      LOG_INF("S3XY", "Unpair request");
      NimBLEDevice::getServer()->disconnect(info);
    } else if (len == 4 && data[0] == 0xA4) {  // rename
      const uint8_t reply[] = {0xA4, 0x00, data[1], data[2]};
      notifyBytes(reply, sizeof(reply));
    }
    events.fetch_add(1);
  }
};

ServerCallbacks serverCallbacks;
NotifyCallbacks notifyCallbacks;
IdCallbacks idCallbacks;

// Stable per-device 10-byte ID ("STICKY" + last 2 MAC bytes as hex).
void buildId(uint8_t (&id)[10]) {
  const uint8_t* mac = NimBLEDevice::getAddress().getVal();  // little-endian
  char text[11];
  snprintf(text, sizeof(text), "STICKY%02X%02X", mac[1], mac[0]);
  memcpy(id, text, sizeof(id));
}

}  // namespace

bool available() { return true; }

bool begin() {
  if (running) return true;
  HalPowerManager::Lock powerLock;  // controller init needs full CPU speed
  if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);
  if (!NimBLEDevice::init(kDeviceName)) {
    LOG_ERR("S3XY", "NimBLE init failed");
    return false;
  }
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&serverCallbacks, false);
  server->advertiseOnDisconnect(true);

  NimBLEService* service = server->createService(kServiceUuid);
  notifyChar = service->createCharacteristic(kNotifyUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  const uint8_t idle = 0x00;
  notifyChar->setValue(&idle, 1);
  notifyChar->setCallbacks(&notifyCallbacks);  // static: never deleted

  NimBLECharacteristic* idChar =
      service->createCharacteristic(kIdUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ_ENC |
                                                 NIMBLE_PROPERTY::WRITE_ENC);
  uint8_t id[10];
  buildId(id);
  idChar->setValue(id, sizeof(id));
  idChar->setCallbacks(&idCallbacks);
  service->start();

  NimBLEAdvertisementData adv;
  adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);  // 0x06
  adv.setAppearance(0x0000);
  adv.setName(kDeviceName);
  NimBLEAdvertisementData scan;
  scan.setCompleteServices(NimBLEUUID(kServiceUuid));
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(adv);
  advertising->setScanResponseData(scan);
  advertising->enableScanResponse(true);
  if (!advertising->start()) {
    LOG_ERR("S3XY", "Advertising failed to start");
    NimBLEDevice::deinit(true);
    notifyChar = nullptr;
    return false;
  }
  running = true;
  LOG_INF("S3XY", "Advertising as %s", kDeviceName);
  return true;
}

void end() {
  if (!running) return;
  HalPowerManager::Lock powerLock;
  running = false;
  connected = false;
  subscribed = false;
  notifyChar = nullptr;
  NimBLEDevice::deinit(true);  // bonds stay in NVS for the next session
  LOG_INF("S3XY", "Stopped");
}

LinkState state() {
  if (!running) return LinkState::Off;
  if (!connected) return LinkState::Advertising;
  return subscribed ? LinkState::Ready : LinkState::Connected;
}

bool send(const Press press) {
  if (state() != LinkState::Ready) return false;
  const uint8_t down[] = {0x01};
  const uint8_t up[] = {0x00};
  switch (press) {
    case Press::Single: {
      const uint8_t click[] = {0xC1, 0x01};
      notifyBytes(down, sizeof(down));
      delay(kGapMs);
      notifyBytes(up, sizeof(up));
      delay(kGapMs);
      notifyBytes(click, sizeof(click));
      delay(kGapMs);
      notifyBytes(up, sizeof(up));
      break;
    }
    case Press::Double: {
      const uint8_t click[] = {0xC1, 0x02};
      notifyBytes(click, sizeof(click));
      break;
    }
    case Press::Long: {
      const uint8_t hold[] = {0xC3, 0x01};
      notifyBytes(hold, sizeof(hold));
      break;
    }
  }
  delay(kGapMs);
  return true;
}

uint32_t commanderEvents() { return events.load(); }

}  // namespace s3xy

#else  // !S3XY_BUTTON

namespace s3xy {
bool available() { return false; }
bool begin() { return false; }
void end() {}
LinkState state() { return LinkState::Off; }
bool send(Press) { return false; }
uint32_t commanderEvents() { return 0; }
}  // namespace s3xy

#endif

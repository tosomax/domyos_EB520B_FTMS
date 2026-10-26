#include <Arduino.h>
#include <BLEDevice.h>
#include <vector>
#include <freertos/semphr.h>

BLEClient *client;
std::vector<BLEAdvertisedDevice> devices;
SemaphoreHandle_t outputMutex;
String command;
std::map<std::string, std::vector<uint8_t>> advertisements;
std::map<uint16_t, std::string> handles;
struct Subscription { uint16_t handle; uint16_t cccd; bool notify; };
std::vector<Subscription> subscriptions;
void bytes(const char *, const std::string &, uint16_t, const uint8_t *, size_t);

void gattEvent(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
               esp_ble_gattc_cb_param_t *p) {
  if (event==ESP_GATTC_NOTIFY_EVT) {
    auto it=handles.find(p->notify.handle);
    bytes(p->notify.is_notify ? "NOTIFY" : "INDICATE",
      it==handles.end() ? "unknown" : it->second, p->notify.handle,
      p->notify.value, p->notify.value_len);
  } else if (event==ESP_GATTC_REG_FOR_NOTIFY_EVT) {
    Serial.printf("%lu REGISTER_ACK handle=%u status=%u\n", millis(),
      p->reg_for_notify.handle, p->reg_for_notify.status);
    if (p->reg_for_notify.status!=ESP_GATT_OK) return;
    for (auto &s : subscriptions) if (s.handle==p->reg_for_notify.handle) {
      uint8_t value[]={uint8_t(s.notify ? 1 : 2), 0};
      auto rc=esp_ble_gattc_write_char_descr(interface, client->getConnId(),
        s.cccd, 2, value, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
      Serial.printf("%lu CCCD_REQUEST handle=%u rc=%d\n", millis(), s.cccd, rc);
    }
  } else if (event==ESP_GATTC_WRITE_DESCR_EVT)
    Serial.printf("%lu CCCD_ACK handle=%u status=%u\n", millis(),
      p->write.handle, p->write.status);
}

class ScanEvents : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    // Arduino 2.x retains a borrowed payload pointer; copy during the callback.
    advertisements[d.getAddress().toString()] =
      std::vector<uint8_t>(d.getPayload(), d.getPayload()+d.getPayloadLength());
  }
};

void bytes(const char *event, const std::string &uuid, uint16_t handle,
           const uint8_t *data, size_t n) {
  xSemaphoreTake(outputMutex, portMAX_DELAY);
  Serial.printf("%lu %s uuid=%s handle=%u len=%u hex=", millis(), event,
                uuid.c_str(), handle, unsigned(n));
  for (size_t i=0; i<n; ++i) Serial.printf("%02x", data[i]);
  Serial.print(" ascii=");
  for (size_t i=0; i<n; ++i) Serial.write(data[i]>=32 && data[i]<127 ? data[i] : '.');
  Serial.println();
  xSemaphoreGive(outputMutex);
}

class ClientEvents : public BLEClientCallbacks {
  void onConnect(BLEClient *) override { Serial.printf("%lu CONNECTED\n", millis()); }
  void onDisconnect(BLEClient *) override { Serial.printf("%lu DISCONNECTED\n", millis()); }
};

void scan() {
  if (client->isConnected()) { Serial.println("Disconnect first: disconnect"); return; }
  Serial.printf("%lu SCAN_BEGIN seconds=12\n", millis());
  auto scanner=BLEDevice::getScan();
  scanner->clearResults();
  devices.clear();
  advertisements.clear();
  auto results=scanner->start(12, false);
  for (int i=0; i<results.getCount(); ++i) {
    auto d=results.getDevice(i);
    devices.push_back(d);
    Serial.printf("%lu ADV index=%d address=%s type=%u rssi=%d name=%s\n",
      millis(), i, d.getAddress().toString().c_str(), unsigned(d.getAddressType()),
      d.getRSSI(), d.getName().c_str());
    auto &raw=advertisements[d.getAddress().toString()];
    bytes("ADV_RAW", d.getAddress().toString(), 0, raw.data(), raw.size());
    for (int j=0;j<d.getServiceUUIDCount();++j)
      Serial.printf(" ADV_SERVICE %s\n", d.getServiceUUID(j).toString().c_str());
  }
  Serial.printf("%lu SCAN_END count=%u\n", millis(), unsigned(devices.size()));
}

void inspect() {
  if (!client->isConnected()) { Serial.println("Not connected"); return; }
  handles.clear(); subscriptions.clear();
  auto services=client->getServices();
  for (auto &s : *services) {
    if (!client->isConnected()) break;
    Serial.printf("%lu SERVICE uuid=%s\n", millis(), s.first.c_str());
    // Handle map preserves duplicate characteristic UUIDs.
    auto characteristics=s.second->getCharacteristicsByHandle();
    for (auto &entry : *characteristics) {
      if (!client->isConnected()) break;
      auto c=entry.second;
      handles[c->getHandle()]=c->getUUID().toString();
      Serial.printf("%lu CHAR uuid=%s handle=%u read=%d write=%d write_nr=%d notify=%d indicate=%d\n",
        millis(), c->getUUID().toString().c_str(), c->getHandle(), c->canRead(),
        c->canWrite(), c->canWriteNoResponse(), c->canNotify(), c->canIndicate());
      auto descriptors=c->getDescriptors();
      for (auto &d : *descriptors)
        Serial.printf(" DESCRIPTOR uuid=%s handle=%u\n", d.first.c_str(), d.second->getHandle());
      if (c->canRead()) {
        auto value=c->readValue();
        bytes("READ_RESULT", c->getUUID().toString(), c->getHandle(),
              reinterpret_cast<const uint8_t*>(value.data()), value.size());
      }
      // This SDK permits only 5 registrations. Observe data streams and leave
      // control-point indications unused because no commands are sent.
      if (c->canNotify()) {
        auto cccd=c->getDescriptor(BLEUUID(uint16_t(0x2902)));
        if (cccd) subscriptions.push_back({c->getHandle(), cccd->getHandle(), c->canNotify()});
      }
    }
  }
  for (auto &s : subscriptions) {
    auto address=client->getPeerAddress();
    auto rc=esp_ble_gattc_register_for_notify(client->getGattcIf(), *address.getNative(), s.handle);
    Serial.printf("%lu SUBSCRIBE_REQUEST handle=%u rc=%d\n", millis(), s.handle, rc);
    delay(150);
  }
  Serial.printf("%lu INSPECT_END connected=%d\n", millis(), client->isConnected());
}

void setup() {
  Serial.begin(115200);
  outputMutex=xSemaphoreCreateMutex();
  delay(1000);
  Serial.println("EB520 BLE probe v2; no characteristic writes except CCCD subscriptions");
  BLEDevice::init("EB520-probe");
  BLEDevice::setCustomGattcHandler(gattEvent);
  client=BLEDevice::createClient();
  client->setClientCallbacks(new ClientEvents());
  BLEDevice::getScan()->setActiveScan(true);
  BLEDevice::getScan()->setAdvertisedDeviceCallbacks(new ScanEvents(), false);
  BLEDevice::getScan()->setInterval(160);
  BLEDevice::getScan()->setWindow(120);
  Serial.println("Commands: scan | connect INDEX | disconnect | mark TEXT | help");
  scan();
}

void loop() {
  while (Serial.available()) {
    char ch=Serial.read();
    if (ch=='\r') continue;
    if (ch!='\n') { if (command.length()<200) command+=ch; continue; }
    String line=command; command=""; line.trim();
    if (line=="scan") scan();
    else if (line=="disconnect") client->disconnect();
    else if (line.startsWith("mark ")) Serial.printf("%lu MARK %s\n", millis(), line.substring(5).c_str());
    else if (line.startsWith("connect ")) {
      String indexText=line.substring(8); bool valid=indexText.length()>0;
      for (unsigned i=0;i<indexText.length();++i) valid &= isDigit(indexText[i]);
      int index=indexText.toInt();
      if (!valid || index<0 || size_t(index)>=devices.size()) Serial.println("Invalid index");
      else if (client->isConnected()) Serial.println("Already connected");
      else {
        Serial.printf("%lu CONNECT_BEGIN address=%s\n", millis(), devices[index].getAddress().toString().c_str());
        if (client->connect(&devices[index])) inspect();
        else Serial.printf("%lu CONNECT_FAILED\n", millis());
      }
    } else Serial.println("Commands: scan | connect INDEX | disconnect | mark TEXT | help");
  }
  delay(10);
}

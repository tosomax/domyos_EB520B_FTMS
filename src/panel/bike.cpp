#include "bike.h"
#include "bridge.h"
#include <BLEDevice.h>
#include <freertos/semphr.h>

static BikeData state;
static SemaphoreHandle_t mutex;
static QueueHandle_t requests;
static BLEClient *client;
static BLERemoteCharacteristic *control;
static BLEAdvertisedDevice *target=nullptr;
static uint16_t dataHandle=0, trainingHandle=0, statusHandle=0, controlHandle=0;
static volatile int responseOp=-1, responseCode=-1;
static volatile bool controlSubscribed=false;
static bool ownsControl=false;

static void message(const char *s) {
  xSemaphoreTake(mutex,portMAX_DELAY);
  snprintf(state.status,sizeof(state.status),"%s",s);
  xSemaphoreGive(mutex);
  Serial.printf("%lu STATE %s\n",millis(),s);
}
BikeData bikeSnapshot() {
  xSemaphoreTake(mutex,portMAX_DELAY); auto copy=state; xSemaphoreGive(mutex); return copy;
}
void bikeAdjust(int delta) { if (requests) xQueueSend(requests,&delta,0); }
void bikeBridgeState(bool connected,bool power,bool cadence,uint32_t frames) {
  xSemaphoreTake(mutex,portMAX_DELAY);
  state.watchConnected=connected;state.watchPower=power;state.watchCadence=cadence;state.bridgeFrames=frames;
  xSemaphoreGive(mutex);
}

static void decode(const uint8_t *p,size_t n) {
  if(n!=23 || p[0]!=0x76 || p[1]!=0x0b) {
    Serial.printf("%lu UNEXPECTED_PACKET len=%u\n",millis(),unsigned(n));return;
  }
  auto u16=[p](int i)->int{return p[i]|(p[i+1]<<8);};
  xSemaphoreTake(mutex,portMAX_DELAY);
  state.speed=u16(2)/100.0f;state.averageSpeed=u16(4)/100.0f;
  state.cadence=u16(6)/2.0f;state.distance=p[8]|(p[9]<<8)|(p[10]<<16);
  state.resistance=int16_t(u16(11));state.power=int16_t(u16(13));
  state.kcal=u16(15);state.kcalHour=u16(17);state.kcalMinute=p[19];
  state.bpm=p[20];state.elapsed=u16(21);state.valid=true;state.updated=millis();state.packets++;
  xSemaphoreGive(mutex);
}

static void gattEvent(esp_gattc_cb_event_t e,esp_gatt_if_t g,esp_ble_gattc_cb_param_t *p) {
  if(e==ESP_GATTC_NOTIFY_EVT) {
    auto &v=p->notify;
    if(v.handle==dataHandle) decode(v.value,v.value_len);
    if(v.handle==statusHandle && v.value_len) {
      xSemaphoreTake(mutex,portMAX_DELAY);
      if(v.value[0]==2)state.paused=true;
      if(v.value[0]==4)state.paused=false;
      xSemaphoreGive(mutex);
    }
    if(v.handle==trainingHandle && v.value_len>=2) {
      xSemaphoreTake(mutex,portMAX_DELAY);
      if(v.value[1]==1)state.paused=true;
      else if(v.value[1]==0x0d)state.paused=false;
      xSemaphoreGive(mutex);
    }
    if(v.handle==controlHandle && v.value_len>=3 && v.value[0]==0x80) {
      responseCode=v.value[2];responseOp=v.value[1];
      Serial.printf("%lu CONTROL_RESPONSE op=%u code=%u\n",millis(),v.value[1],v.value[2]);
    }
  } else if(e==ESP_GATTC_WRITE_DESCR_EVT) {
    Serial.printf("%lu SUBSCRIPTION_ACK handle=%u status=%u\n",millis(),p->write.handle,p->write.status);
    if(p->write.handle==controlHandle+1)controlSubscribed=p->write.status==0;
  }
}

class ScanEvents:public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if(!target && d.haveName() && d.getName().find("Domyos-Bike-")==0) {
      target=new BLEAdvertisedDevice(d);BLEDevice::getScan()->stop();
      Serial.printf("%lu FOUND %s %s\n",millis(),d.getName().c_str(),d.getAddress().toString().c_str());
    }
  }
};
class ClientEvents:public BLEClientCallbacks {
  void onConnect(BLEClient *)override {}
  void onDisconnect(BLEClient *)override {
    xSemaphoreTake(mutex,portMAX_DELAY);
    state.connected=false;state.valid=false;state.busy=false;state.controlReady=false;
    xSemaphoreGive(mutex);controlSubscribed=false; ownsControl=false;
    message("Connessione persa. Riprovo...");
  }
};

static bool subscribe(BLERemoteCharacteristic *c,bool notify) {
  if(!c)return false;
  auto d=c->getDescriptor(BLEUUID(uint16_t(0x2902)));if(!d)return false;
  auto address=client->getPeerAddress();
  auto rc=esp_ble_gattc_register_for_notify(client->getGattcIf(),*address.getNative(),c->getHandle());
  delay(100);
  uint8_t value[]={uint8_t(notify?1:2),0};
  rc=rc==ESP_OK?esp_ble_gattc_write_char_descr(client->getGattcIf(),client->getConnId(),d->getHandle(),2,value,ESP_GATT_WRITE_TYPE_RSP,ESP_GATT_AUTH_REQ_NONE):rc;
  delay(150);return rc==ESP_OK;
}

static bool command(uint8_t op,int value=-1) {
  if(!control || !client->isConnected())return false;
  responseOp=-1;responseCode=-1;
  uint8_t data[]={op,uint8_t(value)};
  Serial.printf("%lu CONTROL_WRITE op=%u value=%d\n",millis(),op,value);
  control->writeValue(data,value<0?1:2,true);
  uint32_t start=millis();
  while(client->isConnected() && millis()-start<3000 && responseOp!=op)delay(20);
  return responseOp==op && responseCode==1;
}

static void adjust(int delta) {
  auto s=bikeSnapshot();
  if(!s.connected || !s.valid || millis()-s.updated>3000 || !controlSubscribed) {
    message("Controllo non disponibile");return;
  }
  int wanted=constrain(s.resistance+delta,s.minimum,s.maximum);
  if(wanted==s.resistance)return;
  xSemaphoreTake(mutex,portMAX_DELAY);state.busy=true;xSemaphoreGive(mutex);
  message("Invio resistenza...");
  bool ok=ownsControl || command(0x00);
  ownsControl=ok;
  // FTMS opcode 04: one unsigned byte, resolution 0.1; range read as 10..150.
  if(ok)ok=command(0x04,wanted*10);
  if(ok) {
    auto start=millis();
    while(client->isConnected() && millis()-start<3500) {
      if(bikeSnapshot().resistance==wanted)break;delay(30);
    }
    ok=bikeSnapshot().resistance==wanted && client->isConnected();
  }
  message(ok?"Resistenza confermata":"Comando non confermato dalla bike");
  xSemaphoreTake(mutex,portMAX_DELAY);state.busy=false;xSemaphoreGive(mutex);
  // Avoid a queue of delayed commands after a rapid sequence of touches.
  xQueueReset(requests);
}

static void worker(void *) {
  BLEDevice::init("Domyos Bridge");BLEDevice::setCustomGattcHandler(gattEvent);
  bridgeBegin();
  client=BLEDevice::createClient();client->setClientCallbacks(new ClientEvents());
  auto scan=BLEDevice::getScan();scan->setActiveScan(true);
  scan->setAdvertisedDeviceCallbacks(new ScanEvents(),true);
  scan->setInterval(160);scan->setWindow(120);
  while(true) {
    if(!client->isConnected()) {
      control=nullptr;controlHandle=dataHandle=statusHandle=trainingHandle=0;
      controlSubscribed=false;ownsControl=false;
      delete target;target=nullptr;
      message("Ricerca della bike...");scan->start(5,false);
      if(!target){delay(1500);continue;}
      message("Connessione alla bike...");
      if(!client->connect(target)){delay(2000);continue;}
      auto service=client->getService(BLEUUID(uint16_t(0x1826)));
      if(!service){client->disconnect();delay(1000);continue;}
      auto data=service->getCharacteristic(BLEUUID(uint16_t(0x2ad2)));
      auto training=service->getCharacteristic(BLEUUID(uint16_t(0x2ad3)));
      auto status=service->getCharacteristic(BLEUUID(uint16_t(0x2ada)));
      control=service->getCharacteristic(BLEUUID(uint16_t(0x2ad9)));
      auto range=service->getCharacteristic(BLEUUID(uint16_t(0x2ad6)));
      if(!data){client->disconnect();continue;}
      dataHandle=data->getHandle();trainingHandle=training?training->getHandle():0;
      statusHandle=status?status->getHandle():0;controlHandle=control?control->getHandle():0;
      if(range && range->canRead()) {
        auto r=range->readValue();
        if(r.size()==6) {
          int lo=uint8_t(r[0])|(uint8_t(r[1])<<8),hi=uint8_t(r[2])|(uint8_t(r[3])<<8);
          if(lo==10 && hi==150) {xSemaphoreTake(mutex,portMAX_DELAY);state.minimum=1;state.maximum=15;xSemaphoreGive(mutex);}
          Serial.printf("%lu RESISTANCE_RANGE raw=%d..%d\n",millis(),lo,hi);
        }
      }
      subscribe(data,true);subscribe(training,true);subscribe(status,true);subscribe(control,false);
      xSemaphoreTake(mutex,portMAX_DELAY);
      state.connected=client->isConnected();state.controlReady=controlSubscribed;
      xSemaphoreGive(mutex);
      message("Bike collegata");
    }
    int delta;if(xQueueReceive(requests,&delta,pdMS_TO_TICKS(100)))adjust(delta);
    static uint32_t last=0;
    if(millis()-last>2000) {
      last=millis();auto s=bikeSnapshot();
      Serial.printf("%lu DATA speed=%.1f rpm=%.0f resistance=%d watts=%d bpm=%d seconds=%d packets=%u\n",millis(),s.speed,s.cadence,s.resistance,s.power,s.bpm,s.elapsed,s.packets);
    }
    if(Serial.available()) {
      char ch=Serial.read();if(ch=='+')adjust(1);else if(ch=='-')adjust(-1);
    }
  }
}
void bikeBegin() {
  mutex=xSemaphoreCreateMutex();requests=xQueueCreate(1,sizeof(int));
  xTaskCreatePinnedToCore(worker,"bike",12000,nullptr,1,nullptr,0);
}

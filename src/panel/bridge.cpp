#include "bridge.h"
#if BRIDGE_PROFILE != 2
#include "bike.h"
#include "crank_clock.h"
#include <BLEDevice.h>
#include <BLE2902.h>
#include <freertos/semphr.h>

static BLEServer *server;
static BLECharacteristic *powerMeasurement,*motionMeasurement,*powerControl,*motionControl;
static BLE2902 *powerCccd,*motionCccd,*powerControlCccd,*motionControlCccd;
static SemaphoreHandle_t peerMutex;
static QueueHandle_t controlQueue;
static volatile bool restartAdvertising=false,rawReady=false,stopReady=false,advertising=false;
static volatile int activeProfile=0;
static uint32_t lastAdvertising=0;
static constexpr double WHEEL_METRES=2.100;
struct Collector {
  bool used=false,power=false,motion=false,powerIndicate=false,motionIndicate=false;
  uint16_t id=0;
  int profile=0;
  esp_gatt_if_t interface=ESP_GATT_IF_NONE;
};
static Collector collectors[2];
struct ControlRequest { uint16_t id;esp_gatt_if_t interface;bool power;uint8_t opcode,result;uint32_t count; };
static void put16(uint8_t *p,uint16_t n){p[0]=n&255;p[1]=n>>8;}
static void put32(uint8_t *p,uint32_t n){for(int i=0;i<4;++i)p[i]=n>>(8*i);}
static uint32_t get32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void gapEvent(esp_gap_ble_cb_event_t e,esp_ble_gap_cb_param_t *p){
  if(e==ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT)rawReady=p->adv_data_raw_cmpl.status==ESP_BT_STATUS_SUCCESS;
  if(e==ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT){stopReady=true;advertising=false;}
  if(e==ESP_GAP_BLE_ADV_START_COMPLETE_EVT){
    advertising=p->adv_start_cmpl.status==ESP_BT_STATUS_SUCCESS;
    Serial.printf("%lu ADV_START profile=%d status=%u\n",millis(),activeProfile,p->adv_start_cmpl.status);
  }
}
static void advertise(int profile){
  stopReady=false;
  if(advertising){esp_ble_gap_stop_advertising();auto start=millis();while(!stopReady && millis()-start<400)delay(10);}
  advertising=false;activeProfile=profile;
  const char *name=profile==0?"Domyos Bike":"Domyos Motion";
  uint8_t payload[31]={2,1,6,3,3,uint8_t(profile==0?0x18:0x16),0x18,3,0x19,uint8_t(profile==0?0x84:0x85),4};
  size_t length=strlen(name);payload[11]=length+1;payload[12]=0x09;memcpy(payload+13,name,length);
  rawReady=false;
  auto rc=esp_ble_gap_config_adv_data_raw(payload,length+13);
  auto start=millis();while(rc==ESP_OK && !rawReady && millis()-start<700)delay(10);
  if(!rawReady){Serial.printf("%lu ADV_CONFIG_FAILED rc=%d\n",millis(),rc);return;}
  esp_ble_adv_params_t parameters={};
  parameters.adv_int_min=0x100;parameters.adv_int_max=0x180;
  parameters.adv_type=ADV_TYPE_IND;
  parameters.own_addr_type=profile==0?BLE_ADDR_TYPE_RANDOM:BLE_ADDR_TYPE_PUBLIC;
  parameters.channel_map=ADV_CHNL_ALL;
  parameters.adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  rc=esp_ble_gap_start_advertising(&parameters);lastAdvertising=millis();
  Serial.printf("%lu ADVERTISE name=%s rc=%d\n",millis(),name,rc);
}
static void serverEvent(esp_gatts_cb_event_t e,esp_gatt_if_t interface,esp_ble_gatts_cb_param_t *p){
  xSemaphoreTake(peerMutex,portMAX_DELAY);
  if(e==ESP_GATTS_CONNECT_EVT && p->connect.link_role==1){
    for(auto &c:collectors)if(!c.used){c.used=true;c.id=p->connect.conn_id;c.interface=interface;c.profile=activeProfile;break;}
    advertising=false;restartAdvertising=true;
    Serial.printf("%lu WATCH_CONNECTED conn=%u profile=%d\n",millis(),p->connect.conn_id,activeProfile);
  }else if(e==ESP_GATTS_DISCONNECT_EVT && p->disconnect.link_role==1){
    for(auto &c:collectors)if(c.used && c.id==p->disconnect.conn_id)c=Collector();
    restartAdvertising=true;
    Serial.printf("%lu WATCH_DISCONNECTED conn=%u reason=%u\n",millis(),p->disconnect.conn_id,p->disconnect.reason);
  }else if(e==ESP_GATTS_WRITE_EVT && !p->write.is_prep){
    for(auto &c:collectors)if(c.used && c.id==p->write.conn_id){
      bool enabled=p->write.len==2 && (p->write.value[0]&1);
      bool indicate=p->write.len==2 && (p->write.value[0]&2);
      auto h=p->write.handle;
      if(powerCccd && h==powerCccd->getHandle())c.power=enabled;
      else if(motionCccd && h==motionCccd->getHandle())c.motion=enabled;
      else if(powerControlCccd && h==powerControlCccd->getHandle())c.powerIndicate=indicate;
      else if(motionControlCccd && h==motionControlCccd->getHandle())c.motionIndicate=indicate;
      else if((powerControl && h==powerControl->getHandle()) || (motionControl && h==motionControl->getHandle())){
        bool power=powerControl && h==powerControl->getHandle();
        if((power?c.powerIndicate:c.motionIndicate) && p->write.len){
          ControlRequest request={c.id,interface,power,p->write.value[0],2,0};
          if(request.opcode==1){request.result=p->write.len==5?1:3;if(request.result==1)request.count=get32(p->write.value+1);}
          xQueueSend(controlQueue,&request,0);
        }
        continue;
      }else continue;
      Serial.printf("%lu WATCH_CCCD conn=%u handle=%u value=%u\n",millis(),c.id,h,p->write.len?p->write.value[0]:0);
    }
  }
  xSemaphoreGive(peerMutex);
}
class SensorReads:public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *c)override {
    auto value=c->getValue();Serial.printf("%lu WATCH_READ uuid=%s hex=",millis(),c->getUUID().toString().c_str());
    for(auto byte:value)Serial.printf("%02x",uint8_t(byte));Serial.println();
  }
};
static BLECharacteristic *readValue(BLEService *s,uint16_t uuid,uint8_t *p,size_t n){
  auto c=s->createCharacteristic(BLEUUID(uuid),BLECharacteristic::PROPERTY_READ);c->setValue(p,n);c->setCallbacks(new SensorReads());return c;
}
static void sender(void *){
  CrankClock crank;WheelClock wheel;
  uint32_t lastSend=0,lastLog=0,frames=0;bool priorFresh=false;
  while(true){
    uint32_t now=millis();auto bike=bikeSnapshot();
    bool fresh=bike.connected && bike.valid && now-bike.updated<1500;
    crank.update(now,bike.cadence,fresh);
    wheel.update(now,bike.speed*1000.0/(60.0*WHEEL_METRES),fresh);
    Collector clients[2];bool powerEnabled=false,motionEnabled=false;unsigned peers=0;
    xSemaphoreTake(peerMutex,portMAX_DELAY);
    for(int i=0;i<2;++i){
      clients[i]=collectors[i];auto &c=collectors[i];
      if(c.used){++peers;powerEnabled|=c.power;motionEnabled|=c.motion;}
    }
    xSemaphoreGive(peerMutex);
    bool restart=restartAdvertising;restartAdvertising=false;
    if(restart && peers==0)advertise(BRIDGE_PROFILE);
    bikeBridgeState(peers>0,powerEnabled,motionEnabled,frames);
    if(fresh!=priorFresh){Serial.printf("%lu BRIDGE_SOURCE %s\n",now,fresh?"LIVE":"UNAVAILABLE");priorFresh=fresh;}
    ControlRequest control;
    if(xQueueReceive(controlQueue,&control,0)){
      if(control.result==1)wheel.revolutions=control.count;
      uint8_t response[]={uint8_t(control.power?0x20:0x10),control.opcode,control.result};
      auto characteristic=control.power?powerControl:motionControl;
      esp_ble_gatts_send_indicate(control.interface,control.id,characteristic->getHandle(),3,response,true);
      Serial.printf("%lu SENSOR_CONTROL op=%u result=%u\n",now,control.opcode,control.result);
    }
    if(fresh && now-lastSend>=500){
      lastSend=now;
      uint8_t power[14]={0x30,0};put16(power+2,uint16_t(int16_t(bike.power)));
      put32(power+4,wheel.revolutions);put16(power+8,uint16_t(wheel.eventTicks*2));
      put16(power+10,crank.revolutions);put16(power+12,crank.eventTicks);
      uint8_t motion[11]={3};put32(motion+1,wheel.revolutions);put16(motion+5,wheel.eventTicks);
      put16(motion+7,crank.revolutions);put16(motion+9,crank.eventTicks);
      powerMeasurement->setValue(power,sizeof(power));
      if(motionMeasurement)motionMeasurement->setValue(motion,sizeof(motion));
      static uint32_t lastWireLog=0;
      if(powerEnabled && now-lastWireLog>=3000){
        lastWireLog=now;Serial.printf("%lu CPS_WIRE hex=",now);
        for(auto byte:power)Serial.printf("%02x",byte);Serial.println();
      }
      bool sent=false;
      for(auto &c:clients)if(c.used){
        if(c.power && esp_ble_gatts_send_indicate(c.interface,c.id,powerMeasurement->getHandle(),sizeof(power),power,false)==ESP_OK)sent=true;
        if(c.motion && esp_ble_gatts_send_indicate(c.interface,c.id,motionMeasurement->getHandle(),sizeof(motion),motion,false)==ESP_OK)sent=true;
      }
      if(sent)++frames;
    }
    if(now-lastLog>=3000){
      lastLog=now;
      Serial.printf("%lu BRIDGE peers=%u cps=%d csc=%d live=%d watts=%d rpm=%.1f speed=%.1f crank=%u ticks=%u wheel=%lu wheel_ticks=%u frames=%u\n",
        now,peers,powerEnabled,motionEnabled,fresh,bike.power,bike.cadence,bike.speed,
        crank.revolutions,crank.eventTicks,static_cast<unsigned long>(wheel.revolutions),wheel.eventTicks,unsigned(frames));
    }
    delay(50);
  }
}
void bridgeBegin(){
  if(!crankClockSelfTest()){Serial.println("FATAL SENSOR_CLOCK_SELFTEST");while(true)delay(1000);}
  Serial.println("SENSOR_CLOCK_SELFTEST PASS crank/wheel/stop/dropout/count-wrap/time-wrap/millis-wrap");
  peerMutex=xSemaphoreCreateMutex();controlQueue=xQueueCreate(4,sizeof(ControlRequest));
  server=BLEDevice::createServer();BLEDevice::setCustomGattsHandler(serverEvent);BLEDevice::setCustomGapHandler(gapEvent);
  uint8_t location[]={5};
#if BRIDGE_PROFILE == 1
  auto motion=server->createService(BLEUUID(uint16_t(0x1816)));
  motionMeasurement=motion->createCharacteristic(BLEUUID(uint16_t(0x2a5b)),BLECharacteristic::PROPERTY_NOTIFY);
  motionCccd=new BLE2902();motionMeasurement->addDescriptor(motionCccd);
  uint8_t motionFeature[]={3,0};readValue(motion,0x2a5c,motionFeature,2);readValue(motion,0x2a5d,location,1);
  motionControl=motion->createCharacteristic(BLEUUID(uint16_t(0x2a55)),BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_INDICATE);
  motionControlCccd=new BLE2902();motionControl->addDescriptor(motionControlCccd);motion->start();
#endif
  auto power=server->createService(BLEUUID(uint16_t(0x1818)));
  powerMeasurement=power->createCharacteristic(BLEUUID(uint16_t(0x2a63)),BLECharacteristic::PROPERTY_NOTIFY);
  powerCccd=new BLE2902();powerMeasurement->addDescriptor(powerCccd);
  uint8_t powerFeature[]={0x0c,0,0,0};readValue(power,0x2a65,powerFeature,4);readValue(power,0x2a5d,location,1);
  powerControl=power->createCharacteristic(BLEUUID(uint16_t(0x2a66)),BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_INDICATE);
  powerControlCccd=new BLE2902();powerControl->addDescriptor(powerControlCccd);power->start();
  auto info=server->createService(BLEUUID(uint16_t(0x180a)));
  info->createCharacteristic(BLEUUID(uint16_t(0x2a29)),BLECharacteristic::PROPERTY_READ)->setValue("EB520 ESP32 Project");
  info->createCharacteristic(BLEUUID(uint16_t(0x2a24)),BLECharacteristic::PROPERTY_READ)->setValue("Domyos Bike Bridge");
  info->createCharacteristic(BLEUUID(uint16_t(0x2a26)),BLECharacteristic::PROPERTY_READ)->setValue(BRIDGE_VERSION);info->start();
#if BRIDGE_PROFILE == 0
  esp_bd_addr_t address;auto base=BLEDevice::getAddress();memcpy(address,*base.getNative(),6);
  address[0]|=0xc0;address[5]^=3;
  auto rc=esp_ble_gap_set_rand_addr(address);
  Serial.printf("%lu BRIDGE_READY mode=CPS_FULL name=Domyos Bike address=%02x:%02x:%02x:%02x:%02x:%02x random_rc=%d wheel_mm=2100 features=0c000000 flags=3000\n",millis(),address[0],address[1],address[2],address[3],address[4],address[5],rc);
#else
  Serial.printf("%lu BRIDGE_READY mode=MOTION name=Domyos Motion address=%s wheel_mm=2100\n",millis(),BLEDevice::getAddress().toString().c_str());
#endif
  advertise(BRIDGE_PROFILE);
  xTaskCreatePinnedToCore(sender,"watch-bridge",5120,nullptr,1,nullptr,0);
}
#endif

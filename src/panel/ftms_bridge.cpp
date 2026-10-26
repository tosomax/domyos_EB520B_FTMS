#include "bridge.h"
#if BRIDGE_PROFILE == 2
#include "bike.h"
#include <BLEDevice.h>
#include <BLE2902.h>
#include <freertos/semphr.h>
#include <math.h>

static BLECharacteristic *measurement,*training,*machineStatus,*controlPoint;
static BLE2902 *dataCccd,*trainingCccd,*statusCccd,*controlCccd;
static SemaphoreHandle_t lock;
static QueueHandle_t requests;
static volatile bool advReady=false,advRestart=false,resetBaseline=true;
static bool connected=false,dataOn=false,trainingOn=false,statusOn=false,controlOn=false;
static uint16_t connection=0;
static esp_gatt_if_t gattInterface=ESP_GATT_IF_NONE;
struct Request{uint8_t opcode,parameter;uint16_t length;};
static void put16(uint8_t *p,uint16_t n){p[0]=n&255;p[1]=n>>8;}
static void put24(uint8_t *p,uint32_t n){p[0]=n&255;p[1]=n>>8;p[2]=n>>16;}

static void gapEvent(esp_gap_ble_cb_event_t e,esp_ble_gap_cb_param_t *p){
  if(e==ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT)advReady=p->adv_data_raw_cmpl.status==0;
  if(e==ESP_GAP_BLE_ADV_START_COMPLETE_EVT)Serial.printf("%lu FTMS_ADV status=%u\n",millis(),p->adv_start_cmpl.status);
}
static void advertise(){
  const char *name="Domyos FTMS";
  // Fitness Machine Service + available Indoor Bike type, matching the bike.
  uint8_t payload[31]={2,1,6,3,3,0x26,0x18,6,0x16,0x26,0x18,1,0x20,0};
  auto length=strlen(name);payload[14]=length+1;payload[15]=9;memcpy(payload+16,name,length);
  advReady=false;auto rc=esp_ble_gap_config_adv_data_raw(payload,length+16);
  auto start=millis();while(rc==ESP_OK && !advReady && millis()-start<700)delay(10);
  if(!advReady){Serial.printf("%lu FTMS_ADV_CONFIG_ERROR rc=%d\n",millis(),rc);return;}
  esp_ble_adv_params_t parameters={};parameters.adv_int_min=0x100;parameters.adv_int_max=0x180;
  parameters.adv_type=ADV_TYPE_IND;parameters.own_addr_type=BLE_ADDR_TYPE_RANDOM;
  parameters.channel_map=ADV_CHNL_ALL;parameters.adv_filter_policy=ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
  rc=esp_ble_gap_start_advertising(&parameters);Serial.printf("%lu FTMS_ADVERTISING rc=%d\n",millis(),rc);
}
static void serverEvent(esp_gatts_cb_event_t e,esp_gatt_if_t interface,esp_ble_gatts_cb_param_t *p){
  xSemaphoreTake(lock,portMAX_DELAY);
  if(e==ESP_GATTS_CONNECT_EVT && p->connect.link_role==1){
    connected=true;connection=p->connect.conn_id;gattInterface=interface;
    dataOn=trainingOn=statusOn=controlOn=false;resetBaseline=true;
    Serial.printf("%lu FTMS_WATCH_CONNECTED conn=%u\n",millis(),connection);
  }else if(e==ESP_GATTS_DISCONNECT_EVT && p->disconnect.link_role==1 && connection==p->disconnect.conn_id){
    connected=false;dataOn=trainingOn=statusOn=controlOn=false;advRestart=true;
    Serial.printf("%lu FTMS_WATCH_DISCONNECTED reason=%u\n",millis(),p->disconnect.reason);
  }else if(e==ESP_GATTS_WRITE_EVT && connected && p->write.conn_id==connection && !p->write.is_prep){
    uint16_t h=p->write.handle;auto value=p->write.len?p->write.value[0]:0;
    if(h==dataCccd->getHandle())dataOn=p->write.len==2 && (value&1);
    else if(h==trainingCccd->getHandle())trainingOn=p->write.len==2 && (value&1);
    else if(h==statusCccd->getHandle())statusOn=p->write.len==2 && (value&1);
    else if(h==controlCccd->getHandle())controlOn=p->write.len==2 && (value&2);
    else if(h==controlPoint->getHandle() && p->write.len){
      Request request={p->write.value[0],uint8_t(p->write.len>1?p->write.value[1]:0),p->write.len};
      xQueueSend(requests,&request,0);
      Serial.printf("%lu FTMS_CONTROL_REQUEST op=%u len=%u\n",millis(),request.opcode,request.length);
    }
    Serial.printf("%lu FTMS_WRITE handle=%u value=%u data=%d training=%d status=%d control=%d\n",millis(),h,value,dataOn,trainingOn,statusOn,controlOn);
  }else if(e==ESP_GATTS_READ_EVT)Serial.printf("%lu FTMS_READ handle=%u\n",millis(),p->read.handle);
  xSemaphoreGive(lock);
}
static void sender(void *){
  uint32_t distanceBase=0;int energyBase=0,elapsedBase=0;bool baselineSet=false;
  bool owner=false,publish=true,lastPaused=true;uint32_t lastSend=0,lastLog=0,frames=0;
  while(true){
    auto bike=bikeSnapshot();auto now=millis();
    bool fresh=bike.connected && bike.valid && now-bike.updated<1500;
    if(advRestart){advRestart=false;owner=false;publish=true;advertise();}
    if(resetBaseline){resetBaseline=false;baselineSet=false;}
    if(fresh && !baselineSet){distanceBase=bike.distance;energyBase=bike.kcal;elapsedBase=bike.elapsed;baselineSet=true;}
    xSemaphoreTake(lock,portMAX_DELAY);
    bool peer=connected,d=dataOn,t=trainingOn,s=statusOn,c=controlOn;
    uint16_t id=connection;auto interface=gattInterface;
    xSemaphoreGive(lock);
    bikeBridgeState(peer,d,d,frames);
    Request request;
    if(xQueueReceive(requests,&request,0)){
      uint8_t result=2;
      if(request.opcode==0){result=request.length==1?1:3;if(result==1)owner=true;}
      else if(request.opcode==1 || request.opcode==7 || request.opcode==8){
        bool valid=request.opcode==8?(request.length==2 && (request.parameter==1 || request.parameter==2)):request.length==1;
        result=!valid?3:(!owner?5:1);
        if(result==1){
          if(request.opcode==1){distanceBase=bike.distance;energyBase=bike.kcal;elapsedBase=bike.elapsed;}
          if(request.opcode==7)publish=true;
          if(request.opcode==8)publish=false;
        }
      }
      // Commands apply only to this telemetry bridge. Never forward hardware
      // start/reset/stop/resistance commands from the watch to the real bike.
      if(peer && c){uint8_t response[]={0x80,request.opcode,result};esp_ble_gatts_send_indicate(interface,id,controlPoint->getHandle(),3,response,true);}
      Serial.printf("%lu FTMS_CONTROL_RESPONSE op=%u result=%u\n",now,request.opcode,result);
    }
    if(fresh && publish && peer && now-lastSend>=500){
      lastSend=now;
      // 18 bytes: native speed, cadence, distance, power, energy and elapsed.
      // No wrist-HR override, no virtual wheel and no resistance-field ambiguity.
      uint8_t packet[18]={0x54,0x09};
      put16(packet+2,uint16_t(lroundf(bike.speed*100)));put16(packet+4,uint16_t(lroundf(bike.cadence*2)));
      auto distance=uint32_t(bike.distance)>=distanceBase?uint32_t(bike.distance)-distanceBase:0;
      put24(packet+6,distance);put16(packet+9,uint16_t(int16_t(bike.power)));
      put16(packet+11,bike.kcal>=energyBase?bike.kcal-energyBase:0);
      put16(packet+13,bike.kcalHour);packet[15]=bike.kcalMinute;
      put16(packet+16,bike.elapsed>=elapsedBase?bike.elapsed-elapsedBase:0);
      measurement->setValue(packet,sizeof(packet));
      if(d && esp_ble_gatts_send_indicate(interface,id,measurement->getHandle(),sizeof(packet),packet,false)==ESP_OK)++frames;
      uint8_t trainingValue[]={1,uint8_t(bike.paused?1:0x0d)};training->setValue(trainingValue,2);
      if(t)esp_ble_gatts_send_indicate(interface,id,training->getHandle(),2,trainingValue,false);
      if(s && bike.paused!=lastPaused){uint8_t status[]={uint8_t(bike.paused?2:4),2};esp_ble_gatts_send_indicate(interface,id,machineStatus->getHandle(),bike.paused?2:1,status,false);}
      lastPaused=bike.paused;
      if(d && now-lastLog>=2000){
        lastLog=now;Serial.printf("%lu FTMS_TX watts=%d rpm=%.1f speed=%.2f distance=%lu frames=%u hex=",now,bike.power,bike.cadence,bike.speed,static_cast<unsigned long>(distance),unsigned(frames));
        for(auto byte:packet)Serial.printf("%02x",byte);Serial.println();
      }
    }
    delay(50);
  }
}
void bridgeBegin(){
  lock=xSemaphoreCreateMutex();requests=xQueueCreate(8,sizeof(Request));
  auto server=BLEDevice::createServer();BLEDevice::setCustomGattsHandler(serverEvent);BLEDevice::setCustomGapHandler(gapEvent);
  auto service=server->createService(BLEUUID(uint16_t(0x1826)),24);
  auto feature=service->createCharacteristic(BLEUUID(uint16_t(0x2acc)),BLECharacteristic::PROPERTY_READ);
  uint8_t capabilities[]={0x06,0x52,0,0,0,0,0,0};feature->setValue(capabilities,8);
  measurement=service->createCharacteristic(BLEUUID(uint16_t(0x2ad2)),BLECharacteristic::PROPERTY_NOTIFY);
  dataCccd=new BLE2902();measurement->addDescriptor(dataCccd);
  training=service->createCharacteristic(BLEUUID(uint16_t(0x2ad3)),BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_NOTIFY);
  uint8_t idle[]={1,1};training->setValue(idle,2);trainingCccd=new BLE2902();training->addDescriptor(trainingCccd);
  machineStatus=service->createCharacteristic(BLEUUID(uint16_t(0x2ada)),BLECharacteristic::PROPERTY_NOTIFY);
  statusCccd=new BLE2902();machineStatus->addDescriptor(statusCccd);
  controlPoint=service->createCharacteristic(BLEUUID(uint16_t(0x2ad9)),BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_INDICATE);
  controlCccd=new BLE2902();controlPoint->addDescriptor(controlCccd);service->start();
  auto info=server->createService(BLEUUID(uint16_t(0x180a)));
  info->createCharacteristic(BLEUUID(uint16_t(0x2a29)),BLECharacteristic::PROPERTY_READ)->setValue("EB520 ESP32 Project");
  info->createCharacteristic(BLEUUID(uint16_t(0x2a24)),BLECharacteristic::PROPERTY_READ)->setValue("Domyos FTMS Bridge");
  info->createCharacteristic(BLEUUID(uint16_t(0x2a26)),BLECharacteristic::PROPERTY_READ)->setValue(BRIDGE_VERSION);info->start();
  esp_bd_addr_t address;auto base=BLEDevice::getAddress();memcpy(address,*base.getNative(),6);address[0]|=0xc0;address[5]^=5;
  auto rc=esp_ble_gap_set_rand_addr(address);
  Serial.printf("%lu FTMS_READY address=%02x:%02x:%02x:%02x:%02x:%02x rc=%d flags=0954 bytes=18\n",millis(),address[0],address[1],address[2],address[3],address[4],address[5],rc);
  advertise();xTaskCreatePinnedToCore(sender,"ftms-bridge",5120,nullptr,1,nullptr,0);
}
#endif

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <Wire.h>
#include "bike.h"
#include "bridge.h"

static Arduino_SWSPI bus(GFX_NOT_DEFINED,39,48,47,GFX_NOT_DEFINED);
static Arduino_ESP32RGBPanel rgb(18,17,16,21,
  11,12,13,14,0, 8,20,3,46,9,10, 4,5,6,7,15,
  1,10,8,50, 1,10,8,20, 0,8000000);
static Arduino_RGB_Display display(480,480,&rgb,0,true,&bus,GFX_NOT_DEFINED,
  st7701_type9_init_operations,sizeof(st7701_type9_init_operations));
static TwoWire touchBus(1);
static uint8_t touchAddress=0;
static lv_disp_draw_buf_t drawBuffer;
static lv_disp_drv_t displayDriver;
static lv_indev_drv_t inputDriver;
static lv_obj_t *home,*details,*linkLabel,*watchLabel,*statusLabel,*speedLabel,*powerLabel;
static lv_obj_t *rpmLabel,*heartLabel,*kmLabel,*kcalLabel,*timeLabel,*pauseLabel;
static lv_obj_t *resistanceLabel,*minusButton,*plusButton,*resistanceBar;
static lv_obj_t *averageLabel,*hourLabel,*minuteLabel,*packetsLabel,*detailTimeLabel,*rangeLabel;
static constexpr uint32_t BG=0x0B1118,CARD=0x15202C,TEXT=0xF3F7FA,MUTED=0x8EABBA,TEAL=0x3FE1C6,AMBER=0xFFB35C;

static bool readTouch(uint16_t reg,uint8_t *data,size_t n) {
  touchBus.beginTransmission(touchAddress);touchBus.write(reg>>8);touchBus.write(reg&255);
  if(touchBus.endTransmission(false)!=0)return false;
  if(touchBus.requestFrom(touchAddress,uint8_t(n))!=n)return false;
  for(size_t i=0;i<n;++i)data[i]=touchBus.read();return true;
}
static void pollTouch(lv_indev_drv_t *,lv_indev_data_t *data) {
  static int x=0,y=0;static bool pressed=false;
  if(touchAddress) {
    uint8_t status=0;
    if(readTouch(0x814e,&status,1) && (status&0x80)) {
      uint8_t point[7];pressed=false;
      if((status&15)>0 && readTouch(0x814f,point,7)) {
        x=point[1]|(point[2]<<8);y=point[3]|(point[4]<<8);
        pressed=x<480 && y<480;
        static uint32_t last=0;
        if(pressed && millis()-last>300){last=millis();Serial.printf("%lu TOUCH x=%d y=%d\n",millis(),x,y);}
      }
      touchBus.beginTransmission(touchAddress);touchBus.write(0x81);touchBus.write(0x4e);touchBus.write(0);touchBus.endTransmission();
    }
  }
  data->point.x=x;data->point.y=y;data->state=pressed?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;
}
static void flush(lv_disp_drv_t *driver,const lv_area_t *area,lv_color_t *pixels) {
  display.draw16bitRGBBitmap(area->x1,area->y1,reinterpret_cast<uint16_t*>(pixels),area->x2-area->x1+1,area->y2-area->y1+1);
  lv_disp_flush_ready(driver);
}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color=CARD) {
  auto o=lv_obj_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
  lv_obj_set_style_bg_color(o,lv_color_hex(color),0);lv_obj_set_style_border_width(o,0,0);
  lv_obj_set_style_radius(o,18,0);lv_obj_set_style_pad_all(o,0,0);
  lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);return o;
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,const lv_font_t *font,uint32_t color=TEXT) {
  auto o=lv_label_create(parent);lv_label_set_text(o,text);lv_obj_set_pos(o,x,y);
  lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);return o;
}
static void changePage(lv_event_t *) {
  bool hidden=lv_obj_has_flag(details,LV_OBJ_FLAG_HIDDEN);
  if(hidden){lv_obj_add_flag(home,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(details,LV_OBJ_FLAG_HIDDEN);}
  else {lv_obj_add_flag(details,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(home,LV_OBJ_FLAG_HIDDEN);}
}
static void adjust(lv_event_t *e) {
  int delta=int(intptr_t(lv_event_get_user_data(e)));
  Serial.printf("%lu TOUCH_BUTTON delta=%d\n",millis(),delta);bikeAdjust(delta);
}
static lv_obj_t *button(lv_obj_t *parent,const char *text,int x,int y,int w,int h,lv_event_cb_t callback,void *arg=nullptr) {
  auto o=lv_btn_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
  lv_obj_set_style_radius(o,16,0);lv_obj_set_style_bg_color(o,lv_color_hex(0x254050),0);
  lv_obj_set_style_shadow_width(o,0,0);lv_obj_add_event_cb(o,callback,LV_EVENT_CLICKED,arg);
  auto t=lv_label_create(o);lv_label_set_text(t,text);lv_obj_set_style_text_font(t,&lv_font_montserrat_24,0);lv_obj_center(t);return o;
}
static lv_obj_t *metric(lv_obj_t *parent,int x,int y,int w,int h,const char *title,const lv_font_t *font,uint32_t accent=TEXT) {
  auto c=box(parent,x,y,w,h);label(c,title,14,12,&lv_font_montserrat_14,MUTED);
  return label(c,"--",14,35,font,accent);
}
static void buildUI() {
  auto screen=lv_scr_act();lv_obj_set_style_bg_color(screen,lv_color_hex(BG),0);
  lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
  label(screen,"DOMYOS",18,14,&lv_font_montserrat_20,TEAL);
  linkLabel=label(screen,"RICERCA BIKE",134,7,&lv_font_montserrat_14,MUTED);
  watchLabel=label(screen,"OROLOGIO NON COLLEGATO",134,29,&lv_font_montserrat_14,MUTED);
  button(screen,LV_SYMBOL_LIST,408,8,54,36,changePage);
  home=box(screen,0,52,480,410,BG);details=box(screen,0,52,480,410,BG);
  lv_obj_add_flag(details,LV_OBJ_FLAG_HIDDEN);
  speedLabel=metric(home,16,4,260,128,"VELOCITA  /  km/h",&lv_font_montserrat_48,TEAL);
  lv_obj_set_y(speedLabel,48);
  powerLabel=metric(home,288,4,176,128,"POTENZA  /  W",&lv_font_montserrat_48,AMBER);
  lv_obj_set_y(powerLabel,48);
  rpmLabel=metric(home,16,144,104,84,"RPM",&lv_font_montserrat_32);
  heartLabel=metric(home,132,144,104,84,"BPM",&lv_font_montserrat_32);
  kmLabel=metric(home,248,144,104,84,"KM",&lv_font_montserrat_24);
  kcalLabel=metric(home,364,144,100,84,"KCAL",&lv_font_montserrat_32);
  auto timer=box(home,16,240,448,64);
  label(timer,"TEMPO",16,10,&lv_font_montserrat_14,MUTED);
  timeLabel=label(timer,"--:--",16,29,&lv_font_montserrat_24);
  pauseLabel=label(timer,"IN ATTESA",270,25,&lv_font_montserrat_16,TEAL);
  auto resistance=box(home,16,316,448,88);
  minusButton=button(resistance,"-",8,8,76,72,adjust,reinterpret_cast<void*>(intptr_t(-1)));
  plusButton=button(resistance,"+",364,8,76,72,adjust,reinterpret_cast<void*>(intptr_t(1)));
  lv_obj_set_style_bg_color(plusButton,lv_color_hex(0x21695E),0);
  label(resistance,"RESISTENZA",105,9,&lv_font_montserrat_14,MUTED);
  resistanceLabel=label(resistance,"--",202,27,&lv_font_montserrat_32,TEAL);
  resistanceBar=lv_bar_create(resistance);lv_obj_set_pos(resistanceBar,106,68);lv_obj_set_size(resistanceBar,232,6);
  lv_bar_set_range(resistanceBar,1,15);lv_obj_set_style_bg_color(resistanceBar,lv_color_hex(0x2B3F4A),LV_PART_MAIN);
  lv_obj_set_style_bg_color(resistanceBar,lv_color_hex(TEAL),LV_PART_INDICATOR);
  averageLabel=metric(details,16,4,216,110,"VELOCITA MEDIA / km/h",&lv_font_montserrat_32,TEAL);
  hourLabel=metric(details,248,4,216,110,"KCAL / ORA",&lv_font_montserrat_32,AMBER);
  minuteLabel=metric(details,16,126,216,110,"KCAL / MINUTO",&lv_font_montserrat_32,AMBER);
  detailTimeLabel=metric(details,248,126,216,110,"TEMPO / SECONDI",&lv_font_montserrat_32);
  packetsLabel=metric(details,16,248,216,110,"PACCHETTI RICEVUTI",&lv_font_montserrat_32);
  rangeLabel=metric(details,248,248,216,110,"LIVELLI RESISTENZA",&lv_font_montserrat_32);
#if BRIDGE_PROFILE == 2
  label(details,"FTMS: DATI DIRETTI DALLA BIKE",24,361,&lv_font_montserrat_14,MUTED);
#else
  label(details,"RUOTA VIRTUALE: 2100 mm",24,361,&lv_font_montserrat_14,MUTED);
#endif
  label(details,"EB520  /  Bluetooth Fitness Machine",24,380,&lv_font_montserrat_16,MUTED);
  statusLabel=label(screen,"Avvio...",18,465,&lv_font_montserrat_14,MUTED);
  lv_obj_set_width(statusLabel,448);lv_label_set_long_mode(statusLabel,LV_LABEL_LONG_DOT);
}
static void updateUI() {
  auto s=bikeSnapshot();bool fresh=s.connected && s.valid && millis()-s.updated<3000;
  lv_label_set_text(linkLabel,fresh?"BIKE COLLEGATA":(s.connected?"ATTESA DATI BIKE":"RICERCA BIKE"));
  lv_obj_set_style_text_color(linkLabel,lv_color_hex(fresh?TEAL:MUTED),0);
  lv_label_set_text(watchLabel,s.watchConnected?"OROLOGIO COLLEGATO":"OROLOGIO NON COLLEGATO");
  lv_obj_set_style_text_color(watchLabel,lv_color_hex(s.watchConnected?TEAL:MUTED),0);
  lv_label_set_text(statusLabel,s.status);
  if(fresh) {
    lv_label_set_text_fmt(speedLabel,"%.1f",s.speed);lv_label_set_text_fmt(powerLabel,"%d",s.power);
    lv_label_set_text_fmt(rpmLabel,"%.0f",s.cadence);lv_label_set_text_fmt(kmLabel,"%.2f",s.distance/1000.0);
    if(s.bpm>0)lv_label_set_text_fmt(heartLabel,"%d",s.bpm);else lv_label_set_text(heartLabel,"--");
    if(s.kcal!=65535)lv_label_set_text_fmt(kcalLabel,"%d",s.kcal);else lv_label_set_text(kcalLabel,"--");
    lv_label_set_text_fmt(timeLabel,"%02d:%02d",s.elapsed/60,s.elapsed%60);
    lv_label_set_text(pauseLabel,s.paused?"IN PAUSA":"IN ALLENAMENTO");
    lv_obj_set_style_text_opa(timeLabel,(s.paused && (millis()/700)%2)?LV_OPA_40:LV_OPA_COVER,0);
    lv_label_set_text_fmt(resistanceLabel,"%d",s.resistance);lv_bar_set_value(resistanceBar,s.resistance,LV_ANIM_OFF);
    lv_label_set_text_fmt(averageLabel,"%.1f",s.averageSpeed);
    if(s.kcalHour!=65535)lv_label_set_text_fmt(hourLabel,"%d",s.kcalHour);else lv_label_set_text(hourLabel,"--");
    if(s.kcalMinute!=255)lv_label_set_text_fmt(minuteLabel,"%d",s.kcalMinute);else lv_label_set_text(minuteLabel,"--");
    lv_label_set_text_fmt(detailTimeLabel,"%d",s.elapsed);
  } else {
    for(auto l:{speedLabel,powerLabel,rpmLabel,heartLabel,kmLabel,kcalLabel,timeLabel,resistanceLabel,averageLabel,hourLabel,minuteLabel,detailTimeLabel})lv_label_set_text(l,"--");
    lv_label_set_text(pauseLabel,"IN ATTESA");
  }
  lv_label_set_text_fmt(packetsLabel,"%lu",static_cast<unsigned long>(s.packets));
  lv_label_set_text_fmt(rangeLabel,"%d - %d",s.minimum,s.maximum);
  bool enable=fresh && s.controlReady && !s.busy;
  if(enable && s.resistance>s.minimum)lv_obj_clear_state(minusButton,LV_STATE_DISABLED);else lv_obj_add_state(minusButton,LV_STATE_DISABLED);
  if(enable && s.resistance<s.maximum)lv_obj_clear_state(plusButton,LV_STATE_DISABLED);else lv_obj_add_state(plusButton,LV_STATE_DISABLED);
}

void setup() {
  Serial.begin(115200);delay(500);
  Serial.printf("PANEL %s psram=%u flash=%u\n",BRIDGE_VERSION,ESP.getPsramSize(),ESP.getFlashChipSize());
  pinMode(38,OUTPUT);digitalWrite(38,LOW);
  if(!psramFound()){Serial.println("FATAL: PSRAM missing");while(true)delay(1000);}
  if(!display.begin()){Serial.println("FATAL: display begin");while(true)delay(1000);}
  display.fillScreen(0);digitalWrite(38,HIGH);
  touchBus.begin(19,45,100000);touchBus.setTimeOut(20);
  for(uint8_t address:{uint8_t(0x5d),uint8_t(0x14)}) {
    touchBus.beginTransmission(address);if(touchBus.endTransmission()==0){touchAddress=address;break;}
  }
  Serial.printf("DISPLAY_OK touch_address=0x%02x\n",touchAddress);
  lv_init();
  auto pixels=static_cast<lv_color_t*>(heap_caps_malloc(480*20*sizeof(lv_color_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!pixels){Serial.println("FATAL: LVGL buffer");while(true)delay(1000);}
  lv_disp_draw_buf_init(&drawBuffer,pixels,nullptr,480*20);
  lv_disp_drv_init(&displayDriver);displayDriver.hor_res=480;displayDriver.ver_res=480;
  displayDriver.flush_cb=flush;displayDriver.draw_buf=&drawBuffer;lv_disp_drv_register(&displayDriver);
  lv_indev_drv_init(&inputDriver);inputDriver.type=LV_INDEV_TYPE_POINTER;inputDriver.read_cb=pollTouch;lv_indev_drv_register(&inputDriver);
  buildUI();bikeBegin();Serial.println("UI_READY");
}
void loop() {
  static uint32_t last=0;
  if(millis()-last>=250){last=millis();updateUI();}
  lv_timer_handler();delay(5);
}

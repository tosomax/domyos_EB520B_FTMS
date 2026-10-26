#pragma once
#include <Arduino.h>
struct BikeData {
  bool connected=false, valid=false, paused=true, busy=false, controlReady=false;
  bool watchConnected=false, watchPower=false, watchCadence=false;
  uint32_t bridgeFrames=0;
  float speed=0, averageSpeed=0, cadence=0;
  int distance=0, resistance=0, power=0, kcal=0, kcalHour=0, kcalMinute=0, bpm=0, elapsed=0;
  int minimum=1, maximum=15;
  uint32_t updated=0, packets=0;
  char status[80]="Ricerca della bike...";
};
void bikeBegin();
BikeData bikeSnapshot();
void bikeAdjust(int delta);
void bikeBridgeState(bool connected,bool power,bool cadence,uint32_t frames);

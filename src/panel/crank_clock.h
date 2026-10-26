#pragma once
#include <stdint.h>
#include <math.h>

// Integrates the bike's RPM into the cumulative events required by CSC/CPS.
// The same count/time is used by both profiles. No new events while stopped.
template<typename Count=uint16_t>
class RevolutionClock {
  bool initialized=false, active=false;
  uint32_t previousMs=0;
  uint64_t elapsedMs=0;
  double phase=0, previousRpm=0;
public:
  Count revolutions=0;
  uint16_t eventTicks=0;
  void update(uint32_t nowMs,double rpm,bool valid) {
    if(!initialized) { initialized=true;previousMs=nowMs;active=valid;previousRpm=rpm;return; }
    uint32_t delta=nowMs-previousMs;previousMs=nowMs;elapsedMs+=delta;
    if(valid && active && delta<=1000 && previousRpm>0) {
      double total=phase+previousRpm*delta/60000.0;
      uint32_t completed=uint32_t(floor(total+1e-10));
      phase=total-completed;if(phase<0)phase=0;
      if(completed) {
        if(sizeof(Count)==4 && uint64_t(revolutions)+completed>0xffffffffULL)
          completed=uint32_t(0xffffffffULL-revolutions);
        revolutions=Count(revolutions+completed);
        double lastMs=double(elapsedMs)-phase*60000.0/previousRpm;
        eventTicks=uint16_t(uint64_t(lastMs*1.024+1e-6));
      }
    } else if(!valid || !active || delta>1000) phase=0;
    active=valid;previousRpm=(valid && rpm>0 && rpm<1000)?rpm:0;
  }
};
using CrankClock=RevolutionClock<uint16_t>;
using WheelClock=RevolutionClock<uint32_t>;

inline bool crankClockSelfTest() {
  CrankClock c;c.update(0,60,true);
  for(uint32_t t=100;t<=10000;t+=100)c.update(t,60,true);
  if(c.revolutions!=10 || c.eventTicks!=10240)return false;
  c.update(10100,0,true);auto count=c.revolutions;auto stamp=c.eventTicks;
  for(uint32_t t=10200;t<=15000;t+=100)c.update(t,0,true);
  if(c.revolutions!=count || c.eventTicks!=stamp)return false;
  c.update(15100,120,false);c.update(20000,120,true);
  if(c.revolutions!=count)return false;
  c.update(20500,120,true);if(c.revolutions!=uint16_t(count+1))return false;
  CrankClock wrap;wrap.update(0,120,true);
  for(uint32_t t=500;t<=32768000;t+=500)wrap.update(t,120,true);
  if(wrap.revolutions!=0 || wrap.eventTicks!=0)return false;
  CrankClock msWrap;msWrap.update(0xfffffff0u,60,true);
  msWrap.update(984,60,true);
  if(msWrap.revolutions!=1 || msWrap.eventTicks!=1024)return false;
  WheelClock wheel;wheel.update(0,60,true);
  for(uint32_t t=100;t<=10000;t+=100)wheel.update(t,60,true);
  if(wheel.revolutions!=10 || wheel.eventTicks!=10240)return false;
  wheel.revolutions=0xffffffffu;wheel.update(11000,60,true);
  return wheel.revolutions==0xffffffffu;
}

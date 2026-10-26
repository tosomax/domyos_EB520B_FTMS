#pragma once
#ifndef BRIDGE_PROFILE
#define BRIDGE_PROFILE 1
#endif
#if BRIDGE_PROFILE == 2
#define BRIDGE_VERSION "bridge-ftms-6"
#elif BRIDGE_PROFILE == 0
#define BRIDGE_VERSION "bridge-power-full-5"
#else
#define BRIDGE_VERSION "bridge-motion-4"
#endif
// Called once after BLEDevice::init, before the bike connection starts.
void bridgeBegin();

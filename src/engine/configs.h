// TouchDuino engine configuration (replaces MaxDuino's userconfig*.h chain).
// Based on the MaxDuino ESP32 profile with every format enabled.
#ifndef CONFIGS_H_INCLUDED
#define CONFIGS_H_INCLUDED

#define AYPLAY
#define MenuBLK2A
#define ID11CDTspeedup
#define ZX81SPEEDUP
#define Use_c64
#define Use_MZF
#define Use_MTX
#define Use_CAQ
#define tapORIC
  #define ORICSPEEDUP
#define Use_CAS                     // MSX / Dragon / CoCo .cas
  #define Use_DRAGON
    #define Use_Dragon_sLeader
      #define Expand_All
#define Use_UEF                     // Acorn .uef (must be gunzipped)
  #define Use_c112
  #define Use_hqUEF
    #define Use_c104
    #define Use_c117
#define TURBOBAUD1500

#define DEFAULT_BAUDRATE 3850
#define DEFAULT_MSELECTMASK 0       // Force run by default (Auto REM selectable in UI)
#define DEFAULT_TSXzxpUEF 0
#define DEFAULT_SKIP2A 0

#define MAXPAUSE_PERIOD 8191        // ms, max 8191 (13 bits in the pulse buffer)
#define BLKBIGSIZE                  // allow > 255 blocks
#define BLOCKID19_IN
#define BLOCKID21_IN
#define BLOCKTAP_IN

#endif

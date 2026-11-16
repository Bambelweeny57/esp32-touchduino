#ifndef BUFFER_H_INCLUDED
#define BUFFER_H_INCLUDED

#include "Arduino.h" // for types
#include "configs.h"

/* With latest casprocessing logic, buffsize can be any multiple of 2.
*/
#ifdef LARGEBUFFER
  #define buffsize 4096
#else
  #define buffsize 4096  // TouchDuino: 2 x 2048 pulses (~0.75 s each at Spectrum speed) so SD stalls never starve the ISR
#endif

extern volatile bool morebuff;
extern word readpos;
extern word writepos;
extern volatile byte wbuffer[2][buffsize];
extern volatile byte * volatile writeBuffer;
extern volatile byte * readBuffer;
void clearBuffer(void);

#endif // BUFFER_H_INCLUDED

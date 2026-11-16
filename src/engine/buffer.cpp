#include "buffer.h"

volatile bool morebuff = false;
word readpos = 0; // only used within the ISR, never accessed outside, so doesn't need to be volatile
word writepos = 0; // only used within the main loop, never accessed by ISR, so doesn't need to be volatile
DRAM_ATTR volatile byte wbuffer[2][buffsize];
volatile byte * volatile writeBuffer=wbuffer[0]; // the pointer itself is volatile (since the ISR can swap readBuffer/writeBuffer)
volatile byte * readBuffer=wbuffer[1]; // this pointer is not volatile since this pointer is only manipulated by the ISR, not code outside the ISR

void clearBuffer(void)
{
  noInterrupts();
  // TouchDuino: fill with short "hold" entries (0x5000 | 50 us: keep the output
  // level, no edge) instead of zeros, which the ISR plays as 1 ms each. With a
  // bigger buffer, zeros would add seconds of silence at start and after a jump.
  for(word i=0;i<buffsize;i+=2)
  {
    wbuffer[0][i]=0x50; wbuffer[0][i+1]=0x32;
    wbuffer[1][i]=0x50; wbuffer[1][i+1]=0x32;
  }
  interrupts();
}

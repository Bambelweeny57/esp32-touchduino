#ifndef ISR_H_INCLUDED
#define ISR_H_INCLUDED

#include "Arduino.h"

void wave2();

//ISR Variables
extern volatile byte isStopped;
extern volatile byte pinState;
extern volatile bool isPauseBlock;
extern volatile bool wasPauseBlock;
extern volatile bool g_c64_out;     // TouchDuino: the tape being played is a C64 TAP (latched at play)

void reset_output_state();

#endif // ISR_H_INCLUDED

// Display stubs: MaxDuino status text is forwarded to the touch UI instead.
#pragma once
#include "Arduino.h"

void engine_message(const char *msg);

static inline void printtextF(const char *text, int) { engine_message(text); }
static inline void printtext2F(const char *text, int) { engine_message(text); }
static inline void printtext(const char *text, int) { engine_message(text); }

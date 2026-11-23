// Tape output pin (GPIO 17 -> LM386 line out -> 3.5mm jack) and motor sense (GPIO 18).
#pragma once
#include "Arduino.h"
#include "soc/gpio_reg.h"

#define TAPE_OUT_GPIO   17
#define TAPE_MOTOR_GPIO 18

extern volatile uint8_t g_tape_level;
extern volatile uint8_t g_tape_invert;   // Options "Invert output": flips the pin, not g_tape_level
void tape_out_init(void);

__attribute__((always_inline)) static inline void tape_out_write(uint8_t level)
{
    if (level ^ g_tape_invert) REG_WRITE(GPIO_OUT_W1TS_REG, 1UL << TAPE_OUT_GPIO);
    else                       REG_WRITE(GPIO_OUT_W1TC_REG, 1UL << TAPE_OUT_GPIO);
    g_tape_level = level;      // logical level: speaker, live border and self-check are unaffected
}

#define WRITE_HIGH   tape_out_write(1)
#define WRITE_LOW    tape_out_write(0)
#define INIT_OUTPORT tape_out_init()

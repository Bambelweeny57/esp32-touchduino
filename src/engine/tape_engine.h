// TouchDuino tape engine public API (C, thread safe).
// All engine state lives on core 1; these calls post commands to it.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TAPE_STOPPED = 0,
    TAPE_PLAYING,
    TAPE_PAUSED,
} tape_state_t;

typedef struct {
    tape_state_t state;
    uint32_t pos;          // bytes consumed
    uint32_t size;         // file size
    int block;             // current block index (0-based), -1 if unknown
    int block_count;       // number of navigable blocks (0 = format has no blocks)
    uint8_t block_id;      // TZX block ID being processed
    bool motor_on;         // REM line state (true = computer asks for play)
    char msg[48];          // last engine message
    char file[128];        // basename being played
} tape_status_t;

typedef struct {
    uint16_t baud;         // 1200, 2400, 3150, 3600, 3850 (TSX/CAS/CDT speed-up)
    bool motor_auto;       // true = Auto REM, false = Force run
    bool polarity;         // MaxDuino TSXCONTROLzxpolarityUEFSWITCHPARITY
    bool skip2a;           // ignore TZX ID2A "stop if 48K"
    bool speaker;          // mirror signal to the onboard speaker
    int volume;            // 0..100
} tape_settings_t;

void tape_engine_start(const tape_settings_t *initial);
void tape_play(const char *path);
void tape_pause_toggle(void);
void tape_stop(void);
void tape_block_skip(int delta);      // +1 / -1 blocks
void tape_apply_settings(const tape_settings_t *s);
void tape_get_status(tape_status_t *out);
const char *tape_block_name(uint8_t id);

#ifdef __cplusplus
}
#endif

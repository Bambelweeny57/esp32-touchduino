#pragma once
// No physical buttons: the engine's "wait for STOP" on an unknown block returns
// immediately so the file is stopped and the UI shows the error.
static inline bool button_stop() { return true; }

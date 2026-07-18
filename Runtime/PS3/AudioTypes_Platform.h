/**
 * @file AudioTypes_Platform.h
 * @brief PS3 platform extension for the engine's `AudioTypes.h` fork.
 *
 * Stub — the engine's `AudioTypes.h` only forks on Windows today (for
 * XAudio2 types). PS3 audio runs via PSL1GHT's libaudio; no shared types
 * need to leak into the engine layer at present.
 */

#pragma once
#include <stdint.h>

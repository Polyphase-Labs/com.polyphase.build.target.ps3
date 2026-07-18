/**
 * @file NetworkTypes_Platform.h
 * @brief PS3 platform extension for the engine's `NetworkTypes.h` fork.
 *
 * Stub. The real PS3 port can opt into PSL1GHT's `net` sockets and add
 * proper socket headers; for now PS3 networking is treated as "not present"
 * by mapping SocketHandle to int32 (matching all the other Unix-like
 * platforms).
 */

#pragma once
#include <stdint.h>

typedef int32_t SocketHandle;

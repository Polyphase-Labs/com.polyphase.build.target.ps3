/**
 * @file InputTypes_Platform.h
 * @brief PS3 platform extension for the engine's `InputTypes.h` fork.
 *
 * Pulls in PSL1GHT's pad header so engine code that references PS3 input
 * types compiles. Input_PS3.cpp keeps padData internal, but exposing the
 * header here means future engine inline blocks touching PS3-specific input
 * types just work.
 */

#pragma once

#include <stdint.h>
#include <io/pad.h>

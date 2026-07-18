/**
 * @file compat/memory.h
 * @brief Compatibility shim for Bullet's legacy `#include <memory.h>`.
 *
 * Several Bullet headers (btSerializer.h, btAlignedObjectArray.h, ...) include
 * the historical SVR4 header <memory.h>, which PSL1GHT's newlib does not ship
 * (the memory functions live in <string.h>). This dir goes on the PS3 include
 * path (Makefile_PS3) so the angle-bracket lookup resolves here and forwards
 * to <string.h>. Harmless on any toolchain that also lacks <memory.h>.
 */
#pragma once
#include <string.h>

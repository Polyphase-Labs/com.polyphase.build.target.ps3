/**
 * @file PS3RSXTypes.h
 * @brief Shared RSX render-state types for the PS3 graphics backend.
 *
 * Phase-1 placeholder: the minimal-bootable backend only brings up video +
 * an RSX context and clears/flips a framebuffer, so it needs no vertex-format
 * or pipeline-state structs yet. Real RSX draw resources (repacked vertex
 * layouts, gcmTexture wrappers, Cg shader handles) land here in later phases,
 * mirroring PSPGUTypes.h on the PSP port.
 */

#pragma once

#include <ppu-types.h>

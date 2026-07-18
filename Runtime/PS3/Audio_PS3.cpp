/**
 * @file Audio_PS3.cpp
 * @brief PS3 audio layer — Phase-1 silent stub.
 *
 * Satisfies the engine's AUD_* surface so the build links and boots. Actual
 * mixing on PSL1GHT's libaudio (audioInit / audioPortOpen + a DMA-fed ring
 * on a dedicated thread) is deferred past the minimal-bootable milestone.
 * Every voice/stream call is a no-op that returns a benign value; nothing
 * plays, but nothing crashes either.
 *
 * When implementing real streaming later, remember to wire the three
 * AudioAnalysis hooks (OnStreamOpened/Submitted/Closed) so visualizers work —
 * see the polyphase-buildtarget skill's "Audio analysis hook" section.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Log.h"

#include <stdlib.h>

void AUD_Initialize()
{
    LogDebug("Audio_PS3: Phase-1 silent stub (no output)");
}

void AUD_Shutdown() {}
void AUD_Update() {}

void AUD_Play(uint32_t /*voiceIndex*/, SoundWave* /*soundWave*/, float /*volume*/,
              float /*pitch*/, bool /*loop*/, float /*startTime*/, bool /*spatial*/) {}
void AUD_Stop(uint32_t /*voiceIndex*/) {}
bool AUD_IsPlaying(uint32_t /*voiceIndex*/) { return false; }
void AUD_SetVolume(uint32_t /*voiceIndex*/, float /*leftVolume*/, float /*rightVolume*/) {}
void AUD_SetPitch(uint32_t /*voiceIndex*/, float /*pitch*/) {}

uint8_t* AUD_AllocWaveBuffer(uint32_t size) { return (uint8_t*)malloc(size); }
void AUD_FreeWaveBuffer(void* buffer) { free(buffer); }
void AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

uint32_t AUD_OpenStream(uint32_t /*sampleRate*/, uint32_t /*numChannels*/, uint32_t /*bitsPerSample*/) { return 0; }
void     AUD_CloseStream(uint32_t /*streamId*/) {}
int32_t  AUD_SubmitStreamBuffer(uint32_t /*streamId*/, const uint8_t* /*data*/, uint32_t byteSize) { return (int32_t)byteSize; }
uint64_t AUD_GetStreamPlayedSamples(uint32_t /*streamId*/) { return 0; }
void     AUD_SetStreamVolume(uint32_t /*streamId*/, float /*volume*/) {}
void     AUD_SetStreamPaused(uint32_t /*streamId*/, bool /*paused*/) {}
void     AUD_FlushStream(uint32_t /*streamId*/) {}

#endif // POLYPHASE_PLATFORM_ADDON

/**
 * @file Audio_PS3.cpp
 * @brief PS3 audio — software mixer feeding one PSL1GHT libaudio port.
 *
 * Architecture (mirrors the PSP backend; only the HW layer differs):
 *   - The engine drives AUD_Play(voiceIndex 0..7, soundWave, vol, pitch, loop)
 *     onto an internal voice table; AUD_Stop/SetVolume/SetPitch mutate entries.
 *   - libaudio exposes a single interleaved 32-bit-float ring (2ch, 256-frame
 *     blocks, fixed 48000 Hz). A dedicated mixer thread waits on the audio
 *     notify event queue; on every "block consumed" notification it mixes one
 *     256-frame block from all active voices + streams and writes it into the
 *     ring. Per-voice nearest-neighbour resampling via a fractional cursor
 *     advanced by rate = sourceRate * pitch / 48000.
 *   - Mixing is done in float (libaudio wants float -1..1); int16/uint8 source
 *     PCM is converted on fetch. A 0.5x master attenuation keeps summed voices
 *     from clipping.
 *   - Voice/stream tables are guarded by an lv2 sys_mutex, held briefly.
 *
 * SoundWave pointers in voices are weak refs; the engine's asset refcount keeps
 * them alive for the duration of playback.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Audio/Audio.h"
#include "Audio/AudioConstants.h"
#include "Audio/AudioAnalysis.h"
#include "Engine/Assets/SoundWave.h"
#include "Log.h"

// NOTE: included as <audio.h> (not <audio/audio.h>) via the dedicated
// -I$(PSL1GHT)/ppu/include/audio in Makefile_PS3. On WSL's case-insensitive
// /mnt, <audio/audio.h> would collide with the engine's Audio/Audio.h.
#include <audio.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>
#include <lv2/thread.h>
#include <sysmodule/sysmodule.h>

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

namespace
{
    // ----- Output format (libaudio is fixed 48 kHz, 256-frame float blocks) --
    constexpr int    kOutputRate  = 48000;
    constexpr int    kBlockFrames  = AUDIO_BLOCK_SAMPLES;   // 256
    constexpr float  kMasterAtten  = 0.5f;                  // ~6 dB headroom

    // ----- Voice table -----------------------------------------------------
    struct Ps3Voice
    {
        bool           active        = false;
        const uint8_t* pcmData       = nullptr;
        uint32_t       numFrames     = 0;
        uint32_t       sampleRate    = 0;
        uint8_t        numChannels   = 1;
        uint8_t        bitsPerSample = 16;
        bool           loop          = false;
        double         positionFrac  = 0.0;
        double         rate          = 1.0;
        float          leftVol       = 1.0f;
        float          rightVol      = 1.0f;
    };
    static Ps3Voice sVoices[AUDIO_MAX_VOICES];

    // ----- Streaming voice table (video-player / external PCM feeders) ------
    constexpr uint32_t kMaxStreams     = 4;
    constexpr double   kStreamRingSecs = 0.5;

    struct Ps3Stream
    {
        bool     inUse         = false;
        bool     paused        = false;
        uint32_t srcSampleRate = 0;
        uint8_t  numChannels   = 1;
        uint8_t  bitsPerSample = 16;
        int16_t* ring          = nullptr;
        uint32_t ringFrames    = 0;
        uint64_t writeFrameAbs = 0;
        double   readFrameAbs  = 0.0;
        double   rate          = 1.0;
        float    leftVol       = 1.0f;
        float    rightVol      = 1.0f;
    };
    static Ps3Stream sStreams[kMaxStreams];

    // ----- libaudio / thread state -----------------------------------------
    static sys_mutex_t         sLock        = 0;
    static bool                sLockValid   = false;
    static sys_ppu_thread_t    sMixerThread = 0;
    static bool                sThreadValid = false;
    static volatile bool       sMixerRun    = false;
    static u32                 sPortNum     = 0;
    static bool                sPortOpen    = false;
    static sys_event_queue_t   sQueue       = 0;
    static sys_ipc_key_t       sQueueKey    = 0;
    static u64                 sAudioDataStart = 0;
    static u64                 sChannelCount   = 2;
    static u64                 sNumBlocks      = AUDIO_BLOCK_8;
    static u32                 sWriteBlock     = 0;

    static float sMix[kBlockFrames * 2];   // stereo float scratch

    inline void Lock()   { if (sLockValid) sysMutexLock(sLock, 0); }
    inline void Unlock() { if (sLockValid) sysMutexUnlock(sLock); }

    inline float ClampVol(float v)
    {
        const float s = v * kMasterAtten;
        return (s < 0.0f) ? 0.0f : s;
    }

    // One source frame → float L/R (already at s16-equivalent magnitude, /32768).
    inline void FetchFrame(const Ps3Voice& v, uint32_t f, float& outL, float& outR)
    {
        if (v.bitsPerSample == 16)
        {
            const int16_t* s = reinterpret_cast<const int16_t*>(v.pcmData);
            if (v.numChannels == 2) { outL = s[f * 2] * (1.0f / 32768.0f); outR = s[f * 2 + 1] * (1.0f / 32768.0f); }
            else                    { const float m = s[f] * (1.0f / 32768.0f); outL = outR = m; }
        }
        else   // 8-bit unsigned
        {
            if (v.numChannels == 2)
            {
                outL = (int32_t(v.pcmData[f * 2])     - 128) * (1.0f / 128.0f);
                outR = (int32_t(v.pcmData[f * 2 + 1]) - 128) * (1.0f / 128.0f);
            }
            else { const float m = (int32_t(v.pcmData[f]) - 128) * (1.0f / 128.0f); outL = outR = m; }
        }
    }

    inline void FetchStreamFrame(const Ps3Stream& s, uint64_t f, float& outL, float& outR)
    {
        const uint32_t idx = (uint32_t)(f % s.ringFrames);
        if (s.numChannels == 2) { outL = s.ring[idx * 2] * (1.0f / 32768.0f); outR = s.ring[idx * 2 + 1] * (1.0f / 32768.0f); }
        else                    { const float m = s.ring[idx] * (1.0f / 32768.0f); outL = outR = m; }
    }

    void MixOneBlock()
    {
        memset(sMix, 0, sizeof(sMix));

        Lock();
        for (uint32_t vi = 0; vi < AUDIO_MAX_VOICES; ++vi)
        {
            Ps3Voice& v = sVoices[vi];
            if (!v.active || v.pcmData == nullptr || v.numFrames == 0) continue;
            for (int f = 0; f < kBlockFrames; ++f)
            {
                uint32_t srcFrame = (uint32_t)v.positionFrac;
                if (srcFrame >= v.numFrames)
                {
                    if (v.loop)
                    {
                        v.positionFrac -= (double)v.numFrames;
                        srcFrame = (uint32_t)v.positionFrac;
                        if (srcFrame >= v.numFrames) srcFrame = 0;
                    }
                    else { v.active = false; break; }
                }
                float l, r;
                FetchFrame(v, srcFrame, l, r);
                sMix[f * 2]     += l * v.leftVol;
                sMix[f * 2 + 1] += r * v.rightVol;
                v.positionFrac += v.rate;
            }
        }

        for (uint32_t si = 0; si < kMaxStreams; ++si)
        {
            Ps3Stream& s = sStreams[si];
            if (!s.inUse || s.paused || s.ring == nullptr) continue;
            for (int f = 0; f < kBlockFrames; ++f)
            {
                const uint64_t srcAbs = (uint64_t)s.readFrameAbs;
                if (srcAbs >= s.writeFrameAbs) break;   // under-run: silence
                float l, r;
                FetchStreamFrame(s, srcAbs, l, r);
                sMix[f * 2]     += l * s.leftVol;
                sMix[f * 2 + 1] += r * s.rightVol;
                s.readFrameAbs += s.rate;
            }
        }
        Unlock();

        // Write the float block into the ring, clamped to [-1, 1].
        float* buf = (float*)(u64)sAudioDataStart;
        float* block = buf + sWriteBlock * sChannelCount * kBlockFrames;
        for (int i = 0; i < kBlockFrames * 2; ++i)
        {
            float s = sMix[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            block[i] = s;
        }
        sWriteBlock = (sWriteBlock + 1) % (u32)sNumBlocks;
    }

    void MixerThread(void* /*arg*/)
    {
        sys_event_t event;
        while (sMixerRun)
        {
            // Wait for the audio server to signal a consumed block (~5.3 ms at
            // 48 kHz / 256). 100 ms timeout so shutdown is noticed promptly.
            if (sysEventQueueReceive(sQueue, &event, 100 * 1000) != 0) continue;
            MixOneBlock();
        }
        sysThreadExit(0);
    }
}  // namespace

// ----- API impl ------------------------------------------------------------

void AUD_Initialize()
{
    if (sysModuleLoad(SYSMODULE_AUDIO) != 0)
    {
        LogError("Audio_PS3: sysModuleLoad(SYSMODULE_AUDIO) failed — silent");
        return;
    }
    if (audioInit() != 0)
    {
        LogError("Audio_PS3: audioInit failed — silent");
        sysModuleUnload(SYSMODULE_AUDIO);
        return;
    }

    audioPortParam param;
    memset(&param, 0, sizeof(param));
    param.numChannels = AUDIO_PORT_2CH;
    param.numBlocks   = AUDIO_BLOCK_8;
    param.attrib      = AUDIO_PORT_INITLEVEL;
    param.level       = 1.0f;
    if (audioPortOpen(&param, &sPortNum) != 0)
    {
        LogError("Audio_PS3: audioPortOpen failed — silent");
        audioQuit(); sysModuleUnload(SYSMODULE_AUDIO);
        return;
    }
    sPortOpen = true;

    audioPortConfig config;
    memset(&config, 0, sizeof(config));
    audioGetPortConfig(sPortNum, &config);
    sAudioDataStart = (u64)config.audioDataStart;
    sChannelCount   = config.channelCount;
    sNumBlocks      = config.numBlocks;
    sWriteBlock     = 0;

    if (audioCreateNotifyEventQueue(&sQueue, &sQueueKey) != 0 ||
        audioSetNotifyEventQueue(sQueueKey) != 0)
    {
        LogError("Audio_PS3: notify event queue setup failed — silent");
        audioPortClose(sPortNum); sPortOpen = false;
        audioQuit(); sysModuleUnload(SYSMODULE_AUDIO);
        return;
    }
    sysEventQueueDrain(sQueue);

    sys_mutex_attr_t mattr;
    sysMutexAttrInitialize(mattr);
    if (sysMutexCreate(&sLock, &mattr) == 0) sLockValid = true;

    LogDebug("Audio_PS3: port cfg dataStart=0x%08llx channels=%llu blocks=%llu",
             (unsigned long long)sAudioDataStart, (unsigned long long)sChannelCount,
             (unsigned long long)sNumBlocks);

    audioPortStart(sPortNum);

    sMixerRun = true;
    if (sysThreadCreate(&sMixerThread, &MixerThread, nullptr, 1000, 0x4000,
                        THREAD_JOINABLE, (char*)"polyaud_mixer") == 0)
    {
        sThreadValid = true;
    }
    else
    {
        LogError("Audio_PS3: mixer thread create failed — silent");
        sMixerRun = false;
    }

    LogDebug("Audio_PS3: libaudio up, port=%u 48000Hz stereo, %u blocks, %d voices, mixer=%d",
             (unsigned)sPortNum, (unsigned)sNumBlocks, AUDIO_MAX_VOICES, (int)sThreadValid);
}

void AUD_Shutdown()
{
    sMixerRun = false;
    if (sThreadValid)
    {
        u64 ret = 0;
        sysThreadJoin(sMixerThread, &ret);
        sThreadValid = false;
    }
    if (sPortOpen)
    {
        audioPortStop(sPortNum);
        if (sQueueKey != 0) audioRemoveNotifyEventQueue(sQueueKey);
        audioPortClose(sPortNum);
        sPortOpen = false;
    }
    if (sQueue != 0) { sysEventQueueDestroy(sQueue, 1); sQueue = 0; }
    audioQuit();
    sysModuleUnload(SYSMODULE_AUDIO);

    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        if (sStreams[i].ring != nullptr) free(sStreams[i].ring);
        sStreams[i] = Ps3Stream{};
    }
    if (sLockValid) { sysMutexDestroy(sLock); sLockValid = false; }
}

void AUD_Update() { /* mixer thread does the work */ }

void AUD_Play(uint32_t voiceIndex, SoundWave* soundWave, float volume,
              float pitch, bool loop, float /*startTime*/, bool /*spatial*/)
{
    if (voiceIndex >= AUDIO_MAX_VOICES || soundWave == nullptr) return;
    const uint8_t* pcm = soundWave->GetWaveData();
    const uint32_t numFrames = soundWave->GetNumSamples();
    if (pcm == nullptr || numFrames == 0) return;

    Lock();
    Ps3Voice& v     = sVoices[voiceIndex];
    v.pcmData       = pcm;
    v.numFrames     = numFrames;
    v.sampleRate    = soundWave->GetSampleRate();
    v.numChannels   = (uint8_t)soundWave->GetNumChannels();
    v.bitsPerSample = (uint8_t)soundWave->GetBitsPerSample();
    v.loop          = loop;
    v.positionFrac  = 0.0;
    v.rate          = (v.sampleRate > 0) ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate) : 1.0;
    const float vol = ClampVol(volume);
    v.leftVol       = vol;
    v.rightVol      = vol;
    v.active        = true;
    Unlock();
}

void AUD_Stop(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    Lock(); sVoices[voiceIndex].active = false; Unlock();
}

bool AUD_IsPlaying(uint32_t voiceIndex)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return false;
    return sVoices[voiceIndex].active;
}

void AUD_SetVolume(uint32_t voiceIndex, float leftVolume, float rightVolume)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    Lock();
    sVoices[voiceIndex].leftVol  = ClampVol(leftVolume);
    sVoices[voiceIndex].rightVol = ClampVol(rightVolume);
    Unlock();
}

void AUD_SetPitch(uint32_t voiceIndex, float pitch)
{
    if (voiceIndex >= AUDIO_MAX_VOICES) return;
    Lock();
    Ps3Voice& v = sVoices[voiceIndex];
    v.rate = (v.sampleRate > 0) ? ((double)v.sampleRate * (double)pitch / (double)kOutputRate) : 1.0;
    Unlock();
}

uint8_t* AUD_AllocWaveBuffer(uint32_t size) { return (uint8_t*)malloc(size); }
void     AUD_FreeWaveBuffer(void* buffer)   { free(buffer); }
void     AUD_ProcessWaveBuffer(SoundWave* /*soundWave*/) {}

// ----- Streaming voices ---------------------------------------------------

uint32_t AUD_OpenStream(uint32_t sampleRate, uint32_t numChannels, uint32_t bitsPerSample)
{
    if (!sPortOpen) return 0;
    if ((numChannels != 1 && numChannels != 2) || bitsPerSample != 16)
    {
        LogWarning("AUD_OpenStream: only 16-bit mono/stereo supported");
        return 0;
    }

    Lock();
    uint32_t picked = kMaxStreams;
    for (uint32_t i = 0; i < kMaxStreams; ++i) { if (!sStreams[i].inUse) { picked = i; break; } }
    if (picked == kMaxStreams) { Unlock(); return 0; }

    Ps3Stream& s = sStreams[picked];
    const uint32_t ringFrames = (uint32_t)((double)sampleRate * kStreamRingSecs);
    s.ring = (int16_t*)memalign(16, (size_t)ringFrames * numChannels * sizeof(int16_t));
    if (s.ring == nullptr) { Unlock(); LogError("AUD_OpenStream: ring alloc failed"); return 0; }

    s.ringFrames    = ringFrames;
    s.srcSampleRate = sampleRate;
    s.numChannels   = (uint8_t)numChannels;
    s.bitsPerSample = (uint8_t)bitsPerSample;
    s.writeFrameAbs = 0;
    s.readFrameAbs  = 0.0;
    s.rate          = (double)sampleRate / (double)kOutputRate;
    s.leftVol       = ClampVol(1.0f);
    s.rightVol      = ClampVol(1.0f);
    s.paused        = false;
    s.inUse         = true;
    Unlock();

    AudioAnalysis::OnStreamOpened(picked + 1, sampleRate, numChannels, bitsPerSample);
    return picked + 1;
}

void AUD_CloseStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    Lock();
    Ps3Stream& s = sStreams[streamId - 1];
    if (s.inUse)
    {
        if (s.ring != nullptr) { free(s.ring); s.ring = nullptr; }
        s = Ps3Stream{};
    }
    Unlock();
    AudioAnalysis::OnStreamClosed(streamId);
}

int32_t AUD_SubmitStreamBuffer(uint32_t streamId, const uint8_t* data, uint32_t byteSize)
{
    if (streamId == 0 || streamId > kMaxStreams || data == nullptr || byteSize == 0) return 0;

    Lock();
    Ps3Stream& s = sStreams[streamId - 1];
    if (!s.inUse || s.ring == nullptr) { Unlock(); return 0; }

    const uint32_t bpf = s.numChannels * 2;
    uint32_t submitFrames = byteSize / bpf;
    if (submitFrames == 0) { Unlock(); return 0; }
    if (submitFrames > s.ringFrames) submitFrames = s.ringFrames;

    const uint64_t readAbs   = (uint64_t)s.readFrameAbs;
    const uint64_t inFlight  = s.writeFrameAbs - readAbs;
    const uint32_t freeFrames = (inFlight < s.ringFrames) ? (uint32_t)(s.ringFrames - inFlight) : 0u;
    if (submitFrames > freeFrames) { Unlock(); return 0; }

    const int16_t* src = reinterpret_cast<const int16_t*>(data);
    const uint32_t head = (uint32_t)(s.writeFrameAbs % s.ringFrames);
    const uint32_t firstFrames = (head + submitFrames <= s.ringFrames) ? submitFrames : (s.ringFrames - head);
    memcpy(s.ring + head * s.numChannels, src, (size_t)firstFrames * s.numChannels * sizeof(int16_t));
    if (firstFrames < submitFrames)
    {
        memcpy(s.ring, src + firstFrames * s.numChannels,
               (size_t)(submitFrames - firstFrames) * s.numChannels * sizeof(int16_t));
    }
    s.writeFrameAbs += submitFrames;
    Unlock();

    AudioAnalysis::OnStreamSubmitted(streamId, data, submitFrames * bpf);
    return (int32_t)(submitFrames * bpf);
}

uint64_t AUD_GetStreamPlayedSamples(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return 0;
    const Ps3Stream& s = sStreams[streamId - 1];
    return s.inUse ? (uint64_t)s.readFrameAbs : 0;
}

void AUD_SetStreamVolume(uint32_t streamId, float volume)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    Lock();
    Ps3Stream& s = sStreams[streamId - 1];
    if (s.inUse) { s.leftVol = ClampVol(volume); s.rightVol = ClampVol(volume); }
    Unlock();
}

void AUD_SetStreamPaused(uint32_t streamId, bool paused)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    Lock();
    if (sStreams[streamId - 1].inUse) sStreams[streamId - 1].paused = paused;
    Unlock();
}

void AUD_FlushStream(uint32_t streamId)
{
    if (streamId == 0 || streamId > kMaxStreams) return;
    Lock();
    Ps3Stream& s = sStreams[streamId - 1];
    if (s.inUse) s.writeFrameAbs = (uint64_t)s.readFrameAbs;
    Unlock();
}

#endif // POLYPHASE_PLATFORM_ADDON

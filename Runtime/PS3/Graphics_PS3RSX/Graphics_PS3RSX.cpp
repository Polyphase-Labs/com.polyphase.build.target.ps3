/**
 * @file Graphics_PS3RSX.cpp
 * @brief PS3 graphics backend (PSL1GHT / RSX) — Phase-1 minimal bootable.
 *
 * Brings up the video output + an RSX context and clears/flips a double-
 * buffered framebuffer every frame, so the engine boots to a solid cleared
 * screen and the main loop runs end-to-end. All actual draw calls
 * (meshes / widgets / text) are no-op stubs for this milestone — real RSX
 * rendering (surface setup, Cg shaders, vertex upload, TEV-equivalent
 * material state) is Phase 2+, mirroring how the PSP port phased in
 * Graphics_PSPGU.
 *
 * The clear is done with a CPU fill of the RSX-mapped scanout buffer rather
 * than the RSX clear-command pipeline — it depends only on the stable
 * rsxInit / videoConfigure / gcmSetDisplayBuffer / gcmSetFlip surface, which
 * is enough to prove the boot path without the full command-buffer draw API.
 *
 * The GFX_* function set below matches Graphics_PSPGU.cpp exactly so the
 * engine links identically on both console addons.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Engine.h"
#include "Log.h"
#include "Maths.h"

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <sys/systime.h>

#include <malloc.h>
#include <string.h>

namespace
{
    // Sizes from the PSL1GHT rsxtest sample: 512 KB command buffer, 16 MB host
    // IO region (for the command ring + any CPU->RSX uploads). Framebuffers come
    // from RSX LOCAL memory via rsxMemalign, not from this region.
    constexpr u32 kCommandBufferSize = 0x80000;         // 512 KB
    constexpr u32 kHostSize          = 16 * 1024 * 1024;
    constexpr u8  kLabelIndex        = 255;

    gcmContextData* gCtx        = nullptr;
    void*           gHostAddr   = nullptr;

    u32   gDisplayWidth  = 1280;
    u32   gDisplayHeight = 720;
    u32   gColorPitch    = 1280 * 4;

    u32*  gColorBuffer[2] = { nullptr, nullptr };
    u32   gColorOffset[2] = { 0, 0 };
    u32*  gDepthBuffer    = nullptr;
    u32   gDepthOffset    = 0;
    u32   gDepthPitch     = 0;
    u32   gCurrentBuffer  = 0;
    u32   gFirstFlip      = 1;
    u32   gLabelVal       = 1;

    bool  gInitialized = false;

    // XRGB8888 clear colour — dark slate blue.
    constexpr u32 kClearColor = 0x00203040u;

    // Wait for the RSX to drain all queued commands (label round-trip) —
    // mirrors rsxutil.cpp's waitFinish/waitRSXIdle.
    void WaitRSXFinish()
    {
        rsxSetWriteBackendLabel(gCtx, kLabelIndex, gLabelVal);
        rsxFlushBuffer(gCtx);
        while (*(vu32*)gcmGetLabelAddress(kLabelIndex) != gLabelVal) sysUsleep(30);
        ++gLabelVal;
    }
    void WaitRSXIdle()
    {
        rsxSetWriteBackendLabel(gCtx, kLabelIndex, gLabelVal);
        rsxSetWaitLabel(gCtx, kLabelIndex, gLabelVal);
        ++gLabelVal;
        WaitRSXFinish();
    }

    // Bind colour buffer `index` + the shared depth buffer as the render target.
    void SetRenderTarget(u32 index)
    {
        gcmSurface sf;
        sf.colorFormat      = GCM_SURFACE_X8R8G8B8;
        sf.colorTarget      = GCM_SURFACE_TARGET_0;
        sf.colorLocation[0] = GCM_LOCATION_RSX;
        sf.colorOffset[0]   = gColorOffset[index];
        sf.colorPitch[0]    = gColorPitch;
        sf.colorLocation[1] = GCM_LOCATION_RSX;
        sf.colorLocation[2] = GCM_LOCATION_RSX;
        sf.colorLocation[3] = GCM_LOCATION_RSX;
        sf.colorOffset[1]   = 0;
        sf.colorOffset[2]   = 0;
        sf.colorOffset[3]   = 0;
        sf.colorPitch[1]    = 64;
        sf.colorPitch[2]    = 64;
        sf.colorPitch[3]    = 64;
        sf.depthFormat      = GCM_SURFACE_ZETA_Z24S8;
        sf.depthLocation    = GCM_LOCATION_RSX;
        sf.depthOffset      = gDepthOffset;
        sf.depthPitch       = gDepthPitch;
        sf.type             = GCM_SURFACE_TYPE_LINEAR;
        sf.antiAlias        = GCM_SURFACE_CENTER_1;
        sf.width            = gDisplayWidth;
        sf.height           = gDisplayHeight;
        sf.x                = 0;
        sf.y                = 0;
        rsxSetSurface(gCtx, &sf);
    }

    // Fixed render state the clear needs (viewport/scissor/mask/depth). Without
    // this the clear region is undefined — RPCS3 painted only part of the screen
    // and then crashed its RSX present. Mirrors rsxutil/main.cpp's setDrawEnv.
    void SetDrawEnv()
    {
        rsxSetColorMask(gCtx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
        rsxSetColorMaskMrt(gCtx, 0);

        const u16 x = 0, y = 0, w = (u16)gDisplayWidth, h = (u16)gDisplayHeight;
        const f32 mn = 0.0f, mx = 1.0f;
        f32 scale[4]  = { w * 0.5f, h * -0.5f, (mx - mn) * 0.5f, 0.0f };
        f32 offset[4] = { x + w * 0.5f, y + h * 0.5f, (mx + mn) * 0.5f, 0.0f };
        rsxSetViewport(gCtx, x, y, w, h, mn, mx, scale, offset);
        rsxSetScissor(gCtx, x, y, w, h);

        rsxSetDepthTestEnable(gCtx, GCM_TRUE);
        rsxSetDepthFunc(gCtx, GCM_LESS);
        rsxSetShadeModel(gCtx, GCM_SHADE_MODEL_SMOOTH);
        rsxSetDepthWriteEnable(gCtx, 1);
        rsxSetFrontFace(gCtx, GCM_FRONTFACE_CCW);
    }

    // Clear the bound render target to the clear colour (colour + depth/stencil).
    void ClearScreen()
    {
        SetDrawEnv();
        rsxSetClearColor(gCtx, kClearColor);
        rsxSetClearDepthStencil(gCtx, 0xffffff00);
        rsxClearSurface(gCtx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
                              GCM_CLEAR_S | GCM_CLEAR_Z);
        rsxSetZMinMaxControl(gCtx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
        for (u32 i = 0; i < 8; ++i) rsxSetViewportClip(gCtx, i, gDisplayWidth, gDisplayHeight);
    }

    // Present the current buffer, then bind the next as the render target.
    // Mirrors rsxutil.cpp's flip().
    void Flip()
    {
        if (!gFirstFlip)
        {
            u32 spins = 0;
            while (gcmGetFlipStatus() != 0)
            {
                sysUsleep(200);
                if (++spins > 50000) break; // ~10 s guard against a stuck flip
            }
        }
        gcmResetFlipStatus();

        gcmSetFlip(gCtx, (u8)gCurrentBuffer);
        rsxFlushBuffer(gCtx);
        gcmSetWaitFlip(gCtx);

        gCurrentBuffer ^= 1;
        SetRenderTarget(gCurrentBuffer);
        gFirstFlip = 0;
    }
}

// =========================================================================
// Lifecycle + per-frame
// =========================================================================

void GFX_Initialize()
{
    if (gInitialized) return;
    LogDebug("[GFX] begin");

    // Host IO memory for the RSX command ring (+ any CPU->RSX uploads).
    gHostAddr = memalign(1024 * 1024, kHostSize);
    if (gHostAddr == nullptr) { LogError("GFX_Initialize: host alloc failed"); return; }

    const s32 rsxRc = rsxInit(&gCtx, kCommandBufferSize, kHostSize, gHostAddr);
    if (rsxRc != 0 || gCtx == nullptr) { LogError("GFX_Initialize: rsxInit failed rc=%d", (int)rsxRc); return; }
    LogDebug("[GFX] rsxInit ok (ctx=%p)", gCtx);

    // Pick the first AVAILABLE resolution, preferring 720p (progressive, matches
    // the engine's default window). videoGetResolutionAvailability is what the
    // PSL1GHT sample uses — picking the display's raw native mode gave an
    // anamorphic 1080 mode that half-cleared.
    static const u32 kResIds[] = { VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_960x1080,
                                   VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576 };
    videoResolution vres;
    s32 resId = 0;
    bool gotRes = false;
    for (u32 i = 0; i < sizeof(kResIds) / sizeof(kResIds[0]); ++i)
    {
        if (videoGetResolutionAvailability(VIDEO_PRIMARY, kResIds[i], VIDEO_ASPECT_AUTO, 0) != 1) continue;
        resId = (s32)kResIds[i];
        if (videoGetResolution(resId, &vres) == 0) { gotRes = true; break; }
    }
    if (!gotRes) { LogError("GFX_Initialize: no usable resolution"); return; }
    gDisplayWidth  = vres.width;
    gDisplayHeight = vres.height;
    gColorPitch    = gDisplayWidth * 4;
    gDepthPitch    = gDisplayWidth * 4;
    LogDebug("[GFX] resolution %ux%u (resId=%d)", (unsigned)gDisplayWidth, (unsigned)gDisplayHeight, (int)resId);

    videoConfiguration vcfg;
    memset(&vcfg, 0, sizeof(vcfg));
    vcfg.resolution = (u8)resId;
    vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.aspect     = VIDEO_ASPECT_AUTO;
    vcfg.pitch      = gColorPitch;
    if (videoConfigure(VIDEO_PRIMARY, &vcfg, NULL, 0) != 0) { LogError("GFX_Initialize: videoConfigure failed"); return; }
    LogDebug("[GFX] videoConfigure ok");

    WaitRSXIdle();
    gcmSetFlipMode(GCM_FLIP_VSYNC);

    // Framebuffers + depth buffer in RSX LOCAL memory.
    for (u32 i = 0; i < 2; ++i)
    {
        gColorBuffer[i] = (u32*)rsxMemalign(64, gDisplayHeight * gColorPitch);
        if (gColorBuffer[i] == nullptr) { LogError("GFX_Initialize: color %u alloc failed", (unsigned)i); return; }
        rsxAddressToOffset(gColorBuffer[i], &gColorOffset[i]);
        gcmSetDisplayBuffer(i, gColorOffset[i], gColorPitch, gDisplayWidth, gDisplayHeight);
    }
    gDepthBuffer = (u32*)rsxMemalign(64, gDisplayHeight * gDepthPitch);
    if (gDepthBuffer == nullptr) { LogError("GFX_Initialize: depth alloc failed"); return; }
    rsxAddressToOffset(gDepthBuffer, &gDepthOffset);
    LogDebug("[GFX] buffers allocated (color 0x%x/0x%x depth 0x%x)",
             (unsigned)gColorOffset[0], (unsigned)gColorOffset[1], (unsigned)gDepthOffset);

    gCurrentBuffer = 0;
    gFirstFlip     = 1;
    gInitialized   = true;
    GetEngineState()->mSystem.mRsxContext = gCtx;

    // Set up the initial render target + draw env, then clear + present once so
    // the cleared colour shows immediately (this works — the boot gets well past
    // it into script execution).
    SetDrawEnv();
    SetRenderTarget(gCurrentBuffer);
    ClearScreen();
    Flip();

    LogDebug("GFX_Initialize: RSX up at %ux%u", (unsigned)gDisplayWidth, (unsigned)gDisplayHeight);
}

void GFX_Shutdown()
{
    // PSL1GHT exposes no rsxFinish here; a full teardown would drain the last
    // flip. For Phase 1 just drop our references — the process is exiting.
    gCtx = nullptr;
    if (gHostAddr != nullptr)
    {
        free(gHostAddr);
        gHostAddr = nullptr;
    }
    gInitialized = false;
    GetEngineState()->mSystem.mRsxContext = nullptr;
}

void GFX_BeginFrame()
{
    // Pump the PS3 system-utility callback queue once per frame (exit request,
    // display/flip advancement). The PSL1GHT sample loop calls this every frame;
    // without it RPCS3's display subsystem can back up and crash after the first
    // flip. Safe before RSX is up (no-op then).
    sysUtilCheckCallback();
}

void GFX_EndFrame()
{
    if (!gInitialized) return;

    // Phase 1: pump system callbacks, clear the back buffer, present. Mirrors the
    // PSL1GHT sample's per-frame "sysUtilCheckCallback -> drawFrame -> flip". The
    // callback pump right before the flip is what keeps RPCS3's display subsystem
    // consistent. Real draws slot in before ClearScreen once RSX rendering lands.
    sysUtilCheckCallback();
    ClearScreen();
    Flip();

    GetEngineState()->mSystem.mFrameIndex++;
}

// =========================================================================
// Render-pass / view / pipeline scaffolding — no-ops for Phase 1.
// =========================================================================

void GFX_BeginScreen(uint32_t /*screenIndex*/) {}
void GFX_BeginView(uint32_t /*viewIndex*/) {}
bool GFX_ShouldCullLights() { return true; }
void GFX_BeginRenderPass(RenderPassId /*renderPassId*/) {}
void GFX_EndRenderPass() {}
void GFX_SetPipelineState(PipelineConfig /*config*/) {}
void GFX_SetViewport(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool /*handlePrerotation*/) {}
void GFX_SetScissor(int32_t /*x*/, int32_t /*y*/, int32_t /*w*/, int32_t /*h*/, bool /*handlePrerotation*/) {}

glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    return glm::perspective(glm::radians(fovyDegrees), aspectRatio, zNear, zFar);
}

glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    return glm::ortho(left, right, bottom, top, zNear, zFar);
}

void GFX_SetFog(const FogSettings& /*fogSettings*/) {}
void GFX_DrawLines(const std::vector<Line>& /*lines*/) {}
void GFX_DrawFullscreen() {}
void GFX_ResizeWindow() {}
void GFX_Reset() {}
Node3D* GFX_ProcessHitCheck(World* /*world*/, int32_t /*x*/, int32_t /*y*/, uint32_t* /*outInstance*/) { return nullptr; }
uint32_t GFX_GetNumViews() { return 1; }
void GFX_SetFrameRate(int32_t /*frameRate*/) {}
void GFX_PathTrace() {}
void GFX_BeginLightBake() {}
void GFX_UpdateLightBake() {}
void GFX_EndLightBake() {}
bool GFX_IsLightBakeInProgress() { return false; }
float GFX_GetLightBakeProgress() { return 0.0f; }
void GFX_EnableMaterials(bool /*enable*/) {}
void GFX_BeginGpuTimestamp(const char* /*name*/) {}
void GFX_EndGpuTimestamp(const char* /*name*/) {}

// =========================================================================
// Resource create / destroy / update / draw — all no-ops for Phase 1.
// =========================================================================

void GFX_CreateTextureResource(Texture* /*texture*/, std::vector<uint8_t>& /*data*/) {}
void GFX_DestroyTextureResource(Texture* /*texture*/) {}
void GFX_UpdateTextureResourcePixels(Texture* /*texture*/, const uint8_t* /*src*/, uint32_t /*srcWidth*/, uint32_t /*srcHeight*/) {}

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

void GFX_CreateStaticMeshResource(StaticMesh* /*staticMesh*/, bool /*hasColor*/, uint32_t /*numVertices*/, void* /*vertices*/, uint32_t /*numIndices*/, IndexType* /*indices*/) {}
void GFX_DestroyStaticMeshResource(StaticMesh* /*staticMesh*/) {}

void GFX_CreateSkeletalMeshResource(SkeletalMesh* /*skeletalMesh*/, uint32_t /*numVertices*/, VertexSkinned* /*vertices*/, uint32_t /*numIndices*/, IndexType* /*indices*/) {}
void GFX_DestroySkeletalMeshResource(SkeletalMesh* /*skeletalMesh*/) {}

void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_DrawStaticMeshComp(StaticMesh3D* /*staticMeshComp*/, StaticMesh* /*meshOverride*/) {}

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*skeletalMeshComp*/) {}
void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* /*skeletalMeshComp*/) {}
void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* /*skeletalMeshComp*/, uint32_t /*numVertices*/) {}
void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* /*skeletalMeshComp*/, const std::vector<Vertex>& /*skinnedVertices*/) {}
void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* /*skeletalMeshComp*/) {}
bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* /*skeletalMeshComp*/) { return true; }

void GFX_DrawShadowMeshComp(ShadowMesh3D* /*shadowMeshComp*/) {}
void GFX_DrawInstancedMeshComp(InstancedMesh3D* /*instancedMeshComp*/) {}

void GFX_CreateTextMeshCompResource(TextMesh3D* /*textMeshComp*/) {}
void GFX_DestroyTextMeshCompResource(TextMesh3D* /*textMeshComp*/) {}
void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* /*textMeshComp*/, const std::vector<Vertex>& /*vertices*/) {}
void GFX_DrawTextMeshComp(TextMesh3D* /*textMeshComp*/) {}

void GFX_CreateVoxel3DResource(Voxel3D* /*voxel*/) {}
void GFX_DestroyVoxel3DResource(Voxel3D* /*voxel*/) {}
void GFX_UpdateVoxel3DResource(Voxel3D* /*voxel*/, const std::vector<VertexColor>& /*vertices*/, const std::vector<IndexType>& /*indices*/) {}
void GFX_DrawVoxel3D(Voxel3D* /*voxel*/) {}

void GFX_CreateTerrain3DResource(Terrain3D* /*terrain*/) {}
void GFX_DestroyTerrain3DResource(Terrain3D* /*terrain*/) {}
void GFX_UpdateTerrain3DResource(Terrain3D* /*terrain*/, const std::vector<VertexColor>& /*vertices*/, const std::vector<IndexType>& /*indices*/) {}
void GFX_DrawTerrain3D(Terrain3D* /*terrain*/) {}

void GFX_CreateTileMap2DResource(TileMap2D* /*tileMap*/) {}
void GFX_DestroyTileMap2DResource(TileMap2D* /*tileMap*/) {}
void GFX_UpdateTileMap2DResource(TileMap2D* /*tileMap*/, const std::vector<VertexColor>& /*vertices*/, const std::vector<IndexType>& /*indices*/) {}
void GFX_DrawTileMap2D(TileMap2D* /*tileMap*/) {}

void GFX_CreateParticleCompResource(Particle3D* /*particleComp*/) {}
void GFX_DestroyParticleCompResource(Particle3D* /*particleComp*/) {}
void GFX_UpdateParticleCompVertexBuffer(Particle3D* /*particleComp*/, const std::vector<VertexParticle>& /*vertices*/) {}
void GFX_DrawParticleComp(Particle3D* /*particleComp*/) {}

void GFX_CreateQuadResource(Quad* /*quad*/) {}
void GFX_DestroyQuadResource(Quad* /*quad*/) {}
void GFX_UpdateQuadResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuad(Quad* /*quad*/) {}

void GFX_CreateQuadBorderResource(Quad* /*quad*/) {}
void GFX_DestroyQuadBorderResource(Quad* /*quad*/) {}
void GFX_UpdateQuadBorderResourceVertexData(Quad* /*quad*/) {}
void GFX_DrawQuadBorder(Quad* /*quad*/) {}

void GFX_CreateTextResource(Text* /*text*/) {}
void GFX_DestroyTextResource(Text* /*text*/) {}
void GFX_UpdateTextResourceVertexData(Text* /*text*/) {}
void GFX_DrawText(Text* /*text*/) {}

void GFX_CreatePolyResource(Poly* /*poly*/) {}
void GFX_DestroyPolyResource(Poly* /*poly*/) {}
void GFX_UpdatePolyResourceVertexData(Poly* /*poly*/) {}
void GFX_DrawPoly(Poly* /*poly*/) {}

void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}
void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON

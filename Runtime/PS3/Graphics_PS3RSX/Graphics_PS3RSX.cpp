/**
 * @file Graphics_PS3RSX.cpp
 * @brief PS3 graphics backend (PSL1GHT / RSX) — Phase 2: real rendering.
 *
 * Phase 1 brought up video + an RSX context and cleared/flipped a double-
 * buffered framebuffer so the engine booted to a solid screen. Phase 2 adds
 * actual draws on top of that surface:
 *
 *   - Cg vertex+fragment programs (compiled offline by shaders/gen_shaders.sh,
 *     embedded via shaders/Ps3Shaders_gen.h): a screen-space UI program and a
 *     single-directional-light mesh program.
 *   - Static-mesh resources: engine Vertex / VertexColor uploaded verbatim to
 *     RSX-mapped memory (shared pos@0 / uv@12 / normal@28 layout), 16-bit
 *     indexed draws with the per-object MVP + model matrices as vertex uniforms.
 *   - Textures: RGBA8 → A8R8G8B8 linear, NPOT-padded with Texture::SetUVMax.
 *   - 2D UI (Quad / QuadBorder / Text / Poly): VertexUI repacked to a float
 *     RSX vertex (endianness-safe colour unpack) and drawn with a top-left
 *     orthographic projection, alpha blending, depth off.
 *
 * The camera view/proj come from the active Camera3D at BeginRenderPass(Forward)
 * exactly like the PSP backend. The remaining GFX_* surface (skeletal, particle,
 * voxel, terrain, tilemap, instanced, text-mesh, shadow, post-process) is still
 * stubbed and slots in on top of this foundation in later phases.
 *
 * RSX facts learned the hard way (see project memory): framebuffers/vertex/
 * index/texture buffers live in rsxMemalign memory; offsets are derived with
 * rsxAddressToOffset at draw time (so no engine resource-struct changes are
 * needed — the shared POLYPHASE_PLATFORM_ADDON arm's void* fields hold the CPU
 * pointer). SetDrawEnv (viewport/scissor/mask/depth) must precede the clear.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsConstants.h"
#include "Engine.h"
#include "Engine/Maths.h"
#include "Engine/Renderer.h"
#include "Engine/World.h"
#include "Engine/Assets/Material.h"
#include "Engine/Assets/MaterialLite.h"
#include "Engine/Assets/StaticMesh.h"
#include "Engine/Assets/SkeletalMesh.h"
#include "Engine/Assets/Texture.h"
#include "Engine/Assets/Font.h"
#include "Engine/Nodes/3D/StaticMesh3d.h"
#include "Engine/Nodes/3D/SkeletalMesh3d.h"
#include "Engine/Nodes/3D/Particle3d.h"
#include "Engine/Nodes/3D/Camera3d.h"
#include "Engine/Nodes/Widgets/Widget.h"
#include "Engine/Nodes/Widgets/Quad.h"
#include "Engine/Nodes/Widgets/Text.h"
#include "Engine/Nodes/Widgets/Poly.h"
#include "Log.h"

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>
#include <sysutil/sysutil.h>
#include <sys/systime.h>

#include <glm/matrix.hpp>

#include <malloc.h>
#include <string.h>

#include "shaders/Ps3Shaders_gen.h"

namespace
{
    // ---- RSX / video state (Phase 1) ------------------------------------
    constexpr u32 kCommandBufferSize = 0x80000;         // 512 KB
    // RSX memory pool: rsxMemalign (framebuffers, depth, ALL vertex/index/
    // texture buffers) allocates from this rsxInit host region. Framebuffers +
    // depth already take ~10 MB at 720p; a real scene's meshes + textures (e.g.
    // a 2 MB texture atlas) blow past a 16 MB pool, so rsxMemalign starts
    // returning null/garbage and the resulting bad RSX commands hard-crash
    // RPCS3. The PSL1GHT rsxtest sample sizes this at 128 MB — match it.
    constexpr u32 kHostSize          = 128 * 1024 * 1024;
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

    // Master enables for the two draw families (mesh vs UI). Kept as simple
    // switches for quick A/B testing; both on for normal rendering.
    bool  gDrawMeshEnabled = true;
    bool  gDrawUIEnabled   = true;

    // ---- Shader programs (Phase 2) --------------------------------------
    rsxVertexProgram*   gUiVp        = nullptr;
    void*               gUiVpUcode   = nullptr;
    rsxFragmentProgram* gUiFp        = nullptr;
    u32*                gUiFpBuffer  = nullptr;   // rsxMemalign copy of fp ucode
    u32                 gUiFpOffset  = 0;
    rsxProgramConst*    gUiOrtho     = nullptr;
    rsxProgramConst*    gUiTint      = nullptr;
    rsxProgramAttrib*   gUiTexUnit   = nullptr;

    rsxVertexProgram*   gMeshVp        = nullptr;
    void*               gMeshVpUcode   = nullptr;
    rsxFragmentProgram* gMeshFp        = nullptr;
    u32*                gMeshFpBuffer  = nullptr;
    u32                 gMeshFpOffset  = 0;
    rsxProgramConst*    gMeshMvp       = nullptr;
    rsxProgramConst*    gMeshModel     = nullptr;
    rsxProgramConst*    gMeshMatColor  = nullptr;
    rsxProgramConst*    gMeshLightDir  = nullptr;
    rsxProgramConst*    gMeshLightCol  = nullptr;
    rsxProgramConst*    gMeshAmbient   = nullptr;
    rsxProgramConst*    gMeshFogColor  = nullptr;
    rsxProgramConst*    gMeshFogParams = nullptr;
    rsxProgramConst*    gMeshCamPos    = nullptr;
    rsxProgramAttrib*   gMeshTexUnit   = nullptr;

    // Particle program (world-space billboard quads, camera MVP, vertex colour).
    rsxVertexProgram*   gPartVp        = nullptr;
    void*               gPartVpUcode   = nullptr;
    rsxFragmentProgram* gPartFp        = nullptr;
    u32*                gPartFpBuffer  = nullptr;
    u32                 gPartFpOffset  = 0;
    rsxProgramConst*    gPartMvp       = nullptr;
    rsxProgramConst*    gPartTint      = nullptr;
    rsxProgramAttrib*   gPartTexUnit   = nullptr;

    // 1x1 white texture bound when a draw has no texture (borders, untextured
    // materials) so the sampler multiply is a no-op.
    u32* gWhiteTex       = nullptr;
    u32  gWhiteTexOffset = 0;

    // Cached per-frame scene state (set at BeginRenderPass(Forward)).
    glm::mat4 gViewMatrix = glm::mat4(1.0f);
    glm::mat4 gProjMatrix = glm::mat4(1.0f);
    glm::vec3 gLightDir   = glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f));
    glm::vec3 gLightColor = glm::vec3(1.0f);
    glm::vec3 gAmbient    = glm::vec3(0.35f);
    glm::vec3 gCamPos     = glm::vec3(0.0f);

    // Fog state (set by GFX_SetFog, uploaded to the mesh fragment shader).
    // gFogParams = (near, far, enabled, densityFunc[0=linear,1=exp]).
    glm::vec4 gFogColor  = glm::vec4(0.0f);
    glm::vec4 gFogParams = glm::vec4(0.0f, 100.0f, 0.0f, 0.0f);

    // RSX particle vertex — floats throughout (endian-safe colour). 36 bytes.
    struct RsxPartVertex
    {
        float x, y, z;
        float u, v;
        float r, g, b, a;
    };

    // Repacked RSX UI vertex — floats throughout so there is no uint32 colour
    // endianness ambiguity on the big-endian PPU. 32 bytes.
    struct RsxUIVertex
    {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    // ---- RSX sync + surface (Phase 1) -----------------------------------
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

    // Full-display viewport + fixed clear-friendly state.
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

    // Sub-rect viewport (from the engine's per-view GFX_SetViewport), clamped to
    // the physical framebuffer.
    void SetViewportRSX(int32_t vx, int32_t vy, int32_t vw, int32_t vh)
    {
        if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = (int32_t)gDisplayWidth; vh = (int32_t)gDisplayHeight; }
        if (vx < 0) vx = 0;
        if (vy < 0) vy = 0;
        if (vx + vw > (int32_t)gDisplayWidth)  vw = (int32_t)gDisplayWidth  - vx;
        if (vy + vh > (int32_t)gDisplayHeight) vh = (int32_t)gDisplayHeight - vy;
        if (vw <= 0 || vh <= 0) { vx = 0; vy = 0; vw = (int32_t)gDisplayWidth; vh = (int32_t)gDisplayHeight; }

        const u16 x = (u16)vx, y = (u16)vy, w = (u16)vw, h = (u16)vh;
        const f32 mn = 0.0f, mx = 1.0f;
        f32 scale[4]  = { w * 0.5f, h * -0.5f, (mx - mn) * 0.5f, 0.0f };
        f32 offset[4] = { x + w * 0.5f, y + h * 0.5f, (mx + mn) * 0.5f, 0.0f };
        rsxSetViewport(gCtx, x, y, w, h, mn, mx, scale, offset);
        rsxSetScissor(gCtx, x, y, w, h);
    }

    void ClearScreen(u32 clearColor)
    {
        SetDrawEnv();
        rsxSetClearColor(gCtx, clearColor);
        rsxSetClearDepthStencil(gCtx, 0xffffff00);
        rsxClearSurface(gCtx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
                              GCM_CLEAR_S | GCM_CLEAR_Z);
        rsxSetZMinMaxControl(gCtx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
        for (u32 i = 0; i < 8; ++i) rsxSetViewportClip(gCtx, i, gDisplayWidth, gDisplayHeight);
    }

    void Flip()
    {
        if (!gFirstFlip)
        {
            u32 spins = 0;
            while (gcmGetFlipStatus() != 0)
            {
                sysUsleep(200);
                if (++spins > 50000) break;
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

    // RPCS3 poisons fresh memory with 0xABADCAFE. __wrap_malloc zeroes malloc/
    // new, but rsxMemalign (RSX/GPU pool) is NOT wrapped, so its buffers come
    // back poisoned. A buffer that is allocated but not fully written is then
    // read/drawn as poison — garbage geometry, and poison interpreted as a
    // pointer/index is a wild write. Zero every RSX allocation, same mitigation
    // the heap gets. Route ALL rsxMemalign through here.
    void* RsxZAlloc(u32 align, u32 size)
    {
        void* p = rsxMemalign(align, size);
        if (p != nullptr) memset(p, 0, size);
        return p;
    }

    // ---- Shaders + textures (Phase 2) -----------------------------------

    void LoadShaders()
    {
        // UI program.
        gUiVp = (rsxVertexProgram*)ps3_ui_vpo;
        gUiFp = (rsxFragmentProgram*)ps3_ui_fpo;
        u32 sz = 0;
        rsxVertexProgramGetUCode(gUiVp, &gUiVpUcode, &sz);
        gUiOrtho   = rsxVertexProgramGetConst(gUiVp, "orthoMatrix");

        void* uiFpUcode = nullptr; u32 uiFpSize = 0;
        rsxFragmentProgramGetUCode(gUiFp, &uiFpUcode, &uiFpSize);
        gUiFpBuffer = (u32*)RsxZAlloc(64, uiFpSize);
        memcpy(gUiFpBuffer, uiFpUcode, uiFpSize);
        rsxAddressToOffset(gUiFpBuffer, &gUiFpOffset);
        gUiTexUnit = rsxFragmentProgramGetAttrib(gUiFp, "texture");
        gUiTint    = rsxFragmentProgramGetConst(gUiFp, "tint");

        // Mesh program.
        gMeshVp = (rsxVertexProgram*)ps3_mesh_vpo;
        gMeshFp = (rsxFragmentProgram*)ps3_mesh_fpo;
        rsxVertexProgramGetUCode(gMeshVp, &gMeshVpUcode, &sz);
        gMeshMvp   = rsxVertexProgramGetConst(gMeshVp, "mvpMatrix");
        gMeshModel = rsxVertexProgramGetConst(gMeshVp, "modelMatrix");

        void* meshFpUcode = nullptr; u32 meshFpSize = 0;
        rsxFragmentProgramGetUCode(gMeshFp, &meshFpUcode, &meshFpSize);
        gMeshFpBuffer = (u32*)RsxZAlloc(64, meshFpSize);
        memcpy(gMeshFpBuffer, meshFpUcode, meshFpSize);
        rsxAddressToOffset(gMeshFpBuffer, &gMeshFpOffset);
        gMeshTexUnit  = rsxFragmentProgramGetAttrib(gMeshFp, "texture");
        gMeshMatColor = rsxFragmentProgramGetConst(gMeshFp, "matColor");
        gMeshLightDir = rsxFragmentProgramGetConst(gMeshFp, "lightDir");
        gMeshLightCol = rsxFragmentProgramGetConst(gMeshFp, "lightColor");
        gMeshAmbient  = rsxFragmentProgramGetConst(gMeshFp, "ambient");
        gMeshFogColor = rsxFragmentProgramGetConst(gMeshFp, "fogColor");
        gMeshFogParams= rsxFragmentProgramGetConst(gMeshFp, "fogParams");
        gMeshCamPos   = rsxFragmentProgramGetConst(gMeshFp, "camPos");

        // Particle program.
        gPartVp = (rsxVertexProgram*)ps3_particle_vpo;
        gPartFp = (rsxFragmentProgram*)ps3_particle_fpo;
        rsxVertexProgramGetUCode(gPartVp, &gPartVpUcode, &sz);
        gPartMvp = rsxVertexProgramGetConst(gPartVp, "mvpMatrix");

        void* partFpUcode = nullptr; u32 partFpSize = 0;
        rsxFragmentProgramGetUCode(gPartFp, &partFpUcode, &partFpSize);
        gPartFpBuffer = (u32*)RsxZAlloc(64, partFpSize);
        memcpy(gPartFpBuffer, partFpUcode, partFpSize);
        rsxAddressToOffset(gPartFpBuffer, &gPartFpOffset);
        gPartTexUnit = rsxFragmentProgramGetAttrib(gPartFp, "texture");
        gPartTint    = rsxFragmentProgramGetConst(gPartFp, "tint");

        LogDebug("[GFX] shaders loaded (ui fp=%uB mesh fp=%uB part fp=%uB)",
                 (unsigned)uiFpSize, (unsigned)meshFpSize, (unsigned)partFpSize);
    }

    void MakeWhiteTexture()
    {
        gWhiteTex = (u32*)RsxZAlloc(128, 4 * 4 * 4);
        if (gWhiteTex == nullptr) return;
        for (u32 i = 0; i < 16; ++i) gWhiteTex[i] = 0xFFFFFFFFu;   // ARGB white
        rsxAddressToOffset(gWhiteTex, &gWhiteTexOffset);
    }

    // Bind an engine texture (or the white fallback) to a fragment sampler unit.
    void BindTextureUnit(Texture* tex, u8 unit)
    {
        rsxInvalidateTextureCache(gCtx, GCM_INVALIDATE_TEXTURE);

        u32   offset = gWhiteTexOffset;
        u32   w = 4, h = 4, pitch = 16;
        bool  linear = true;

        if (tex != nullptr)
        {
            TextureResource* r = tex->GetResource();
            if (r != nullptr && r->mPixels != nullptr && r->mWidth > 0 && r->mHeight > 0)
            {
                rsxAddressToOffset(r->mPixels, &offset);
                w     = r->mWidth;
                h     = r->mHeight;
                pitch = r->mBufWidth * 4;
                linear = (r->mSwizzled == 0);
            }
        }

        gcmTexture t;
        t.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 | (linear ? GCM_TEXTURE_FORMAT_LIN : 0);
        t.mipmap    = 1;
        t.dimension = GCM_TEXTURE_DIMS_2D;
        t.cubemap   = GCM_FALSE;
        t.remap     = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
                       (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
                       (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
                       (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
                       (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
                       (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
                       (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
                       (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
        t.width     = (u16)w;
        t.height    = (u16)h;
        t.depth     = 1;
        t.location  = GCM_LOCATION_RSX;
        t.pitch     = pitch;
        t.offset    = offset;
        rsxLoadTexture(gCtx, unit, &t);
        rsxTextureControl(gCtx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
        rsxTextureFilter(gCtx, unit, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        rsxTextureWrapMode(gCtx, unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                           GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
    }

    // Upload a glm matrix transposed (glm is column-major; Cg mul() treats the
    // uniform as row-major, so mul(M,v) == glm_M * v only after transpose).
    void SetVertexMatrix(rsxVertexProgram* vp, rsxProgramConst* c, const glm::mat4& m)
    {
        if (c == nullptr) return;
        const glm::mat4 t = glm::transpose(m);
        rsxSetVertexProgramParameter(gCtx, vp, c, (const f32*)&t[0][0]);
    }
    void SetFragmentVec3(rsxFragmentProgram* fp, rsxProgramConst* c, u32 fpOffset, const glm::vec3& v)
    {
        if (c == nullptr) return;
        rsxSetFragmentProgramParameter(gCtx, fp, c, (const f32*)&v.x, fpOffset, GCM_LOCATION_RSX);
    }
    void SetFragmentVec4(rsxFragmentProgram* fp, rsxProgramConst* c, u32 fpOffset, const glm::vec4& v)
    {
        if (c == nullptr) return;
        rsxSetFragmentProgramParameter(gCtx, fp, c, (const f32*)&v.x, fpOffset, GCM_LOCATION_RSX);
    }

    void UploadCameraMatrices()
    {
        Renderer* renderer = Renderer::Get();
        if (renderer == nullptr) return;
        World* world = renderer->GetCurrentWorld();
        if (world == nullptr) return;
        Camera3D* cam = world->GetActiveCamera();
        if (cam == nullptr) return;
        gViewMatrix = cam->GetViewMatrix();
        gProjMatrix = cam->GetProjectionMatrix();
        // Camera world position = inverse(view) translation. Robust regardless
        // of the Node3D world-position accessor name; used for fog distance.
        gCamPos = glm::vec3(glm::inverse(gViewMatrix)[3]);
    }

    void UploadLightData()
    {
        // Defaults (used when the scene has no directional light).
        gLightDir   = glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f));
        gLightColor = glm::vec3(1.0f);
        gAmbient    = glm::vec3(0.35f);

        Renderer* renderer = Renderer::Get();
        World* world = renderer ? renderer->GetCurrentWorld() : nullptr;
        if (world == nullptr) return;

        const glm::vec4 amb = world->GetAmbientLightColor();
        gAmbient = glm::vec3(amb.r, amb.g, amb.b);

        const std::vector<LightData>& lights = renderer->GetLightData();
        for (const LightData& ld : lights)
        {
            if (ld.mType == LightType::Directional)
            {
                gLightDir   = glm::normalize(ld.mDirection);
                gLightColor = glm::vec3(ld.mColor.r, ld.mColor.g, ld.mColor.b) * ld.mIntensity;
                break;
            }
        }
    }

    void SetupMeshPipeline()
    {
        rsxSetBlendEnable(gCtx, GCM_FALSE);
        rsxSetDepthTestEnable(gCtx, GCM_TRUE);
        rsxSetDepthWriteEnable(gCtx, GCM_TRUE);
        rsxSetDepthFunc(gCtx, GCM_LESS);
        rsxSetCullFaceEnable(gCtx, GCM_FALSE);   // ignore winding for the first pass
        rsxSetUserClipPlaneControl(gCtx,
            GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
            GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
    }

    void SetupUIPipeline()
    {
        rsxSetBlendEnable(gCtx, GCM_TRUE);
        rsxSetBlendFunc(gCtx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
        rsxSetBlendEquation(gCtx, GCM_FUNC_ADD, GCM_FUNC_ADD);
        rsxSetDepthTestEnable(gCtx, GCM_FALSE);
        rsxSetDepthWriteEnable(gCtx, GCM_FALSE);
        rsxSetCullFaceEnable(gCtx, GCM_FALSE);
    }

    // Top-left-origin orthographic projection over the logical window.
    glm::mat4 UIOrtho()
    {
        Renderer* r = Renderer::Get();
        float w = r ? (float)r->GetViewportWidth()  : (float)gDisplayWidth;
        float h = r ? (float)r->GetViewportHeight() : (float)gDisplayHeight;
        if (w <= 0.0f) w = (float)gDisplayWidth;
        if (h <= 0.0f) h = (float)gDisplayHeight;
        return glm::ortho(0.0f, w, h, 0.0f, -1.0f, 1.0f);
    }

    // Expand a widget's 2D affine transform (glm::mat3: rotate-around-pivot +
    // translation) into a 4x4 model matrix for the UI vertex shader. Identity
    // when the widget has no rotation.
    glm::mat4 WidgetModel(Widget* w)
    {
        const glm::mat3& t = w->GetTransform();
        glm::mat4 m(1.0f);
        m[0][0] = t[0][0]; m[0][1] = t[0][1];
        m[1][0] = t[1][0]; m[1][1] = t[1][1];
        m[3][0] = t[2][0]; m[3][1] = t[2][1];
        return m;
    }

    void RepackUI(const VertexUI* src, uint32_t n, RsxUIVertex* dst)
    {
        for (uint32_t i = 0; i < n; ++i)
        {
            dst[i].x = src[i].mPosition.x;
            dst[i].y = src[i].mPosition.y;
            dst[i].u = src[i].mTexcoord.x;
            dst[i].v = src[i].mTexcoord.y;
            const uint32_t c = src[i].mColor;   // engine packs R in the low byte
            dst[i].r = float(c & 0xFF)         / 255.0f;
            dst[i].g = float((c >> 8)  & 0xFF) / 255.0f;
            dst[i].b = float((c >> 16) & 0xFF) / 255.0f;
            dst[i].a = float((c >> 24) & 0xFF) / 255.0f;
        }
    }

    void BindUIAttribs(const RsxUIVertex* buf)
    {
        // Defensively disable every vertex attribute array first. A prior mesh
        // draw leaves NORMAL(2) enabled into the small mesh buffer; any other
        // stale array would likewise be fetched OOB by rsxDrawVertexArray for
        // all n UI verts → RSX GPU fault (hard-crashes RPCS3). size/stride 0 =
        // attribute off. We then enable only POS/TEX0/COLOR0 below.
        for (u8 a = 0; a < 16; ++a)
            rsxBindVertexArrayAttrib(gCtx, a, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

        u32 off;
        rsxAddressToOffset((void*)&buf[0].x, &off);
        rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_POS, 0, off, sizeof(RsxUIVertex), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        rsxAddressToOffset((void*)&buf[0].u, &off);
        rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_TEX0, 0, off, sizeof(RsxUIVertex), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        rsxAddressToOffset((void*)&buf[0].r, &off);
        rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_COLOR0, 0, off, sizeof(RsxUIVertex), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        // A prior mesh draw leaves NORMAL(attr 2) enabled, pointing into the
        // (small) mesh vertex buffer. rsxDrawVertexArray fetches every enabled
        // attribute for all n vertices regardless of what the UI vertex program
        // reads, so the stale NORMAL is read OOB past the mesh buffer → RSX GPU
        // fault (hard-crashes RPCS3). Disable it (size/stride 0 = attribute off).
        rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_NORMAL, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    }

    // Common UI draw: bind program + ortho*model + tint + texture + attribs.
    // The model matrix carries the widget's rotation/position so the shared
    // vertex buffer is NEVER mutated per-draw (RSX reads it asynchronously, so
    // an in-place transform + undo would be read AFTER the undo → wrong pos).
    void DrawUI(const RsxUIVertex* buf, uint32_t n, u32 prim, const glm::vec4& tint, Texture* tex,
                const glm::mat4& model = glm::mat4(1.0f))
    {
        if (!gDrawUIEnabled) return;
        if (buf == nullptr || n == 0) return;

        SetupUIPipeline();
        rsxLoadVertexProgram(gCtx, gUiVp, gUiVpUcode);
        SetVertexMatrix(gUiVp, gUiOrtho, UIOrtho() * model);

        SetFragmentVec4(gUiFp, gUiTint, gUiFpOffset, tint);
        BindTextureUnit(tex, gUiTexUnit ? (u8)gUiTexUnit->index : 0);
        rsxLoadFragmentProgramLocation(gCtx, gUiFp, gUiFpOffset, GCM_LOCATION_RSX);

        BindUIAttribs(buf);
        rsxDrawVertexArray(gCtx, prim, 0, n);
    }

    // Allocate/grow an RSX-mapped UI vertex buffer to hold `bytes`.
    void EnsureUIBuffer(void** data, uint32_t* cap, uint32_t bytes)
    {
        if (*data != nullptr && *cap >= bytes) return;
        if (*data != nullptr) { rsxFree(*data); *data = nullptr; }
        *data = RsxZAlloc(128, bytes);
        *cap  = (*data != nullptr) ? bytes : 0;
    }
}

// =========================================================================
// Lifecycle + per-frame
// =========================================================================

void GFX_Initialize()
{
    if (gInitialized) return;
    LogDebug("[GFX] begin");

    gHostAddr = memalign(1024 * 1024, kHostSize);
    if (gHostAddr == nullptr) { LogError("GFX_Initialize: host alloc failed"); return; }

    const s32 rsxRc = rsxInit(&gCtx, kCommandBufferSize, kHostSize, gHostAddr);
    if (rsxRc != 0 || gCtx == nullptr) { LogError("GFX_Initialize: rsxInit failed rc=%d", (int)rsxRc); return; }
    LogDebug("[GFX] rsxInit ok (ctx=%p)", gCtx);

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

    for (u32 i = 0; i < 2; ++i)
    {
        gColorBuffer[i] = (u32*)RsxZAlloc(64, gDisplayHeight * gColorPitch);
        if (gColorBuffer[i] == nullptr) { LogError("GFX_Initialize: color %u alloc failed", (unsigned)i); return; }
        rsxAddressToOffset(gColorBuffer[i], &gColorOffset[i]);
        gcmSetDisplayBuffer(i, gColorOffset[i], gColorPitch, gDisplayWidth, gDisplayHeight);
    }
    gDepthBuffer = (u32*)RsxZAlloc(64, gDisplayHeight * gDepthPitch);
    if (gDepthBuffer == nullptr) { LogError("GFX_Initialize: depth alloc failed"); return; }
    rsxAddressToOffset(gDepthBuffer, &gDepthOffset);

    gCurrentBuffer = 0;
    gFirstFlip     = 1;

    LoadShaders();
    MakeWhiteTexture();

    gInitialized   = true;
    GetEngineState()->mSystem.mRsxContext = gCtx;

    SetDrawEnv();
    SetRenderTarget(gCurrentBuffer);
    ClearScreen(0x00203040u);
    Flip();

    LogDebug("GFX_Initialize: RSX up at %ux%u", (unsigned)gDisplayWidth, (unsigned)gDisplayHeight);
}

void GFX_Shutdown()
{
    gCtx = nullptr;
    if (gHostAddr != nullptr) { free(gHostAddr); gHostAddr = nullptr; }
    gInitialized = false;
    GetEngineState()->mSystem.mRsxContext = nullptr;
}

void GFX_BeginFrame()
{
    // Pump the PS3 system-utility callback queue (exit request, display/flip
    // advancement). Must run every frame or RPCS3's display subsystem backs up.
    sysUtilCheckCallback();
    if (!gInitialized) return;

    // Clear the back buffer to the engine's clear colour, then leave 3D state
    // ready for the Forward pass. Real draws land between here and EndFrame.
    glm::vec4 cc = Renderer::Get() ? Renderer::Get()->GetClearColor() : glm::vec4(0, 0, 0, 1);
    const u8 cr = (u8)(glm::clamp(cc.r, 0.0f, 1.0f) * 255.0f);
    const u8 cg = (u8)(glm::clamp(cc.g, 0.0f, 1.0f) * 255.0f);
    const u8 cb = (u8)(glm::clamp(cc.b, 0.0f, 1.0f) * 255.0f);
    const u32 clearColor = ((u32)cr << 16) | ((u32)cg << 8) | (u32)cb;   // XRGB

    SetRenderTarget(gCurrentBuffer);
    ClearScreen(clearColor);
}

void GFX_EndFrame()
{
    if (!gInitialized) return;
    sysUtilCheckCallback();
    Flip();
    GetEngineState()->mSystem.mFrameIndex++;
}

// =========================================================================
// Render-pass / view / pipeline
// =========================================================================

void GFX_BeginScreen(uint32_t /*screenIndex*/) {}
void GFX_BeginView(uint32_t /*viewIndex*/) {}
bool GFX_ShouldCullLights() { return true; }

void GFX_BeginRenderPass(RenderPassId renderPassId)
{
    if (!gInitialized) return;
    if (renderPassId == RenderPassId::Forward)
    {
        UploadCameraMatrices();
        UploadLightData();
        SetupMeshPipeline();
    }
    else if (renderPassId == RenderPassId::Ui)
    {
        SetupUIPipeline();
    }
}
void GFX_EndRenderPass() {}

void GFX_SetPipelineState(PipelineConfig config)
{
    if (!gInitialized) return;
    switch (config)
    {
        case PipelineConfig::Translucent:
        case PipelineConfig::Additive:
            rsxSetBlendEnable(gCtx, GCM_TRUE);
            rsxSetBlendFunc(gCtx,
                            GCM_SRC_ALPHA,
                            (config == PipelineConfig::Additive) ? GCM_ONE : GCM_ONE_MINUS_SRC_ALPHA,
                            GCM_SRC_ALPHA,
                            (config == PipelineConfig::Additive) ? GCM_ONE : GCM_ONE_MINUS_SRC_ALPHA);
            rsxSetBlendEquation(gCtx, GCM_FUNC_ADD, GCM_FUNC_ADD);
            rsxSetDepthWriteEnable(gCtx, GCM_FALSE);
            break;
        case PipelineConfig::Forward:
        case PipelineConfig::Opaque:
        default:
            rsxSetBlendEnable(gCtx, GCM_FALSE);
            rsxSetDepthTestEnable(gCtx, GCM_TRUE);
            rsxSetDepthWriteEnable(gCtx, GCM_TRUE);
            break;
    }
}

void GFX_SetViewport(int32_t x, int32_t y, int32_t w, int32_t h, bool /*handlePrerotation*/)
{
    if (!gInitialized) return;
    SetViewportRSX(x, y, w, h);
}
void GFX_SetScissor(int32_t x, int32_t y, int32_t w, int32_t h, bool /*handlePrerotation*/)
{
    if (!gInitialized) return;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = (int32_t)gDisplayWidth; h = (int32_t)gDisplayHeight; }
    rsxSetScissor(gCtx, (u16)glm::max(0, x), (u16)glm::max(0, y),
                  (u16)glm::min((int32_t)gDisplayWidth, w), (u16)glm::min((int32_t)gDisplayHeight, h));
}

glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    return glm::perspective(glm::radians(fovyDegrees), aspectRatio, zNear, zFar);
}
glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    return glm::ortho(left, right, bottom, top, zNear, zFar);
}

void GFX_SetFog(const FogSettings& fog)
{
    // Stored here; uploaded to the mesh fragment shader per draw. Distance is
    // world-space camera->fragment; near/far are world units. Linear vs
    // exponential falloff selected by densityFunc (enum: Linear=0, Exp=1).
    gFogColor  = fog.mColor;
    gFogParams = glm::vec4(fog.mNear, fog.mFar,
                           fog.mEnabled ? 1.0f : 0.0f,
                           float((int32_t)fog.mDensityFunc));
}
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
// Textures
// =========================================================================

void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& /*data*/)
{
    if (texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr) return;

    const uint32_t srcW = texture->GetWidth();
    const uint32_t srcH = texture->GetHeight();
    if (srcW == 0 || srcH == 0) return;

    const std::vector<uint8_t>& pixels = texture->GetPixels();
    if (pixels.empty()) return;

    // This backend only understands 32-bit RGBA8 (4 bytes/texel). If the cooked
    // pixel buffer is smaller than srcW*srcH*4 the texture is a different format
    // (RGB565 / compressed / etc.); reading it as RGBA8 would run off the end of
    // the vector and crash. Skip it (the sampler falls back to the white texel)
    // rather than fault. Real format support is a later phase.
    const size_t rgba8Bytes = (size_t)srcW * (size_t)srcH * 4u;
    if (pixels.size() < rgba8Bytes)
    {
        LogWarning("[GFX] texture '%s' is not RGBA8 (%u px bytes, need %u) — skipping (white fallback)",
                   texture->GetName().c_str(), (unsigned)pixels.size(), (unsigned)rgba8Bytes);
        return;
    }

    // Linear RSX textures need a pitch that is a multiple of 64 bytes → pad the
    // width up to a multiple of 16 texels. Height is unconstrained.
    const uint32_t bufW  = (srcW + 15u) & ~15u;
    const uint32_t bufH  = srcH;
    const uint32_t bytes = bufW * bufH * 4u;

    u32* dst = (u32*)RsxZAlloc(128, bytes);
    if (dst == nullptr) return;

    // RGBA8 (R,G,B,A byte order) → A8R8G8B8 (A,R,G,B byte order) for the RSX
    // sampler, padding the right edge by replicating the last source texel.
    const uint8_t* src = pixels.data();
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t y = 0; y < srcH; ++y)
    {
        const uint8_t* srow = src + y * srcW * 4;
        uint8_t*       drow = d   + y * bufW * 4;
        for (uint32_t x = 0; x < srcW; ++x)
        {
            drow[x * 4 + 0] = srow[x * 4 + 3]; // A
            drow[x * 4 + 1] = srow[x * 4 + 0]; // R
            drow[x * 4 + 2] = srow[x * 4 + 1]; // G
            drow[x * 4 + 3] = srow[x * 4 + 2]; // B
        }
        for (uint32_t x = srcW; x < bufW; ++x)
            ((u32*)drow)[x] = ((u32*)drow)[srcW - 1];
    }

    texture->SetUVMax(glm::vec2(float(srcW) / float(bufW), 1.0f));

    r->mPixels   = dst;
    r->mWidth    = bufW;
    r->mHeight   = bufH;
    r->mBufWidth = bufW;
    r->mPsm      = 0;
    r->mMipCount = 0;
    r->mSwizzled = 0;
}

void GFX_DestroyTextureResource(Texture* texture)
{
    if (texture == nullptr) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr) return;
    if (r->mPixels != nullptr) { rsxFree(r->mPixels); r->mPixels = nullptr; }
    r->mWidth = r->mHeight = r->mBufWidth = 0;
}

void GFX_UpdateTextureResourcePixels(Texture* texture, const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight)
{
    if (texture == nullptr || src == nullptr || srcWidth == 0 || srcHeight == 0) return;
    TextureResource* r = texture->GetResource();
    if (r == nullptr || r->mPixels == nullptr || r->mBufWidth == 0) return;

    const uint32_t copyW = (srcWidth  < r->mWidth)  ? srcWidth  : r->mWidth;
    const uint32_t copyH = (srcHeight < r->mHeight) ? srcHeight : r->mHeight;
    uint8_t* d = (uint8_t*)r->mPixels;
    for (uint32_t y = 0; y < copyH; ++y)
    {
        const uint8_t* srow = src + y * srcWidth * 4;
        uint8_t*       drow = d   + y * r->mBufWidth * 4;
        for (uint32_t x = 0; x < copyW; ++x)
        {
            drow[x * 4 + 0] = srow[x * 4 + 3];
            drow[x * 4 + 1] = srow[x * 4 + 0];
            drow[x * 4 + 2] = srow[x * 4 + 1];
            drow[x * 4 + 3] = srow[x * 4 + 2];
        }
    }
}

// =========================================================================
// Materials (no persistent GPU resource — state is applied per-draw)
// =========================================================================

void GFX_CreateMaterialResource(Material* /*material*/) {}
void GFX_DestroyMaterialResource(Material* /*material*/) {}

// =========================================================================
// Static meshes
// =========================================================================

void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor, uint32_t numVertices, void* vertices, uint32_t numIndices, IndexType* indices)
{
    if (staticMesh == nullptr || vertices == nullptr || indices == nullptr) return;
    if (numVertices == 0 || numIndices == 0) return;

    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr) return;

    // Vertex and VertexColor share pos@0 / uv0@12 / normal@28; upload verbatim.
    const uint32_t stride = hasColor ? sizeof(VertexColor) : sizeof(Vertex);
    const uint32_t vBytes = stride * numVertices;
    const uint32_t iBytes = sizeof(IndexType) * numIndices;

    void* vBuf = RsxZAlloc(128, vBytes);
    void* iBuf = RsxZAlloc(128, iBytes);
    if (vBuf == nullptr || iBuf == nullptr)
    {
        if (vBuf) rsxFree(vBuf);
        if (iBuf) rsxFree(iBuf);
        return;
    }
    memcpy(vBuf, vertices, vBytes);
    memcpy(iBuf, indices, iBytes);

    r->mVertexData   = vBuf;
    r->mIndexData    = iBuf;
    r->mNumVertices  = numVertices;
    r->mNumIndices   = numIndices;
    r->mVertexStride = stride;
    r->mVertexFlags  = hasColor ? 1u : 0u;
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (staticMesh == nullptr) return;
    StaticMeshResource* r = staticMesh->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    if (r->mIndexData  != nullptr) { rsxFree(r->mIndexData);  r->mIndexData  = nullptr; }
    r->mNumVertices = r->mNumIndices = r->mVertexStride = r->mVertexFlags = 0;
}

// Draw an indexed lit mesh (static OR CPU-skinned skeletal) with the mesh
// shader. vertexData/indexData are RSX-mapped; the skinned vertices are plain
// engine `Vertex` (pos@0/uv@12/normal@28), identical layout to static meshes,
// so both paths share this. matBase may be null (default material).
static void DrawLitMesh(void* vertexData, uint32_t stride, void* indexData, uint32_t numIndices,
                        Material* matBase, const glm::mat4& world)
{
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    Texture* tex = mat ? mat->GetTexture(0) : nullptr;

    glm::vec4 matColor(1.0f);
    bool unlit = false;
    int  fogMode = 0;   // 0=off, 1=distance, 2=sky/horizon (matches engine Vulkan path)
    if (mat != nullptr)
    {
        matColor = mat->GetColor();
        const BlendMode blend = mat->GetBlendMode();
        if (blend == BlendMode::Translucent || blend == BlendMode::Additive) matColor.a = mat->GetOpacity();
        unlit = (mat->GetShadingModel() == ShadingModel::Unlit);
        // The skybox is uniquely Unlit + depth-test-disabled + negative sort
        // priority → horizon fog; other fog materials get radial distance fog.
        if (gFogParams.z > 0.5f && mat->ShouldApplyFog())
        {
            const bool isSky = unlit && mat->IsDepthTestDisabled() && (mat->GetSortPriority() < 0);
            fogMode = isSky ? 2 : 1;
        }
    }

    SetupMeshPipeline();

    const glm::mat4 mvp = gProjMatrix * gViewMatrix * world;
    rsxLoadVertexProgram(gCtx, gMeshVp, gMeshVpUcode);
    SetVertexMatrix(gMeshVp, gMeshMvp, mvp);
    SetVertexMatrix(gMeshVp, gMeshModel, world);

    // fogParams = (near, far, mode[0/1/2], densityFunc); fogColor.a = intensity.
    const glm::vec4 fogParams(gFogParams.x, gFogParams.y, float(fogMode), gFogParams.w);

    SetFragmentVec4(gMeshFp, gMeshMatColor, gMeshFpOffset, matColor);
    SetFragmentVec3(gMeshFp, gMeshLightDir, gMeshFpOffset, gLightDir);
    SetFragmentVec3(gMeshFp, gMeshLightCol, gMeshFpOffset, unlit ? glm::vec3(0.0f) : gLightColor);
    SetFragmentVec3(gMeshFp, gMeshAmbient,  gMeshFpOffset, unlit ? glm::vec3(1.0f) : gAmbient);
    SetFragmentVec4(gMeshFp, gMeshFogColor, gMeshFpOffset, gFogColor);
    SetFragmentVec4(gMeshFp, gMeshFogParams, gMeshFpOffset, fogParams);
    SetFragmentVec3(gMeshFp, gMeshCamPos,   gMeshFpOffset, gCamPos);
    BindTextureUnit(tex, gMeshTexUnit ? (u8)gMeshTexUnit->index : 0);
    rsxLoadFragmentProgramLocation(gCtx, gMeshFp, gMeshFpOffset, GCM_LOCATION_RSX);

    const uint8_t* vbuf = (const uint8_t*)vertexData;
    u32 off;
    rsxAddressToOffset((void*)(vbuf + 0),  &off);
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_POS,    0, off, stride, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxAddressToOffset((void*)(vbuf + 12), &off);
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_TEX0,   0, off, stride, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxAddressToOffset((void*)(vbuf + 28), &off);
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_NORMAL, 0, off, stride, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    // Symmetric to BindUIAttribs: disable COLOR0(attr 3) that a prior UI draw
    // leaves enabled, else it is fetched OOB from the stale UI buffer here.
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_COLOR0, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

    u32 ioff;
    rsxAddressToOffset(indexData, &ioff);
    rsxDrawIndexArray(gCtx, GCM_TYPE_TRIANGLES, ioff, numIndices, GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
}

void GFX_DrawStaticMeshComp(StaticMesh3D* comp, StaticMesh* meshOverride)
{
    if (!gDrawMeshEnabled || !gInitialized || comp == nullptr) return;
    StaticMesh* mesh = meshOverride ? meshOverride : comp->GetStaticMesh();
    if (mesh == nullptr) return;
    StaticMeshResource* r = mesh->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mIndexData == nullptr || r->mNumIndices == 0) return;
    DrawLitMesh(r->mVertexData, r->mVertexStride, r->mIndexData, r->mNumIndices,
                comp->GetMaterial(), comp->GetRenderTransform());
}

// =========================================================================
// UI — Quad / QuadBorder / Text / Poly
// =========================================================================

void GFX_CreateQuadResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr) return;
    EnsureUIBuffer(&r->mVertexData, &r->mVertexCapacity, Quad::kMaxQuadVertices * sizeof(RsxUIVertex));
}
void GFX_DestroyQuadResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
}
void GFX_UpdateQuadResourceVertexData(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr) return;
    const uint32_t n = quad->GetNumVertices();
    if (n == 0) return;
    EnsureUIBuffer(&r->mVertexData, &r->mVertexCapacity, n * sizeof(RsxUIVertex));
    if (r->mVertexData == nullptr) return;
    RepackUI(quad->GetVertices(), n, (RsxUIVertex*)r->mVertexData);
}
void GFX_DrawQuad(Quad* quad)
{
    if (!gInitialized || quad == nullptr) return;
    QuadResource* r = quad->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    // Quad vertices are already at screen position (mRect); WidgetModel adds the
    // rotate-around-pivot. GetColor() (glm::vec4 return) MUST be the last quad->
    // call to avoid the ppu-gcc codegen fault (see memory note).
    Texture* tex = quad->GetTexture();
    const uint32_t nv = quad->GetNumVertices();
    const glm::mat4 model = WidgetModel(quad);
    const glm::vec4 col = quad->GetColor();
    DrawUI((const RsxUIVertex*)r->mVertexData, nv, GCM_TYPE_TRIANGLE_FAN, col, tex, model);
}

void GFX_CreateQuadBorderResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr) return;
    EnsureUIBuffer(&r->mVertexData, &r->mVertexCapacity, Quad::kMaxQuadVertices * sizeof(RsxUIVertex));
}
void GFX_DestroyQuadBorderResource(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
}
void GFX_UpdateQuadBorderResourceVertexData(Quad* quad)
{
    if (quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr) return;
    const uint32_t n = quad->GetBorderNumVertices();
    if (n == 0) return;
    EnsureUIBuffer(&r->mVertexData, &r->mVertexCapacity, n * sizeof(RsxUIVertex));
    if (r->mVertexData == nullptr) return;
    RepackUI(quad->GetBorderVertices(), n, (RsxUIVertex*)r->mVertexData);
}
void GFX_DrawQuadBorder(Quad* quad)
{
    if (!gInitialized || quad == nullptr) return;
    QuadResource* r = quad->GetBorderResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    const uint32_t nv = quad->GetBorderNumVertices();
    const glm::mat4 model = WidgetModel(quad);
    const glm::vec4 col = quad->GetBorderColor();
    DrawUI((const RsxUIVertex*)r->mVertexData, nv, GCM_TYPE_TRIANGLE_FAN, col, nullptr, model);
}

void GFX_CreateTextResource(Text* /*text*/) {}
void GFX_DestroyTextResource(Text* text)
{
    if (text == nullptr) return;
    TextResource* r = text->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
    r->mNumBufferCharsAllocated = 0;
}
void GFX_UpdateTextResourceVertexData(Text* text)
{
    if (text == nullptr) return;
    TextResource* r = text->GetResource();
    if (r == nullptr) return;
    const uint32_t numCharsAlloc = text->GetNumCharactersAllocated();
    if (numCharsAlloc == 0 || text->GetText().empty()) return;

    const uint32_t verts = numCharsAlloc * TEXT_VERTS_PER_CHAR;
    const uint32_t bytes = verts * sizeof(RsxUIVertex);
    if (r->mVertexCapacity < bytes)
    {
        if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
        r->mVertexData = RsxZAlloc(128, bytes);
        r->mVertexCapacity = (r->mVertexData != nullptr) ? bytes : 0;
        r->mNumBufferCharsAllocated = (r->mVertexData != nullptr) ? numCharsAlloc : 0;
    }
    if (r->mVertexData == nullptr) return;
    RepackUI(text->GetVertices(), verts, (RsxUIVertex*)r->mVertexData);
}
void GFX_DrawText(Text* text)
{
    if (!gInitialized || text == nullptr) return;
    TextResource* r = text->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;

    Font* font = text->GetFont();
    if (font == nullptr) return;
    Texture* fontTex = font->GetTexture();
    if (fontTex == nullptr) return;

    const uint32_t numVisible = text->GetNumVisibleCharacters();
    if (numVisible == 0) return;
    const uint32_t numVerts = numVisible * TEXT_VERTS_PER_CHAR;

    // Text vertices are widget-local at the font's native size; apply the same
    // scale + offset the shader backends bake in, straight into the RSX buffer.
    const int32_t fontSize = font->GetSize();
    const float scale = (fontSize > 0) ? (text->GetScaledTextSize() / (float)fontSize) : 1.0f;
    const Rect rect = text->GetRect();
    const glm::vec2 justified = text->GetJustifiedOffset();
    const float ox = rect.mX + justified.x;
    const float oy = rect.mY + justified.y;

    // Text vertices stay widget-local (font-native size). Position/scale/rotate
    // them via the model matrix — the vertex buffer is NOT mutated, so the
    // asynchronous RSX draw reads the correct data. Order: rotate-around-pivot
    // (WidgetModel) * translate to the widget rect * scale to the text size.
    const glm::mat4 model = WidgetModel(text)
        * glm::translate(glm::mat4(1.0f), glm::vec3(ox, oy, 0.0f))
        * glm::scale(glm::mat4(1.0f), glm::vec3(scale, scale, 1.0f));

    // GetColor() returns glm::vec4 by value — keep it LAST (after every other
    // text-> call) to avoid the ppu-gcc codegen fault. See memory note.
    const glm::vec4 color = text->GetColor();
    DrawUI((const RsxUIVertex*)r->mVertexData, numVerts, GCM_TYPE_TRIANGLES, color, fontTex, model);
}

void GFX_CreatePolyResource(Poly* /*poly*/) {}
void GFX_DestroyPolyResource(Poly* poly)
{
    if (poly == nullptr) return;
    PolyResource* r = poly->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = 0;
}
void GFX_UpdatePolyResourceVertexData(Poly* poly)
{
    if (poly == nullptr) return;
    PolyResource* r = poly->GetResource();
    if (r == nullptr) return;
    const uint32_t n = poly->GetNumVertices();
    if (n == 0) return;
    EnsureUIBuffer(&r->mVertexData, &r->mVertexCapacity, n * sizeof(RsxUIVertex));
    if (r->mVertexData == nullptr) return;
    RepackUI(poly->GetVertices(), n, (RsxUIVertex*)r->mVertexData);
}
void GFX_DrawPoly(Poly* poly)
{
    if (!gInitialized || poly == nullptr) return;
    PolyResource* r = poly->GetResource();
    if (r == nullptr || r->mVertexData == nullptr) return;
    const uint32_t nv = poly->GetNumVertices();
    const glm::mat4 model = WidgetModel(poly);
    const glm::vec4 col = poly->GetColor();
    DrawUI((const RsxUIVertex*)r->mVertexData, nv, GCM_TYPE_LINE_STRIP, col, nullptr, model);
}

// =========================================================================
// Remaining GFX_* surface — stubs slotted on top of this foundation later.
// =========================================================================

// Skeletal mesh: the engine CPU-skins each frame into a std::vector<Vertex>
// (same layout as static Vertex) and hands it to the per-component buffer; the
// mesh ASSET resource keeps only the (static) index buffer. Draw = indices from
// the mesh resource + skinned verts from the comp resource, via DrawLitMesh.
void GFX_CreateSkeletalMeshResource(SkeletalMesh* skeletalMesh, uint32_t /*numVertices*/, VertexSkinned* /*vertices*/, uint32_t numIndices, IndexType* indices)
{
    if (skeletalMesh == nullptr || indices == nullptr || numIndices == 0) return;
    SkeletalMeshResource* r = skeletalMesh->GetResource();
    if (r == nullptr) return;
    const uint32_t iBytes = sizeof(IndexType) * numIndices;
    void* iBuf = RsxZAlloc(128, iBytes);
    if (iBuf == nullptr) return;
    memcpy(iBuf, indices, iBytes);
    r->mIndexData  = iBuf;
    r->mNumIndices = numIndices;
}
void GFX_DestroySkeletalMeshResource(SkeletalMesh* skeletalMesh)
{
    if (skeletalMesh == nullptr) return;
    SkeletalMeshResource* r = skeletalMesh->GetResource();
    if (r == nullptr) return;
    if (r->mIndexData != nullptr) { rsxFree(r->mIndexData); r->mIndexData = nullptr; }
    r->mNumIndices = 0;
}

void GFX_CreateStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_DestroyStaticMeshCompResource(StaticMesh3D* /*staticMeshComp*/) {}
void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* /*staticMeshComp*/) {}

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* /*skeletalMeshComp*/) {}   // lazy alloc
void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* skeletalMeshComp)
{
    if (skeletalMeshComp == nullptr) return;
    SkeletalMeshCompResource* r = skeletalMeshComp->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = r->mNumVertices = r->mVertexStride = 0;
}
void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* skeletalMeshComp, uint32_t numVertices)
{
    if (skeletalMeshComp == nullptr || numVertices == 0) return;
    SkeletalMeshCompResource* r = skeletalMeshComp->GetResource();
    if (r == nullptr) return;
    const uint32_t stride = sizeof(Vertex);
    const uint32_t needed = stride * numVertices;
    if (r->mVertexData != nullptr && r->mVertexCapacity >= needed)
    {
        r->mNumVertices  = numVertices;
        r->mVertexStride = stride;
        return;
    }
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexData     = RsxZAlloc(128, needed);
    r->mVertexCapacity = (r->mVertexData != nullptr) ? needed : 0;
    r->mNumVertices    = (r->mVertexData != nullptr) ? numVertices : 0;
    r->mVertexStride   = stride;
}
void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* skeletalMeshComp, const std::vector<Vertex>& skinnedVertices)
{
    if (skeletalMeshComp == nullptr || skinnedVertices.empty()) return;
    SkeletalMeshCompResource* r = skeletalMeshComp->GetResource();
    if (r == nullptr) return;
    const uint32_t stride = sizeof(Vertex);
    const uint32_t n      = (uint32_t)skinnedVertices.size();
    const uint32_t needed = stride * n;
    if (r->mVertexData == nullptr || r->mVertexCapacity < needed)
    {
        if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
        r->mVertexData     = RsxZAlloc(128, needed);
        r->mVertexCapacity = (r->mVertexData != nullptr) ? needed : 0;
    }
    if (r->mVertexData == nullptr) return;
    memcpy(r->mVertexData, skinnedVertices.data(), needed);
    r->mNumVertices  = n;
    r->mVertexStride = stride;
}
void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* skeletalMeshComp)
{
    if (!gDrawMeshEnabled || !gInitialized || skeletalMeshComp == nullptr) return;
    SkeletalMesh* mesh = skeletalMeshComp->GetSkeletalMesh();
    if (mesh == nullptr) return;
    SkeletalMeshResource*     mr = mesh->GetResource();
    SkeletalMeshCompResource* cr = skeletalMeshComp->GetResource();
    if (mr == nullptr || cr == nullptr) return;
    if (mr->mIndexData == nullptr || mr->mNumIndices == 0) return;
    if (cr->mVertexData == nullptr || cr->mNumVertices == 0) return;
    DrawLitMesh(cr->mVertexData, cr->mVertexStride, mr->mIndexData, mr->mNumIndices,
                skeletalMeshComp->GetMaterial(), skeletalMeshComp->GetRenderTransform());
}
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

// Particles: the engine emits 4 world-space (pre-billboarded) verts per
// particle; we expand to 2 triangles and repack colour to floats (endian-safe).
// Draw uses the camera MVP (model = local transform or identity) + the particle
// shader (pos3/uv/vertex-colour, textured, material-tinted).
void GFX_CreateParticleCompResource(Particle3D* /*particleComp*/) {}   // lazy alloc
void GFX_DestroyParticleCompResource(Particle3D* particleComp)
{
    if (particleComp == nullptr) return;
    ParticleCompResource* r = particleComp->GetResource();
    if (r == nullptr) return;
    if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
    r->mVertexCapacity = r->mNumVertices = r->mVertexStride = 0;
}
void GFX_UpdateParticleCompVertexBuffer(Particle3D* particleComp, const std::vector<VertexParticle>& vertices)
{
    if (particleComp == nullptr) return;
    ParticleCompResource* r = particleComp->GetResource();
    if (r == nullptr) return;
    const uint32_t numParticles = (uint32_t)vertices.size() / 4u;
    if (numParticles == 0) { r->mNumVertices = 0; return; }

    const uint32_t outVerts = numParticles * 6u;
    const uint32_t stride   = sizeof(RsxPartVertex);
    const uint32_t needed   = outVerts * stride;
    if (r->mVertexData == nullptr || r->mVertexCapacity < needed)
    {
        if (r->mVertexData != nullptr) { rsxFree(r->mVertexData); r->mVertexData = nullptr; }
        r->mVertexData     = RsxZAlloc(128, needed);
        r->mVertexCapacity = (r->mVertexData != nullptr) ? needed : 0;
    }
    if (r->mVertexData == nullptr) { r->mNumVertices = 0; return; }

    // Engine pre-divides particle vertex color (RGBA) by the renderer's color
    // scale (an HDR trick to fit >1.0 colours in 8-bit); ForwardParticle.vert
    // multiplies it back by mColorScale. Do the same here or sprites come out
    // at half brightness / half alpha (dim, dark-transparent).
    const float colorScale = Renderer::Get()->GetColorScale();
    // Quad corners 0=TL 1=BL 2=TR 3=BR → triangles (0,1,2)(2,1,3).
    static const int kIdx[6] = { 0, 1, 2, 2, 1, 3 };
    RsxPartVertex* dst = (RsxPartVertex*)r->mVertexData;
    for (uint32_t p = 0; p < numParticles; ++p)
    {
        const VertexParticle* quad = &vertices[p * 4u];
        for (int k = 0; k < 6; ++k)
        {
            const VertexParticle& s = quad[kIdx[k]];
            RsxPartVertex& d = dst[p * 6u + k];
            d.x = s.mPosition.x; d.y = s.mPosition.y; d.z = s.mPosition.z;
            d.u = s.mTexcoord.x; d.v = s.mTexcoord.y;
            const uint32_t c = s.mColor;   // engine packs R in the low byte
            d.r = float(c & 0xFF)         / 255.0f * colorScale;
            d.g = float((c >> 8)  & 0xFF) / 255.0f * colorScale;
            d.b = float((c >> 16) & 0xFF) / 255.0f * colorScale;
            d.a = float((c >> 24) & 0xFF) / 255.0f * colorScale;
        }
    }
    r->mNumVertices  = outVerts;
    r->mVertexStride = stride;
}
void GFX_DrawParticleComp(Particle3D* particleComp)
{
    if (!gDrawMeshEnabled || !gInitialized || particleComp == nullptr) return;
    ParticleCompResource* r = particleComp->GetResource();
    if (r == nullptr || r->mVertexData == nullptr || r->mNumVertices == 0) return;

    Material* matBase = particleComp->GetMaterial();
    MaterialLite* mat = Material::AsLite(matBase ? matBase : Renderer::Get()->GetDefaultMaterial());
    Texture* tex = mat ? mat->GetTexture(0) : nullptr;
    const BlendMode blend = mat ? mat->GetBlendMode() : BlendMode::Translucent;
    const bool localSpace = particleComp->GetUseLocalSpace();

    // Honour the material's blend mode, matching the desktop pipeline. Opaque/
    // Masked particles render solid (blend off, depth write on) — the engine's
    // default P_DefaultParticle uses M_DefaultUnlit (Opaque), so its sprites are
    // solid colour squares, NOT alpha-faded (force-blending them washed the
    // colour out over a bright sky → looked grey). Only Translucent/Additive
    // blend, and those skip depth write so overlapping sprites don't occlude.
    const bool blended = (blend == BlendMode::Translucent || blend == BlendMode::Additive);
    rsxSetBlendEnable(gCtx, blended ? GCM_TRUE : GCM_FALSE);
    if (blended)
    {
        const u32 dstFactor = (blend == BlendMode::Additive) ? GCM_ONE : GCM_ONE_MINUS_SRC_ALPHA;
        rsxSetBlendFunc(gCtx, GCM_SRC_ALPHA, dstFactor, GCM_SRC_ALPHA, dstFactor);
        rsxSetBlendEquation(gCtx, GCM_FUNC_ADD, GCM_FUNC_ADD);
    }
    rsxSetDepthTestEnable(gCtx, GCM_TRUE);
    rsxSetDepthWriteEnable(gCtx, blended ? GCM_FALSE : GCM_TRUE);
    rsxSetCullFaceEnable(gCtx, GCM_FALSE);

    const glm::mat4 model = localSpace ? particleComp->GetTransform() : glm::mat4(1.0f);
    const glm::mat4 mvp   = gProjMatrix * gViewMatrix * model;
    // GetColor() (glm::vec4 by value) kept last — ppu-gcc codegen hazard.
    const glm::vec4 tint  = mat ? mat->GetColor() : glm::vec4(1.0f);

    rsxLoadVertexProgram(gCtx, gPartVp, gPartVpUcode);
    SetVertexMatrix(gPartVp, gPartMvp, mvp);
    SetFragmentVec4(gPartFp, gPartTint, gPartFpOffset, tint);
    BindTextureUnit(tex, gPartTexUnit ? (u8)gPartTexUnit->index : 0);
    rsxLoadFragmentProgramLocation(gCtx, gPartFp, gPartFpOffset, GCM_LOCATION_RSX);

    const uint8_t* vbuf = (const uint8_t*)r->mVertexData;
    const uint32_t stride = r->mVertexStride;
    u32 off;
    rsxAddressToOffset((void*)(vbuf + 0),  &off);
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_POS,    0, off, stride, 3, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxAddressToOffset((void*)(vbuf + 12), &off);
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_TEX0,   0, off, stride, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxAddressToOffset((void*)(vbuf + 20), &off);
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_COLOR0, 0, off, stride, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    // Disable NORMAL(attr 2) a prior mesh draw may have left enabled.
    rsxBindVertexArrayAttrib(gCtx, GCM_VERTEX_ATTRIB_NORMAL, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

    rsxDrawVertexArray(gCtx, GCM_TYPE_TRIANGLES, 0, r->mNumVertices);
}

void GFX_DrawStaticMesh(StaticMesh* /*mesh*/, Material* /*material*/, const glm::mat4& /*transform*/, glm::vec4 /*color*/) {}
void GFX_RenderPostProcessPasses() {}

#endif // POLYPHASE_PLATFORM_ADDON

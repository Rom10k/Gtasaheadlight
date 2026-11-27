// HeadlightTuner v2 - GTA San Andreas Android 2.00 (libGTASA.so, ARM32) AML plugin
//
// 1) Ground light (the game's own headlight.png / headlight1.png projection):
//    CShadows::StoreCarLightShadow() is hooked and Width / Length / Brightness / Height /
//    BumperDistance can be changed in AML's config file.
//
// 2) Wall light (new): headlights now also light fences, walls, buildings and other objects.
//    For every lit headlight a small grid of rays is cast along the beam
//    (CWorld::ProcessLineOfSight). Where a ray hits a non-ground surface, an additive light
//    patch (the same headlight texture) is drawn on that surface with RenderWare immediate mode
//    after CCoronas::Render(). No shadows are generated. The patches are rebuilt every few
//    frames and cached, to keep it cheap.
//
// Everything is found by exported symbol name (dlsym), no hard-coded code offsets.
#include <mod/config.h>
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <cstdint>
#include <cmath>
#include <cstring>

MYMODCFG(net.user.headlighttuner, Headlight Tuner, 2.1.0, user)

struct CVector { float x, y, z; };

struct TunerCfg
{
    int   enabled        = 1;
    // ground light
    float width          = 1.0f;
    float length         = 1.0f;
    float brightness     = 1.0f;
    float height         = 0.0f;
    float bumperDistance = 0.0f;   // m: moves the whole ground patch along the car's direction (+ away from the car)
    // wall light
    int   wall           = 1;
    float wallSize       = 1.0f;   // size of one light patch
    float wallBrightness = 1.0f;
    float wallRange      = 25.0f;  // m: how far the beam reaches
    float wallCone       = 0.45f;  // sideways spread of the beam (0.45 ~ 24 degrees)
    float wallConeV      = 0.16f;  // vertical spread
    int   texture        = 1;      // wall light texture: -1 = headlight.png, 0..9 = the game's corona textures (1 = soft round glow)
    int   rays           = 3;      // rays per axis (3 or 5): more = smoother but slower
    int   maxLights      = 6;      // lit headlights processed per frame (nearest first is not guaranteed)
    float maxDistance    = 70.0f;  // m: headlights further than this from the player are ignored
    int   updateFrames   = 2;      // the patches are rebuilt every N frames
    float originForward  = 1.9f;   // m: where the headlight sits in front of the car's centre
    float originHeight   = 0.55f;  // m: and above it
    float originSpread   = 0.65f;  // m: and to each side
};
static TunerCfg g_cfg;

static void LoadCfg()
{
    TunerCfg d;
    g_cfg.enabled        = cfg->Bind("Enabled",        d.enabled,        "General")->GetInt();
    g_cfg.width          = cfg->Bind("Width",          d.width,          "Headlight")->GetFloat();
    g_cfg.length         = cfg->Bind("Length",         d.length,         "Headlight")->GetFloat();
    g_cfg.brightness     = cfg->Bind("Brightness",     d.brightness,     "Headlight")->GetFloat();
    g_cfg.height         = cfg->Bind("Height",         d.height,         "Headlight")->GetFloat();
    g_cfg.bumperDistance = cfg->Bind("BumperDistance", d.bumperDistance, "Headlight")->GetFloat();
    g_cfg.wall           = cfg->Bind("Enabled",        d.wall,           "WallLight")->GetInt();
    g_cfg.wallSize       = cfg->Bind("Size",           d.wallSize,       "WallLight")->GetFloat();
    g_cfg.wallBrightness = cfg->Bind("Brightness",     d.wallBrightness, "WallLight")->GetFloat();
    g_cfg.wallRange      = cfg->Bind("Range",          d.wallRange,      "WallLight")->GetFloat();
    g_cfg.wallCone       = cfg->Bind("ConeWidth",      d.wallCone,       "WallLight")->GetFloat();
    g_cfg.wallConeV      = cfg->Bind("ConeHeight",     d.wallConeV,      "WallLight")->GetFloat();
    g_cfg.texture        = cfg->Bind("TextureIndex",   d.texture,        "WallLight")->GetInt();
    g_cfg.rays           = cfg->Bind("Rays",           d.rays,           "WallLight")->GetInt();
    g_cfg.maxLights      = cfg->Bind("MaxLights",      d.maxLights,      "WallLight")->GetInt();
    g_cfg.maxDistance    = cfg->Bind("MaxDistance",    d.maxDistance,    "WallLight")->GetFloat();
    g_cfg.updateFrames   = cfg->Bind("UpdateFrames",   d.updateFrames,   "WallLight")->GetInt();
    g_cfg.originForward  = cfg->Bind("OriginForward",  d.originForward,  "WallLight")->GetFloat();
    g_cfg.originHeight   = cfg->Bind("OriginHeight",   d.originHeight,   "WallLight")->GetFloat();
    g_cfg.originSpread   = cfg->Bind("OriginSpread",   d.originSpread,   "WallLight")->GetFloat();
    cfg->Save();

    if (g_cfg.rays < 2) g_cfg.rays = 2;
    if (g_cfg.rays > 5) g_cfg.rays = 5;
    if (g_cfg.maxLights < 0) g_cfg.maxLights = 0;
    if (g_cfg.maxLights > 12) g_cfg.maxLights = 12;
    if (g_cfg.updateFrames < 1) g_cfg.updateFrames = 1;
    if (g_cfg.wallRange < 1.0f) g_cfg.wallRange = 1.0f;
}

// ---------------------------------------------------------------------------------------------
// game functions
typedef void (*StoreCarLightShadow_t)(void* veh, int id, void* tex, CVector* pos,
                                      float frontX, float frontY, float sideX, float sideY,
                                      uint8_t r, uint8_t g, uint8_t b, float maxViewAngle);
typedef void (*DoHeadLightBeam_t)(void* veh, int dummyId, float* matrix, uint8_t left);
typedef void (*CoronasRender_t)();
typedef bool (*LOS_t)(const CVector& from, const CVector& to, void* colPoint, void** entityOut,
                      bool buildings, bool vehicles, bool peds, bool objects,
                      bool dummies, bool seeThrough, bool camIgnore, bool shootThrough);
typedef void (*FindPlayerCoors_t)(CVector* out, int playerNum);
typedef int  (*RwRenderStateSet_t)(int state, void* value);
typedef int  (*RwRenderStateGet_t)(int state, void* value);
typedef void* (*RwIm3DTransform_t)(void* verts, unsigned int num, void* ltm, unsigned int flags);
typedef int  (*RwIm3DRenderIndexedPrimitive_t)(int primType, uint16_t* indices, int numIndices);
typedef int  (*RwIm3DEnd_t)();

static StoreCarLightShadow_t  Store_orig      = NULL;
static DoHeadLightBeam_t      Beam_orig       = NULL;
static CoronasRender_t        Coronas_orig    = NULL;
static LOS_t                  LOS_f           = NULL;
static FindPlayerCoors_t      FindPlayerCoors_f = NULL;
static RwRenderStateSet_t     RwRenderStateSet_f = NULL;
static RwRenderStateGet_t     RwRenderStateGet_f = NULL;
static RwIm3DTransform_t      RwIm3DTransform_f = NULL;
static RwIm3DRenderIndexedPrimitive_t RwIm3DRender_f = NULL;
static RwIm3DEnd_t            RwIm3DEnd_f     = NULL;

// RenderWare constants
enum { RS_TEXTURERASTER = 1, RS_ZTESTENABLE = 6, RS_ZWRITEENABLE = 8, RS_SRCBLEND = 10, RS_DESTBLEND = 11,
       RS_VERTEXALPHAENABLE = 12, RS_CULLMODE = 20 };
enum { BLEND_ONE = 2, BLEND_SRCALPHA = 5 };
enum { CULL_NONE = 1 };
enum { PRIM_TRILIST = 3 };
enum { IM3D_VERTEXUV = 1 };

struct Vertex3D { float x, y, z; float nx, ny, nz; uint32_t color; float u, v; };   // RxObjSpace3DVertex (36 bytes)

static inline uint8_t Clamp255(float v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)(v + 0.5f)); }

// ---------------------------------------------------------------------------------------------
// 1) ground light
static void* g_lightTex = NULL;     // the headlight texture seen in StoreCarLightShadow
static void** g_coronaTex = NULL;   // gpCoronaTexture[] of the game (soft round glows)

static void Store_hook(void* veh, int id, void* tex, CVector* pos,
                       float fx, float fy, float sx, float sy,
                       uint8_t r, uint8_t g, uint8_t b, float maxViewAngle)
{
    if (tex) g_lightTex = tex;
    if (!g_cfg.enabled || !pos)
    {
        Store_orig(veh, id, tex, pos, fx, fy, sx, sy, r, g, b, maxViewAngle);
        return;
    }

    CVector p = *pos;

    // BumperDistance: move the whole patch along the beam direction
    float flen = std::sqrt(fx * fx + fy * fy);
    if (flen > 0.0001f && g_cfg.bumperDistance != 0.0f)
    {
        p.x += fx / flen * g_cfg.bumperDistance;
        p.y += fy / flen * g_cfg.bumperDistance;
    }

    // Length / Width: the position stays where the game put it, only the size changes
    fx *= g_cfg.length; fy *= g_cfg.length;
    sx *= g_cfg.width;  sy *= g_cfg.width;
    p.z += g_cfg.height;

    r = Clamp255(r * g_cfg.brightness);
    g = Clamp255(g * g_cfg.brightness);
    b = Clamp255(b * g_cfg.brightness);

    Store_orig(veh, id, tex, &p, fx, fy, sx, sy, r, g, b, maxViewAngle);
}

// ---------------------------------------------------------------------------------------------
// 2) wall light
struct BeamLight { CVector pos; CVector dir; CVector right; CVector up; };
static BeamLight g_lights[16];
static int       g_lightCount = 0;

static void Beam_hook(void* veh, int dummyId, float* m, uint8_t left)
{
    Beam_orig(veh, dummyId, m, left);
    if (!g_cfg.enabled || !g_cfg.wall || !m || g_lightCount >= 16) return;

    // CMatrix: right(0) up(16) = driving direction, at(32), pos(48)
    BeamLight& L = g_lights[g_lightCount];
    L.right = { m[0], m[1], m[2] };
    L.dir   = { m[4], m[5], m[6] };
    L.up    = { m[8], m[9], m[10] };
    float side = left ? -g_cfg.originSpread : g_cfg.originSpread;
    L.pos = { m[12] + m[4] * g_cfg.originForward + m[0] * side + m[8] * g_cfg.originHeight,
              m[13] + m[5] * g_cfg.originForward + m[1] * side + m[9] * g_cfg.originHeight,
              m[14] + m[6] * g_cfg.originForward + m[2] * side + m[10] * g_cfg.originHeight };
    g_lightCount++;
}

static const int MAX_QUADS = 12 * 25;
static Vertex3D  g_verts[MAX_QUADS * 4];
static uint16_t  g_indices[MAX_QUADS * 6];
static int       g_quadCount = 0;
static int       g_frame = 0;

static inline CVector Sub(const CVector& a, const CVector& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline float   Dot(const CVector& a, const CVector& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline CVector Cross(const CVector& a, const CVector& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline float   Len(const CVector& a) { return std::sqrt(Dot(a, a)); }
static inline CVector Norm(const CVector& a) { float l = Len(a); return l > 1e-5f ? CVector{ a.x / l, a.y / l, a.z / l } : CVector{ 0, 0, 1 }; }

static void BuildQuads(const CVector& playerPos)
{
    g_quadCount = 0;
    int used = 0;
    const int N = g_cfg.rays;
    float OFF[5];
    for (int k = 0; k < N; k++) OFF[k] = N > 1 ? -1.0f + 2.0f * (float)k / (float)(N - 1) : 0.0f;
    const float* DX = OFF;
    const float* DZ = OFF;

    for (int i = 0; i < g_lightCount && used < g_cfg.maxLights; i++)
    {
        const BeamLight& L = g_lights[i];
        CVector dp = Sub(L.pos, playerPos);
        if (Len(dp) > g_cfg.maxDistance) continue;
        used++;

        for (int ix = 0; ix < N; ix++)
        for (int iz = 0; iz < N; iz++)
        {
            if (g_quadCount >= MAX_QUADS) return;

            CVector d = Norm({ L.dir.x + L.right.x * DX[ix] * g_cfg.wallCone + L.up.x * (DZ[iz] * g_cfg.wallConeV - 0.05f),
                               L.dir.y + L.right.y * DX[ix] * g_cfg.wallCone + L.up.y * (DZ[iz] * g_cfg.wallConeV - 0.05f),
                               L.dir.z + L.right.z * DX[ix] * g_cfg.wallCone + L.up.z * (DZ[iz] * g_cfg.wallConeV - 0.05f) });
            CVector to = { L.pos.x + d.x * g_cfg.wallRange, L.pos.y + d.y * g_cfg.wallRange, L.pos.z + d.z * g_cfg.wallRange };

            alignas(16) char colPoint[160];
            memset(colPoint, 0, sizeof(colPoint));
            void* hit = NULL;
            // buildings, vehicles(no: the ground light already handles them), peds(no), objects, dummies
            if (!LOS_f(L.pos, to, colPoint, &hit, true, false, false, true, true, false, false, false)) continue;

            const float* hp = (const float*)colPoint;                  // CColPoint::m_vecPoint
            const float* hn = (const float*)(colPoint + 0x10);         // CColPoint::m_vecNormal
            CVector p = { hp[0], hp[1], hp[2] };
            CVector n = { hn[0], hn[1], hn[2] };
            float nl = Len(n);
            if (!(nl > 0.9f && nl < 1.1f)) n = Norm({ -d.x, -d.y, -d.z });   // fall back to "facing the light"
            else { n.x /= nl; n.y /= nl; n.z /= nl; }
            if (Dot(n, d) > 0.0f) n = { -n.x, -n.y, -n.z };                 // face the light

            if (n.z > 0.65f) continue;                                     // ground: the game lights it already
            CVector t1 = Cross(n, { 0, 0, 1 });
            if (Len(t1) < 0.2f) continue;                                  // ceiling / floor
            t1 = Norm(t1);
            CVector t2 = Norm(Cross(n, t1));

            float dist = Len(Sub(p, L.pos));
            float fall = 1.0f - dist / g_cfg.wallRange;
            float facing = -Dot(d, n);
            if (fall <= 0.02f || facing <= 0.05f) continue;
            float axis = 1.0f - 0.35f * (std::fabs(DX[ix]) + std::fabs(DZ[iz]) * 0.5f);   // brighter near the beam axis
            float alpha = g_cfg.wallBrightness * fall * fall * facing * axis * 255.0f;
            uint8_t a = Clamp255(alpha);
            if (a < 3) continue;

            float half = g_cfg.wallSize * (0.6f + dist * 0.22f);
            if (half > 4.0f) half = 4.0f;

            p = { p.x + n.x * 0.03f, p.y + n.y * 0.03f, p.z + n.z * 0.03f };   // lift off the surface (z-fighting)
            uint32_t col = ((uint32_t)a << 24) | (210u << 16) | (238u << 8) | 255u;    // warm white: bytes in memory are R,G,B,A

            Vertex3D* v = &g_verts[g_quadCount * 4];
            const float sxv[4] = { -1, 1, 1, -1 };
            const float syv[4] = { -1, -1, 1, 1 };
            const float us[4]  = { 0, 1, 1, 0 };
            const float vs[4]  = { 1, 1, 0, 0 };
            for (int k = 0; k < 4; k++)
            {
                v[k].x = p.x + t1.x * sxv[k] * half + t2.x * syv[k] * half;
                v[k].y = p.y + t1.y * sxv[k] * half + t2.y * syv[k] * half;
                v[k].z = p.z + t1.z * sxv[k] * half + t2.z * syv[k] * half;
                v[k].nx = n.x; v[k].ny = n.y; v[k].nz = n.z;
                v[k].color = col;
                v[k].u = us[k]; v[k].v = vs[k];
            }
            uint16_t base = (uint16_t)(g_quadCount * 4);
            uint16_t* ix6 = &g_indices[g_quadCount * 6];
            ix6[0] = base; ix6[1] = base + 1; ix6[2] = base + 2;
            ix6[3] = base; ix6[4] = base + 2; ix6[5] = base + 3;
            g_quadCount++;
        }
    }
}

static void DrawQuads()
{
    void* tex = g_lightTex;
    if (g_cfg.texture >= 0 && g_cfg.texture < 10 && g_coronaTex && g_coronaTex[g_cfg.texture])
        tex = g_coronaTex[g_cfg.texture];                       // soft round glow instead of headlight.png
    if (g_quadCount <= 0 || !tex) return;
    void* raster = *(void**)tex;                               // RwTexture::raster is the first member
    if (!raster) return;

    // remember and set the render states
    int oldZTest = 0, oldZWrite = 0, oldSrc = 0, oldDst = 0, oldAlpha = 0, oldCull = 0;
    void* oldRasterPtr = NULL;
    RwRenderStateGet_f(RS_TEXTURERASTER, &oldRasterPtr);
    RwRenderStateGet_f(RS_ZTESTENABLE, &oldZTest);
    RwRenderStateGet_f(RS_ZWRITEENABLE, &oldZWrite);
    RwRenderStateGet_f(RS_SRCBLEND, &oldSrc);
    RwRenderStateGet_f(RS_DESTBLEND, &oldDst);
    RwRenderStateGet_f(RS_VERTEXALPHAENABLE, &oldAlpha);
    RwRenderStateGet_f(RS_CULLMODE, &oldCull);

    RwRenderStateSet_f(RS_TEXTURERASTER, raster);
    RwRenderStateSet_f(RS_ZTESTENABLE, (void*)1);
    RwRenderStateSet_f(RS_ZWRITEENABLE, (void*)0);
    RwRenderStateSet_f(RS_VERTEXALPHAENABLE, (void*)1);
    RwRenderStateSet_f(RS_SRCBLEND, (void*)BLEND_SRCALPHA);
    RwRenderStateSet_f(RS_DESTBLEND, (void*)BLEND_ONE);
    RwRenderStateSet_f(RS_CULLMODE, (void*)CULL_NONE);

    if (RwIm3DTransform_f(g_verts, (unsigned)(g_quadCount * 4), NULL, IM3D_VERTEXUV))
    {
        RwIm3DRender_f(PRIM_TRILIST, g_indices, g_quadCount * 6);
        RwIm3DEnd_f();
    }

    RwRenderStateSet_f(RS_TEXTURERASTER, oldRasterPtr);
    RwRenderStateSet_f(RS_ZTESTENABLE, (void*)(intptr_t)oldZTest);
    RwRenderStateSet_f(RS_ZWRITEENABLE, (void*)(intptr_t)oldZWrite);
    RwRenderStateSet_f(RS_VERTEXALPHAENABLE, (void*)(intptr_t)oldAlpha);
    RwRenderStateSet_f(RS_SRCBLEND, (void*)(intptr_t)oldSrc);
    RwRenderStateSet_f(RS_DESTBLEND, (void*)(intptr_t)oldDst);
    RwRenderStateSet_f(RS_CULLMODE, (void*)(intptr_t)oldCull);
}

static void Coronas_hook()
{
    Coronas_orig();                                            // let the game draw its coronas first
    if (!g_cfg.enabled || !g_cfg.wall) { g_lightCount = 0; return; }

    if (g_lightCount == 0) { g_quadCount = 0; return; }        // no headlight is on

    if ((g_frame++ % g_cfg.updateFrames) == 0)
    {
        CVector player = { 0, 0, 0 };
        FindPlayerCoors_f(&player, -1);
        BuildQuads(player);
    }
    g_lightCount = 0;                                          // lights are collected again next frame
    DrawQuads();
}

// ---------------------------------------------------------------------------------------------
extern "C" void OnModLoad()
{
    logger->SetTag("HeadlightTuner");

    uintptr_t base = aml->GetLib("libGTASA.so");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!base || !h) { logger->Error("libGTASA.so not found"); return; }

    LoadCfg();

    uintptr_t storeAddr = (uintptr_t)aml->GetSym(h, "_ZN8CShadows19StoreCarLightShadowEP8CVehicleiP9RwTextureP7CVectorffffhhhf");
    if (!storeAddr) { logger->Error("StoreCarLightShadow not found, plugin disabled"); return; }
    aml->Hook((void*)storeAddr, (void*)Store_hook, (void**)&Store_orig);
    logger->Info("Ground light tuning active");

    // wall light: every piece is optional, the ground light keeps working without it
    uintptr_t beamAddr    = (uintptr_t)aml->GetSym(h, "_ZN8CVehicle15DoHeadLightBeamEiR7CMatrixh");
    uintptr_t coronasAddr = (uintptr_t)aml->GetSym(h, "_ZN8CCoronas6RenderEv");
    LOS_f              = (LOS_t)             aml->GetSym(h, "_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbb");
    FindPlayerCoors_f  = (FindPlayerCoors_t) aml->GetSym(h, "_Z15FindPlayerCoorsi");
    RwRenderStateSet_f = (RwRenderStateSet_t)aml->GetSym(h, "_Z16RwRenderStateSet13RwRenderStatePv");
    RwRenderStateGet_f = (RwRenderStateGet_t)aml->GetSym(h, "_Z16RwRenderStateGet13RwRenderStatePv");
    RwIm3DTransform_f  = (RwIm3DTransform_t) aml->GetSym(h, "_Z15RwIm3DTransformP18RxObjSpace3DVertexjP11RwMatrixTagj");
    RwIm3DRender_f     = (RwIm3DRenderIndexedPrimitive_t)aml->GetSym(h, "_Z28RwIm3DRenderIndexedPrimitive15RwPrimitiveTypePti");
    RwIm3DEnd_f        = (RwIm3DEnd_t)       aml->GetSym(h, "_Z9RwIm3DEndv");
    g_coronaTex        = (void**)            aml->GetSym(h, "gpCoronaTexture");

    if (beamAddr && coronasAddr && LOS_f && FindPlayerCoors_f && RwRenderStateSet_f && RwRenderStateGet_f &&
        RwIm3DTransform_f && RwIm3DRender_f && RwIm3DEnd_f)
    {
        aml->Hook((void*)beamAddr, (void*)Beam_hook, (void**)&Beam_orig);
        aml->Hook((void*)coronasAddr, (void*)Coronas_hook, (void**)&Coronas_orig);
        logger->Info("Wall light active");
    }
    else logger->Error("Wall light disabled: missing game symbols");

    logger->Info("HeadlightTuner v2.1 loaded");
}

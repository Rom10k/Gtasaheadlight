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

MYMODCFG(net.user.headlighttuner, Headlight Tuner, 2.2.1, user)

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
    int   rays           = 3;      // minimum rays per axis near a surface (the number grows with distance)
    int   maxRaysX       = 9;      // most rays sideways per headlight (far surfaces use more, close ones fewer)
    int   maxRaysZ       = 5;      // most rays up/down per headlight
    float groundZ        = 0.88f;  // surfaces flatter than this (normal.z) are left to the game's own ground light; slopes are lit
    int   rayBudget      = 160;    // most rays per rebuild over ALL headlights (keeps the frame rate)
    int   thinObjects    = 1;      // 1 = shrink the patch on thin / small objects (poles, trees, bumps)
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
    g_cfg.maxRaysX       = cfg->Bind("MaxRaysX",       d.maxRaysX,       "WallLight")->GetInt();
    g_cfg.maxRaysZ       = cfg->Bind("MaxRaysZ",       d.maxRaysZ,       "WallLight")->GetInt();
    g_cfg.groundZ        = cfg->Bind("GroundNormalZ",  d.groundZ,        "WallLight")->GetFloat();
    g_cfg.thinObjects    = cfg->Bind("ThinObjects",    d.thinObjects,    "WallLight")->GetInt();
    g_cfg.rayBudget      = cfg->Bind("RayBudget",      d.rayBudget,      "WallLight")->GetInt();
    g_cfg.maxLights      = cfg->Bind("MaxLights",      d.maxLights,      "WallLight")->GetInt();
    g_cfg.maxDistance    = cfg->Bind("MaxDistance",    d.maxDistance,    "WallLight")->GetFloat();
    g_cfg.updateFrames   = cfg->Bind("UpdateFrames",   d.updateFrames,   "WallLight")->GetInt();
    g_cfg.originForward  = cfg->Bind("OriginForward",  d.originForward,  "WallLight")->GetFloat();
    g_cfg.originHeight   = cfg->Bind("OriginHeight",   d.originHeight,   "WallLight")->GetFloat();
    g_cfg.originSpread   = cfg->Bind("OriginSpread",   d.originSpread,   "WallLight")->GetFloat();
    cfg->Save();

    if (g_cfg.rays < 2) g_cfg.rays = 2;
    if (g_cfg.rays > 9) g_cfg.rays = 9;
    if (g_cfg.maxRaysX < g_cfg.rays) g_cfg.maxRaysX = g_cfg.rays;
    if (g_cfg.maxRaysX > 15) g_cfg.maxRaysX = 15;
    if (g_cfg.maxRaysZ < g_cfg.rays) g_cfg.maxRaysZ = g_cfg.rays;
    if (g_cfg.maxRaysZ > 9) g_cfg.maxRaysZ = 9;
    if (g_cfg.rayBudget < 20) g_cfg.rayBudget = 20;
    if (g_cfg.groundZ < 0.3f) g_cfg.groundZ = 0.3f;
    if (g_cfg.groundZ > 1.0f) g_cfg.groundZ = 1.0f;
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

static const int MAX_QUADS = 400;
static Vertex3D  g_verts[MAX_QUADS * 4];
static uint16_t  g_indices[MAX_QUADS * 6];
static int       g_quadCount = 0;
static int       g_frame = 0;

static inline CVector Sub(const CVector& a, const CVector& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline float   Dot(const CVector& a, const CVector& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline CVector Cross(const CVector& a, const CVector& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline float   Len(const CVector& a) { return std::sqrt(Dot(a, a)); }
static inline CVector Norm(const CVector& a) { float l = Len(a); return l > 1e-5f ? CVector{ a.x / l, a.y / l, a.z / l } : CVector{ 0, 0, 1 }; }

struct Hit { bool valid; CVector p, n; float dist, facing; };

static bool CastRay(const BeamLight& L, const CVector& d, Hit& out)
{
    out.valid = false;
    CVector to = { L.pos.x + d.x * g_cfg.wallRange, L.pos.y + d.y * g_cfg.wallRange, L.pos.z + d.z * g_cfg.wallRange };
    alignas(16) char colPoint[160];
    memset(colPoint, 0, sizeof(colPoint));
    void* hit = NULL;
    // buildings, objects, dummies (vehicles and peds are lit by the game's own light)
    if (!LOS_f(L.pos, to, colPoint, &hit, true, false, false, true, true, false, false, false)) return false;

    const float* hp = (const float*)colPoint;                  // CColPoint::m_vecPoint
    const float* hn = (const float*)(colPoint + 0x10);         // CColPoint::m_vecNormal
    CVector p = { hp[0], hp[1], hp[2] };
    CVector n = { hn[0], hn[1], hn[2] };
    float nl = Len(n);
    if (!(nl > 0.9f && nl < 1.1f)) n = Norm({ -d.x, -d.y, -d.z });
    else { n.x /= nl; n.y /= nl; n.z /= nl; }
    if (Dot(n, d) > 0.0f) n = { -n.x, -n.y, -n.z };            // face the light

    out.valid  = true;
    out.p      = p;
    out.n      = n;
    out.dist   = Len(Sub(p, L.pos));
    out.facing = -Dot(d, n);
    return true;
}

static inline bool Similar(const Hit& a, const Hit& b, float tol)
{
    return a.valid && b.valid && std::fabs(a.dist - b.dist) <= tol;
}

static void BuildQuads(const CVector& playerPos)
{
    g_quadCount = 0;
    int used = 0;
    const int MAXX = 15, MAXZ = 9;
    static Hit hits[MAXX][MAXZ];

    int eligible = 0;
    for (int i = 0; i < g_lightCount && eligible < g_cfg.maxLights; i++)
        if (Len(Sub(g_lights[i].pos, playerPos)) <= g_cfg.maxDistance) eligible++;
    if (eligible < 1) return;
    const int perLight = g_cfg.rayBudget / eligible;            // rays this headlight may use (incl. the axis ray)

    for (int i = 0; i < g_lightCount && used < g_cfg.maxLights; i++)
    {
        const BeamLight& L = g_lights[i];
        if (Len(Sub(L.pos, playerPos)) > g_cfg.maxDistance) continue;
        used++;

        // 1) one ray along the beam axis tells how far the lit surface is
        Hit centre;
        CVector axis = Norm({ L.dir.x - L.up.x * 0.05f, L.dir.y - L.up.y * 0.05f, L.dir.z - L.up.z * 0.05f });
        CastRay(L, axis, centre);
        float D0 = centre.valid ? centre.dist : g_cfg.wallRange * 0.6f;

        // 2) patch size grows slowly with distance; the number of rays grows with it, so far surfaces
        //    get MANY close patches and near ones FEW (but still touching each other)
        float halfBase = g_cfg.wallSize * (0.55f + 0.07f * D0);
        if (halfBase > 3.0f) halfBase = 3.0f;
        float spacing = halfBase * 0.9f;
        int nx = (int)std::ceil((2.0f * D0 * g_cfg.wallCone)  / spacing) + 1;
        int nz = (int)std::ceil((2.0f * D0 * g_cfg.wallConeV) / spacing) + 1;
        if (nx < g_cfg.rays) nx = g_cfg.rays;
        if (nz < g_cfg.rays) nz = g_cfg.rays;
        if (nx > g_cfg.maxRaysX) nx = g_cfg.maxRaysX;
        if (nz > g_cfg.maxRaysZ) nz = g_cfg.maxRaysZ;
        while (nx * nz + 1 > perLight && (nx > 2 || nz > 2))      // stay inside the ray budget
        {
            if (nx >= nz && nx > 2) nx--;
            else if (nz > 2) nz--;
            else nx--;
        }

        // 3) cast the grid
        CVector dirs[MAXX][MAXZ];
        for (int ix = 0; ix < nx; ix++)
        for (int iz = 0; iz < nz; iz++)
        {
            float ox = nx > 1 ? -1.0f + 2.0f * (float)ix / (float)(nx - 1) : 0.0f;
            float oz = nz > 1 ? -1.0f + 2.0f * (float)iz / (float)(nz - 1) : 0.0f;
            CVector d = Norm({ L.dir.x + L.right.x * ox * g_cfg.wallCone + L.up.x * (oz * g_cfg.wallConeV - 0.05f),
                               L.dir.y + L.right.y * ox * g_cfg.wallCone + L.up.y * (oz * g_cfg.wallConeV - 0.05f),
                               L.dir.z + L.right.z * ox * g_cfg.wallCone + L.up.z * (oz * g_cfg.wallConeV - 0.05f) });
            dirs[ix][iz] = d;
            CastRay(L, d, hits[ix][iz]);
        }

        // 4) one light patch per hit
        for (int ix = 0; ix < nx; ix++)
        for (int iz = 0; iz < nz; iz++)
        {
            if (g_quadCount >= MAX_QUADS) return;
            const Hit& h = hits[ix][iz];
            if (!h.valid) continue;
            if (h.n.z > g_cfg.groundZ) continue;                   // flat ground: the game's own ground light
            float fall = 1.0f - h.dist / g_cfg.wallRange;
            if (fall <= 0.02f || h.facing <= 0.04f) continue;

            // patch frame: u = the direction the light travels along the surface, v = across it.
            // Grazing light (slopes, hills, oblique walls) gives a stretched ellipse.
            const CVector& d = dirs[ix][iz];
            CVector proj = { d.x - h.n.x * Dot(d, h.n), d.y - h.n.y * Dot(d, h.n), d.z - h.n.z * Dot(d, h.n) };
            CVector u;
            if (Len(proj) > 0.05f) u = Norm(proj);
            else
            {
                u = Cross(h.n, { 0, 0, 1 });
                if (Len(u) < 0.2f) continue;                       // looking straight at a floor / ceiling
                u = Norm(u);
            }
            CVector v = Norm(Cross(h.n, u));

            float half  = halfBase * (0.75f + 0.25f * fall);
            float halfU = half / (h.facing < 0.4f ? 0.4f : h.facing);   // stretch along the light direction
            if (halfU > half * 2.5f) halfU = half * 2.5f;
            float halfV = half;

            // thin / small objects: neighbouring rays that miss or hit something much further away
            // mean the surface is narrower than the patch -> make the patch narrower in that direction
            if (g_cfg.thinObjects)
            {
                float tolH = 0.6f + 0.12f * h.dist;
                float tolV = 1.0f + 0.35f * h.dist;
                bool contX = true, contZ = true;
                if (ix > 0)      contX = contX && Similar(h, hits[ix - 1][iz], tolH);
                if (ix < nx - 1) contX = contX && Similar(h, hits[ix + 1][iz], tolH);
                if (iz > 0)      contZ = contZ && Similar(h, hits[ix][iz - 1], tolV);
                if (iz < nz - 1) contZ = contZ && Similar(h, hits[ix][iz + 1], tolV);
                bool uHoriz = std::sqrt(u.x * u.x + u.y * u.y) > 0.7f;
                float kX = contX ? 1.0f : 0.35f, kZ = contZ ? 1.0f : 0.35f;
                if (uHoriz) { halfU *= kX; halfV *= kZ; } else { halfU *= kZ; halfV *= kX; }
                if (halfU < 0.1f) halfU = 0.1f;
                if (halfV < 0.1f) halfV = 0.1f;
            }

            // overlapping patches add up: keep the brightness even whatever the ray density is
            float cellW = h.dist * 2.0f * g_cfg.wallCone  / (float)(nx > 1 ? nx - 1 : 1);
            float cellH = h.dist * 2.0f * g_cfg.wallConeV / (float)(nz > 1 ? nz - 1 : 1);
            float cell  = cellW * cellH / (h.facing < 0.3f ? 0.3f : h.facing);
            float overlap = (4.0f * halfU * halfV) / (cell > 0.05f ? cell : 0.05f);
            float norm = 1.6f / (overlap < 1.6f ? 1.6f : overlap);

            float axisW = 1.0f - 0.35f * (std::fabs(-1.0f + 2.0f * (float)ix / (float)(nx > 1 ? nx - 1 : 1)) +
                                          std::fabs(-1.0f + 2.0f * (float)iz / (float)(nz > 1 ? nz - 1 : 1)) * 0.5f);
            uint8_t a = Clamp255(g_cfg.wallBrightness * fall * fall * h.facing * axisW * norm * 255.0f);
            if (a < 3) continue;

            CVector p = { h.p.x + h.n.x * 0.03f, h.p.y + h.n.y * 0.03f, h.p.z + h.n.z * 0.03f };   // lift off the surface
            uint32_t col = ((uint32_t)a << 24) | (210u << 16) | (238u << 8) | 255u;                // warm white: bytes R,G,B,A

            Vertex3D* vtx = &g_verts[g_quadCount * 4];
            const float su[4] = { -1, 1, 1, -1 };
            const float sv[4] = { -1, -1, 1, 1 };
            const float us[4] = { 0, 1, 1, 0 };
            const float vs[4] = { 1, 1, 0, 0 };
            for (int k = 0; k < 4; k++)
            {
                vtx[k].x = p.x + u.x * su[k] * halfU + v.x * sv[k] * halfV;
                vtx[k].y = p.y + u.y * su[k] * halfU + v.y * sv[k] * halfV;
                vtx[k].z = p.z + u.z * su[k] * halfU + v.z * sv[k] * halfV;
                vtx[k].nx = h.n.x; vtx[k].ny = h.n.y; vtx[k].nz = h.n.z;
                vtx[k].color = col;
                vtx[k].u = us[k]; vtx[k].v = vs[k];
            }
            uint16_t base = (uint16_t)(g_quadCount * 4);
            uint16_t* q = &g_indices[g_quadCount * 6];
            q[0] = base; q[1] = base + 1; q[2] = base + 2;
            q[3] = base; q[4] = base + 2; q[5] = base + 3;
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

    logger->Info("HeadlightTuner v2.2 loaded");
}

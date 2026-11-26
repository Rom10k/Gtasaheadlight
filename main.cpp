// HeadlightTuner - GTA San Andreas Android 2.00 (libGTASA.so, ARM32) AML plugin
//
// STAGE 1: the headlight beam texture (headlight.png / headlight1.png) is stored every frame by
// CShadows::StoreCarLightShadow(vehicle, id, texture, centre, frontX, frontY, sideX, sideY, r, g, b, maxViewAngle).
// This plugin hooks that function and lets you change, in AML's config file:
//   Width      - how wide the light patch is        (1.0 = vanilla)
//   Length     - how far in front of the car it reaches (1.0 = vanilla), the near edge stays where it is
//   Brightness - colour multiplier                   (1.0 = vanilla)
//   Height     - the patch is moved up/down by this many metres (vanilla = 0)
//
// It does not add any new rendering: the light is still the game's projected ground texture.
// (Lighting walls / fences / buildings needs new rendering code and is a separate, bigger stage.)
#include <mod/config.h>
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <cstdint>
#include <cmath>

MYMODCFG(net.user.headlighttuner, Headlight Tuner, 1.0.0, user)

struct CVector { float x, y, z; };

struct TunerCfg
{
    int   enabled    = 1;
    float width      = 1.0f;
    float length     = 1.0f;
    float brightness = 1.0f;
    float height     = 0.0f;
};
static TunerCfg g_cfg;

typedef void (*StoreCarLightShadow_t)(void* veh, int id, void* tex, CVector* pos,
                                      float frontX, float frontY, float sideX, float sideY,
                                      uint8_t r, uint8_t g, uint8_t b, float maxViewAngle);
static StoreCarLightShadow_t Store_orig = NULL;

static void LoadCfg()
{
    TunerCfg d;
    g_cfg.enabled    = cfg->Bind("Enabled",    d.enabled,    "General")->GetInt();
    g_cfg.width      = cfg->Bind("Width",      d.width,      "Headlight")->GetFloat();
    g_cfg.length     = cfg->Bind("Length",     d.length,     "Headlight")->GetFloat();
    g_cfg.brightness = cfg->Bind("Brightness", d.brightness, "Headlight")->GetFloat();
    g_cfg.height     = cfg->Bind("Height",     d.height,     "Headlight")->GetFloat();
    cfg->Save();
}

static inline uint8_t Clamp255(float v)
{
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

static void Store_hook(void* veh, int id, void* tex, CVector* pos,
                       float fx, float fy, float sx, float sy,
                       uint8_t r, uint8_t g, uint8_t b, float maxViewAngle)
{
    if (!g_cfg.enabled || !pos)
    {
        Store_orig(veh, id, tex, pos, fx, fy, sx, sy, r, g, b, maxViewAngle);
        return;
    }

    CVector p = *pos;

    // length: stretch the front vector, keep the edge nearest to the car where it is
    p.x += fx * (g_cfg.length - 1.0f);
    p.y += fy * (g_cfg.length - 1.0f);
    fx *= g_cfg.length;
    fy *= g_cfg.length;

    // width
    sx *= g_cfg.width;
    sy *= g_cfg.width;

    // height
    p.z += g_cfg.height;

    // brightness
    r = Clamp255(r * g_cfg.brightness);
    g = Clamp255(g * g_cfg.brightness);
    b = Clamp255(b * g_cfg.brightness);

    Store_orig(veh, id, tex, &p, fx, fy, sx, sy, r, g, b, maxViewAngle);
}

extern "C" void OnModLoad()
{
    logger->SetTag("HeadlightTuner");

    uintptr_t base = aml->GetLib("libGTASA.so");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!base || !h) { logger->Error("libGTASA.so not found"); return; }

    LoadCfg();

    uintptr_t addr = (uintptr_t)aml->GetSym(h, "_ZN8CShadows19StoreCarLightShadowEP8CVehicleiP9RwTextureP7CVectorffffhhhf");
    if (!addr) { logger->Error("StoreCarLightShadow not found, plugin disabled"); return; }

    aml->Hook((void*)addr, (void*)Store_hook, (void**)&Store_orig);
    logger->Info("HeadlightTuner loaded (width %.2f length %.2f brightness %.2f height %.2f)",
                 g_cfg.width, g_cfg.length, g_cfg.brightness, g_cfg.height);
}

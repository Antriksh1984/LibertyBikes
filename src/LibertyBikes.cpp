// LibertyBikes — real Vice City motorbikes for GTA III (v1.0).
// Bike physics/rider logic ported from reVC (https://github.com/mrxenginner/reVC).
// reVC licence: LICENSE-reVC.txt. Built with plugin-sdk (Plugin-SDK-LICENSE.txt).
#include "plugin.h"
#include "Bike.h"
#include "Hook.h"
#include "common.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CPad.h"
#include "CTimer.h"
#include "CWorld.h"
#include "CModelInfo.h"
#include "CVehicleModelInfo.h"
#include "CAnimManager.h"
#include "CAnimBlendAssocGroup.h"
#include "CAnimBlendAssociation.h"
#include "CAnimBlendHierarchy.h"
#include "CAnimBlendSequence.h"
#include "RpAnimBlend.h"
#include "cHandlingDataMgr.h"
#include "CText.h"
#include "CStreaming.h"
#include "CPools.h"
#include "CMenuManager.h"
#include "CWeaponInfo.h"
#include "CCamera.h"
#include "CGeneral.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace plugin;
extern "C" IMAGE_DOS_HEADER __ImageBase;

// ------------------------------------------------------------------------------------------
// gta3.exe v1.0 addresses not covered (or only covered virtually) by plugin-sdk
namespace addr {
constexpr uintptr_t CVehicleVtbl = 0x6028A8;            // 35 entries
constexpr uintptr_t CAutomobileCtor = 0x52C6B0;
constexpr uintptr_t AudioProcessVehicle = 0x569A00;     // cAudioManager::ProcessVehicle(CVehicle*)
constexpr uintptr_t PedSetObjective = 0x4D83E0;         // CPed::SetObjective(eObjective, void*)
constexpr uintptr_t PedSetPositionInCar = 0x4D4970;
constexpr uintptr_t PedWarpIntoCar = 0x4D7D20;
constexpr uintptr_t AnimLoadAnimFiles = 0x4038F0;
constexpr uintptr_t AnimShutdown = 0x4033B0;
constexpr uintptr_t TextGet = 0x52C5A0;
constexpr uintptr_t ConvertDataToGameUnits = 0x546BB0;  // cHandlingDataMgr::ConvertDataToGameUnits(tHandlingData*)
constexpr uintptr_t AnimHierCtor = 0x401960, AnimSeqCtor = 0x4022D0, AnimAssocCtor = 0x401460;
}

static void Log(const char *fmt, ...) {
    static FILE *f = nullptr;
    if (!f) f = fopen("LibertyBikes.log", "w");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f);
}

// ------------------------------------------------------------------------------------------
// Bike definitions (LibertyBikes.ini)
static std::vector<BikeModelDef *> g_defs;
static int g_spawnKey = VK_F4;
static std::wstring g_names[16];

const BikeModelDef *bikes::DefForModel(int modelId) {
    for (auto *d : g_defs) if (d->modelId == modelId) return d;
    return nullptr;
}

static std::string IniPath() {
    char buf[MAX_PATH];
    GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), buf, MAX_PATH);
    std::string p(buf);
    return p.substr(0, p.find_last_of("\\/") + 1) + "LibertyBikes.ini";
}
static std::string DataDir() {
    std::string p = IniPath();
    return p.substr(0, p.find_last_of("\\/") + 1) + "LibertyBikes\\";
}

// VC handling.cfg line -> III tHandlingData (raw units), then the game's own conversion.
static bool ParseHandling(const char *line, tHandlingData &h) {
    std::vector<std::string> t;
    char buf[512]; strncpy(buf, line, 511); buf[511] = 0;
    for (char *tok = strtok(buf, " \t"); tok; tok = strtok(nullptr, " \t")) t.push_back(tok);
    if (t.size() == 33) t.erase(t.begin() + 29);          // drop VC's suspension anti-dive column
    if (t.size() != 32) { Log("Handling line has %d fields, expected 32/33", (int)t.size()); return false; }
    memset(&h, 0, sizeof(h));
    h.m_nHandlingId = 0;
    h.m_fMass = (float)atof(t[1].c_str());
    h.m_vDimensions = CVector((float)atof(t[2].c_str()), (float)atof(t[3].c_str()), (float)atof(t[4].c_str()));
    h.m_vCentreOfMass = CVector((float)atof(t[5].c_str()), (float)atof(t[6].c_str()), (float)atof(t[7].c_str()));
    h.m_nPercentSubmerged = (unsigned char)atoi(t[8].c_str());
    h.m_fTractionMultiplier = (float)atof(t[9].c_str());
    h.m_fTractionLoss = (float)atof(t[10].c_str());
    h.m_fTractionBias = (float)atof(t[11].c_str());
    h.m_transmissionData.m_nNumberOfGears = (unsigned char)atoi(t[12].c_str());
    h.m_transmissionData.m_fMaxForwardsVelocity = (float)atof(t[13].c_str());
    h.m_transmissionData.m_fEngineAcceleration = (float)atof(t[14].c_str()) * 0.4f;   // VC loader scales this
    h.m_transmissionData.m_nDriveType = (unsigned char)t[15][0];
    h.m_transmissionData.m_nEngineType = (unsigned char)t[16][0];
    h.m_fBrakeDeceleration = (float)atof(t[17].c_str());
    h.m_fBrakeBias = (float)atof(t[18].c_str());
    h.m_bABS = atoi(t[19].c_str()) != 0;
    h.m_fSteeringLock = (float)atof(t[20].c_str());
    h.m_fSuspensionForceLevel = (float)atof(t[21].c_str());
    h.m_fSuspensionDampingLevel = (float)atof(t[22].c_str());
    h.m_fSeatOffsetDistance = (float)atof(t[23].c_str());
    h.m_fCollisionDamageMultiplier = (float)atof(t[24].c_str());
    h.m_nMonetaryValue = (unsigned int)atoi(t[25].c_str());
    h.m_fSuspensionUpperLimit = (float)atof(t[26].c_str());
    h.m_fSuspensionLowerLimit = (float)atof(t[27].c_str());
    h.m_fSuspensionBiasBetweenFrontAndRear = (float)atof(t[28].c_str());
    unsigned int flags = 0; sscanf(t[29].c_str(), "%x", &flags);
    *reinterpret_cast<unsigned int *>(&h.m_nModelFlags) = flags & 0xFFFF;     // III only knows the low 16 bits
    h.m_transmissionData.m_nHandlingFlags.m_b1gBoost = flags & 1;
    h.m_transmissionData.m_nHandlingFlags.m_b2gBoost = (flags >> 1) & 1;
    h.m_nFrontLights = (eVehicleLightsSize)atoi(t[30].c_str());
    h.m_nRearLights = (eVehicleLightsSize)atoi(t[31].c_str());
    CallMethod<addr::ConvertDataToGameUnits, cHandlingDataMgr *, tHandlingData *>(&mod_HandlingManager, &h);
    h.m_transmissionData.m_fMaxBackwardsVelocity = -0.05f;      // VC: bikes reverse slowly
    return true;
}

static bool ParseBikeHandling(const char *line, BikeHandling &b) {
    float v[15];
    int n = sscanf(line, "%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                   &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14]);
    if (n != 15) { Log("BikeHandling has %d values, expected 15", n); return false; }
    b.fLeanFwdCOM = v[0]; b.fLeanFwdForce = v[1]; b.fLeanBakCOM = v[2]; b.fLeanBackForce = v[3];
    b.fMaxLean = sinf(DegToRad(v[4])); b.fFullAnimLean = DegToRad(v[5]); b.fDesLean = v[6];
    b.fSpeedSteer = v[7]; b.fSlipSteer = v[8]; b.fNoPlayerCOMz = v[9];
    b.fWheelieAng = sinf(DegToRad(v[10])); b.fStoppieAng = sinf(DegToRad(v[11]));
    b.fWheelieSteer = v[12]; b.fWheelieStabMult = v[13]; b.fStoppieStabMult = v[14];
    return true;
}

static void LoadDefinitions() {
    std::string ini = IniPath();
    char list[256];
    GetPrivateProfileStringA("Bikes", "Models", "PCJ600", list, sizeof(list), ini.c_str());
    char key[32];
    GetPrivateProfileStringA("Bikes", "SpawnKey", "0x73", key, sizeof(key), ini.c_str());
    g_spawnKey = (int)strtol(key, nullptr, 0);
    int idx = 0;
    for (char *sec = strtok(list, ", "); sec && idx < 16; sec = strtok(nullptr, ", "), idx++) {
        auto *d = new BikeModelDef();
        char buf[512];
        d->modelId = GetPrivateProfileIntA(sec, "ModelId", -1, ini.c_str());
        d->audioDonorModel = GetPrivateProfileIntA(sec, "AudioDonorModel", 111, ini.c_str());
        GetPrivateProfileStringA(sec, "GxtKey", sec, buf, sizeof(buf), ini.c_str());
        strncpy(d->gxtKey, buf, 7); d->gxtKey[7] = 0;
        GetPrivateProfileStringA(sec, "Name", sec, buf, sizeof(buf), ini.c_str());
        g_names[idx] = std::wstring(buf, buf + strlen(buf));
        d->displayName = g_names[idx].c_str();
        GetPrivateProfileStringA(sec, "Handling", "", buf, sizeof(buf), ini.c_str());
        bool ok = d->modelId > 0 && ParseHandling(buf, d->handling);
        GetPrivateProfileStringA(sec, "BikeHandling", "", buf, sizeof(buf), ini.c_str());
        ok = ok && ParseBikeHandling(buf, d->bike);
        if (!ok) { Log("[%s] skipped (bad or missing data)", sec); delete d; continue; }
        g_defs.push_back(d);
        Log("[%s] model %d, mass %.0f, steer lock %.1f", sec, d->modelId, d->handling.m_fMass, d->handling.m_fSteeringLock);
    }
}

// ------------------------------------------------------------------------------------------
// vtable
static void *g_vtbl[35];
void *bikes::Vtable() { return g_vtbl; }
bool bikes::IsBike(const CVehicle *v) {
    return v && v->m_nVehicleClass == VEHICLE_TYPE_BIKE && *reinterpret_cast<void *const *>(v) == g_vtbl;
}

using DtorFn = void *(__thiscall *)(void *, int);
static void *__fastcall V_Dtor(CBike *b, void *, int flags) {
    b->Destruct();
    return reinterpret_cast<DtorFn>(reinterpret_cast<void **>(addr::CVehicleVtbl)[0])(b, flags);
}
static void __fastcall V_SetModelIndex(CBike *b, void *, unsigned int id) { b->SetModelIndexBike(id); }
static void __fastcall V_ProcessControl(CBike *b) { b->ProcessControlBike(); }
static void __fastcall V_Teleport(CBike *b, void *, CVector pos) { b->TeleportBike(pos); }
static void __fastcall V_PreRender(CBike *b) { b->PreRenderBike(); }
static void __fastcall V_Render(CBike *b) { b->RenderBike(); }
static int __fastcall V_ProcessEntityCollision(CBike *b, void *, CEntity *ent, CColPoint *cp) { return b->ProcessEntityCollisionBike(ent, cp); }
static void __fastcall V_ProcessControlInputs(CBike *b, void *, unsigned char pad) { b->ProcessControlInputsBike(pad); }
static void __fastcall V_GetComponentWorldPosition(CBike *b, void *, int c, CVector &pos) { b->GetComponentWorldPositionBike(c, pos); }
static bool __fastcall V_IsComponentPresent(CBike *b, void *, int c) { return c > 0 && c < BIKE_NUM_NODES && b->Ext()->nodes[c]; }
static void __fastcall V_SetComponentRotation(CBike *, void *, int, CVector) {}
static void __fastcall V_OpenDoor(CBike *, void *, int, int, float) {}
static void __fastcall V_ProcessOpenDoor(CBike *, void *, unsigned int, unsigned int, float) {}
static bool __fastcall V_IsDoorReady(CBike *, void *, int) { return true; }
static bool __fastcall V_IsDoorFullyOpen(CBike *, void *, int) { return false; }
static bool __fastcall V_IsDoorClosed(CBike *, void *, int) { return false; }
static bool __fastcall V_IsDoorMissing(CBike *, void *, int) { return true; }
static void __fastcall V_RemoveRefsToVehicle(CBike *b, void *, CEntity *e) { b->RemoveRefsToVehicleBike(e); }
static void __fastcall V_BlowUpCar(CBike *b, void *, CEntity *culprit) { b->BlowUpCarBike(culprit); }
static bool __fastcall V_SetUpWheelColModel(CBike *b, void *, CColModel *c) { return b->SetUpWheelColModelBike(c); }
static void __fastcall V_BurstTyre(CBike *b, void *, unsigned char w) { b->BurstTyreBike(w); }
static bool __fastcall V_IsRoomForPedToLeaveCar(CBike *b, void *, unsigned int c, CVector *o) { return b->IsRoomForPedToLeaveCarBike(c, o); }
static float __fastcall V_GetHeightAboveRoad(CBike *b) { return b->GetHeightAboveRoadBike(); }
static void __fastcall V_PlayCarHorn(CBike *) {}

void bikes::InstallVtable() {
    memcpy(g_vtbl, reinterpret_cast<void *>(addr::CVehicleVtbl), sizeof(g_vtbl));
    g_vtbl[0] = (void *)V_Dtor;              g_vtbl[3] = (void *)V_SetModelIndex;
    g_vtbl[8] = (void *)V_ProcessControl;    g_vtbl[11] = (void *)V_Teleport;
    g_vtbl[12] = (void *)V_PreRender;        g_vtbl[13] = (void *)V_Render;
    g_vtbl[17] = (void *)V_ProcessEntityCollision;
    g_vtbl[18] = (void *)V_ProcessControlInputs;
    g_vtbl[19] = (void *)V_GetComponentWorldPosition;
    g_vtbl[20] = (void *)V_IsComponentPresent; g_vtbl[21] = (void *)V_SetComponentRotation;
    g_vtbl[22] = (void *)V_OpenDoor;          g_vtbl[23] = (void *)V_ProcessOpenDoor;
    g_vtbl[24] = (void *)V_IsDoorReady;       g_vtbl[25] = (void *)V_IsDoorFullyOpen;
    g_vtbl[26] = (void *)V_IsDoorClosed;      g_vtbl[27] = (void *)V_IsDoorMissing;
    g_vtbl[28] = (void *)V_RemoveRefsToVehicle; g_vtbl[29] = (void *)V_BlowUpCar;
    g_vtbl[30] = (void *)V_SetUpWheelColModel; g_vtbl[31] = (void *)V_BurstTyre;
    g_vtbl[32] = (void *)V_IsRoomForPedToLeaveCar; g_vtbl[33] = (void *)V_GetHeightAboveRoad;
    g_vtbl[34] = (void *)V_PlayCarHorn;
}

// ------------------------------------------------------------------------------------------
// Animations: VC "bikes" block (retargeted to III's skeleton by tools/retarget_ifp.py)
enum BikeAnim { BA_BACK, BA_DRIVEBY_FT, BA_DRIVEBY_LHS, BA_DRIVEBY_RHS, BA_FWD, BA_GETOFF_BACK, BA_GETOFF_LHS,
                BA_GETOFF_RHS, BA_HIT, BA_JUMPON_L, BA_JUMPON_R, BA_KICK, BA_LEFT, BA_PASSENGER, BA_PUSHES,
                BA_RIDE, BA_RIGHT, BA_STILL, BA_FALLOFF, BA_FALLBACK, BA_COUNT };
static const char *kAnimNames[BA_COUNT] = {
    "BIKEs_Back", "BIKEs_drivebyFT", "BIKEs_drivebyLHS", "BIKEs_drivebyRHS", "BIKEs_Fwd", "BIKEs_getoffBACK",
    "BIKEs_getoffLHS", "BIKEs_getoffRHS", "BIKEs_hit", "BIKEs_jumponL", "BIKEs_jumponR", "BIKEs_kick",
    "BIKEs_Left", "BIKEs_passenger", "BIKEs_pushes", "BIKEs_Ride", "BIKEs_Right", "BIKEs_Still",
    "BIKE_fall_off", "BIKE_fallR" };   // the last two come from VC's ped.ifp
// flags from reVC's aBikeAnimDescs (ASSOC_DRIVING has no III equivalent)
static const int kAnimFlags[BA_COUNT] = {
    ASSOC_PARTIAL, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL,
    ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL, ASSOC_PARTIAL, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION,
    ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL,
    ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL, ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL,
    ASSOC_PARTIAL, ASSOC_DELETEFADEDOUT, ASSOC_REPEAT | ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL,
    ASSOC_DELETEFADEDOUT, ASSOC_PARTIAL, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL,
    ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION, ASSOC_DELETEFADEDOUT | ASSOC_PARTIAL | ASSOC_HAS_TRANSLATION };
constexpr int BIKE_ANIM_GROUP = 25;     // III has groups 0..24
constexpr int BIKE_ANIM_BASE = 200;     // anim ids above every III ped anim id
static inline int AnimId(int a) { return BIKE_ANIM_BASE + a; }

static CAnimBlendHierarchy *g_hier[BA_COUNT];
static CAnimBlendAssociation *g_assocs;     // BA_COUNT base associations
static CAnimBlendAssocGroup *g_origGroups, *g_groups;
static CVector g_pelvisStart[BA_COUNT], g_pelvisEnd[BA_COUNT];
static bool g_animsReady;

static uint32_t RdU32(const std::vector<char> &d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }

static bool LoadBikeIfp(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { Log("Missing %s", path.c_str()); return false; }
    std::vector<char> d;
    fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f); fclose(f);
    auto pad = [](uint32_t s) { return (s + 3) & ~3u; };
    if (d.size() < 16 || memcmp(&d[0], "ANPK", 4)) return false;
    size_t end = 8 + RdU32(d, 4);
    size_t p = 16 + pad(RdU32(d, 12));      // skip INFO
    int current = -1, loaded = 0;
    while (p + 8 <= end) {
        uint32_t size = RdU32(d, p + 4);
        if (!memcmp(&d[p], "NAME", 4)) {
            const char *name = &d[p + 8];
            current = -1;
            for (int i = 0; i < BA_COUNT; i++) if (!_stricmp(name, kAnimNames[i])) current = i;
            p += 8 + pad(size);
            continue;
        }
        if (!memcmp(&d[p], "DGAN", 4) && current >= 0) {
            size_t q = p + 8, dend = p + 8 + size;
            int numSeq = (int)RdU32(d, q + 8);
            q += 8 + pad(RdU32(d, q + 4));
            auto *hier = static_cast<CAnimBlendHierarchy *>(operator new(sizeof(CAnimBlendHierarchy)));
            CallMethod<addr::AnimHierCtor, CAnimBlendHierarchy *>(hier);
            hier->SetName(kAnimNames[current]);
            char *seqMem = static_cast<char *>(operator new(sizeof(CAnimBlendSequence) * numSeq));
            auto *seqs = reinterpret_cast<CAnimBlendSequence *>(seqMem);
            for (int s = 0; s < numSeq && q < dend; s++) {
                CAnimBlendSequence *seq = &seqs[s];
                CallMethod<addr::AnimSeqCtor, CAnimBlendSequence *>(seq);
                size_t a = q + 8;                       // ANIM chunk inside CPAN
                uint32_t animSize = RdU32(d, a + 4);
                seq->SetName(&d[a + 8]);
                int numFrames = (int)RdU32(d, a + 8 + 28);
                size_t k = a + 8 + animSize;
                bool trans = !memcmp(&d[k], "KRT0", 4);
                uint32_t kSize = RdU32(d, k + 4);
                seq->SetNumFrames(numFrames, trans);
                const float *fb = reinterpret_cast<const float *>(&d[k + 8]);
                float prevTime = 0.0f;   // IFP stores absolute times; III keyframes hold the gap to the previous key
                for (int i = 0; i < numFrames; i++) {
                    KeyFrame *kf = seq->GetKeyFrame(i);
                    const float *v = fb + i * (trans ? 8 : 5);
                    kf->rotation.imag = CVector(-v[0], -v[1], -v[2]);
                    kf->rotation.real = v[3];
                    if (trans) {
                        static_cast<KeyFrameTrans *>(kf)->translation = CVector(v[4], v[5], v[6]);
                    }
                    float t = trans ? v[7] : v[4];
                    kf->deltaTime = t - prevTime;
                    prevTime = t;
                }
                if (trans && !_stricmp(seq->szName, "Swaist") && numFrames > 0) {
                    g_pelvisStart[current] = static_cast<KeyFrameTrans *>(seq->GetKeyFrame(0))->translation;
                    g_pelvisEnd[current] = static_cast<KeyFrameTrans *>(seq->GetKeyFrame(numFrames - 1))->translation;
                }
                q = k + 8 + kSize;
            }
            hier->m_pAnimBlendSequence = seqs;
            hier->m_nNumSequences = (short)numSeq;
            hier->m_nCompressed = 0;
            hier->linkPtr = nullptr;
            hier->RemoveQuaternionFlips();
            hier->CalcTotalTime();
            g_hier[current] = hier;
            loaded++;
            p = dend;
            continue;
        }
        p += 8 + pad(size);
    }
    Log("Loaded %d/%d bike animations", loaded, BA_COUNT);
    return loaded == BA_COUNT;
}

static void InstallAnimGroup() {
    if (!g_hier[BA_RIDE]) return;
    // Bind the hierarchies to a ped clump, like CAnimManager::LoadAnimFiles does for III's groups
    int mi = CAnimManager::ms_aAnimAssocDefinitions[0].modelIndex;
    CBaseModelInfo *info = CModelInfo::ms_modelInfoPtrs[mi];
    RpClump *clump = info ? reinterpret_cast<RpClump *>(info->CreateInstance()) : nullptr;
    if (!clump) { Log("No ped clump (model %d) to build bike anims", mi); return; }
    RpAnimBlendClumpInit(clump);
    char *mem = static_cast<char *>(operator new(sizeof(CAnimBlendAssociation) * BA_COUNT));
    g_assocs = reinterpret_cast<CAnimBlendAssociation *>(mem);
    for (int i = 0; i < BA_COUNT; i++) {
        CAnimBlendAssociation *a = &g_assocs[i];
        CallMethod<addr::AnimAssocCtor, CAnimBlendAssociation *>(a);
        a->Init(clump, g_hier[i]);
        a->m_nAnimID = AnimId(i);
        a->m_nFlags = kAnimFlags[i];
    }
    RpClumpDestroy(clump);

    // Extend III's group array by one: group 25 = bikes. GetAnimation(id) indexes assocList[id]
    // without a bounds check, so offset the list pointer by the base id.
    g_origGroups = CAnimManager::ms_aAnimAssocGroups;
    g_groups = static_cast<CAnimBlendAssocGroup *>(calloc(BIKE_ANIM_GROUP + 1, sizeof(CAnimBlendAssocGroup)));
    memcpy(g_groups, g_origGroups, sizeof(CAnimBlendAssocGroup) * BIKE_ANIM_GROUP);
    g_groups[BIKE_ANIM_GROUP].m_pAssociations =
        reinterpret_cast<CAnimBlendAssociation *>(mem - BIKE_ANIM_BASE * sizeof(CAnimBlendAssociation));
    g_groups[BIKE_ANIM_GROUP].m_nNumAssociations = BIKE_ANIM_BASE + BA_COUNT;
    CAnimManager::ms_aAnimAssocGroups = g_groups;
    g_animsReady = true;
    Log("Bike anim group installed as group %d", BIKE_ANIM_GROUP);
}

static void *g_origLoadAnimFiles, *g_origAnimShutdown;
static void Hook_LoadAnimFiles() {
    reinterpret_cast<void (*)()>(g_origLoadAnimFiles)();
    g_animsReady = false;
    if (!g_hier[BA_RIDE]) LoadBikeIfp(DataDir() + "bikes.ifp");
    InstallAnimGroup();
}
static void Hook_AnimShutdown() {
    if (g_groups && CAnimManager::ms_aAnimAssocGroups == g_groups) {
        CAnimManager::ms_aAnimAssocGroups = g_origGroups;   // the game delete[]s its own array
        free(g_groups); g_groups = nullptr; g_animsReady = false;
    }
    reinterpret_cast<void (*)()>(g_origAnimShutdown)();
}

static CAnimBlendAssociation *GetAssoc(CPed *ped, int a) { return RpAnimBlendClumpGetAssociation(ped->m_pRwClump, AnimId(a)); }
static CAnimBlendAssociation *Blend(CPed *ped, int a, float delta) {
    return CAnimManager::BlendAnimation(ped->m_pRwClump, BIKE_ANIM_GROUP, AnimId(a), delta);
}
static CAnimBlendAssociation *Add(CPed *ped, int a) {
    return CAnimManager::AddAnimation(ped->m_pRwClump, BIKE_ANIM_GROUP, AnimId(a));
}
static void RemoveBikeAnims(CPed *ped) {
    for (int a = 0; a < BA_COUNT; a++)
        while (CAnimBlendAssociation *as = GetAssoc(ped, a)) {
            if (ped->m_pVehicleAnim == as) ped->m_pVehicleAnim = nullptr;
            delete as;   // virtual: runs the game's own deleting destructor
        }
}

// ------------------------------------------------------------------------------------------
// Rider: get on / get off (player) and the lean animations while riding
using SetObjectiveFn = void(__thiscall *)(CPed *, int, void *);
static SetObjectiveFn g_origSetObjective;
static bool g_objectivePassThrough;

struct RiderAction {
    CPed *ped = nullptr;
    CBike *bike = nullptr;
    int anim = -1;
    bool mounting = false;
    unsigned int start = 0;
    CVector root;
    float heading = 0.0f;
    unsigned char savedControls = 0;
};
static RiderAction g_action;
static bool g_driveByModelShown;   // Uzi model attached for a bike drive-by

static void UpdatePedFrame(CPed *ped) {
    ped->m_matrix.UpdateRW();
    if (ped->m_pRwObject) RwFrameUpdateObjects(static_cast<RwFrame *>(rwObjectGetParent(ped->m_pRwObject)));
}

static CVector SeatWorldPos(CBike *bike) {
    CVehicleModelInfo *mi = (CVehicleModelInfo *)CModelInfo::ms_modelInfoPtrs[bike->m_nModelIndex];
    bike->CalculateLeanMatrix();
    CMatrix &m = bike->Ext()->leanMatrix;
    return m.pos + Multiply3x3(m, mi->m_vecDummyPos[2]);   // ped_frontseat
}

// Root position that makes the pelvis of `anim` at `t` (start or end) line up with the riding pelvis
static CVector RootForAnim(CBike *bike, int anim, bool atStart) {
    CVector pel = atStart ? g_pelvisStart[anim] : g_pelvisEnd[anim];
    CVector ride = g_pelvisStart[BA_RIDE];
    CVector local = ride - pel;
    return SeatWorldPos(bike) + Multiply3x3(bike->m_matrix, local);
}

static void ClearAction() {
    if (g_action.ped && g_action.ped->IsPlayer()) CPad::GetPad(0)->DisablePlayerControls = g_action.savedControls;
    if (g_action.ped) CleanUpRef(g_action.ped, reinterpret_cast<CEntity **>(&g_action.ped));
    if (g_action.bike) {
        g_action.bike->Ext()->beingPickedUp = false;
        CleanUpRef(g_action.bike, reinterpret_cast<CEntity **>(&g_action.bike));
    }
    g_action = RiderAction();
}

static void BeginAction(CPed *ped, CBike *bike, int anim, bool mounting) {
    ClearAction();
    g_action.ped = ped; ped->RegisterReference(reinterpret_cast<CEntity **>(&g_action.ped));
    g_action.bike = bike; bike->RegisterReference(reinterpret_cast<CEntity **>(&g_action.bike));
    g_action.anim = anim;
    g_action.mounting = mounting;
    g_action.start = CTimer::m_snTimeInMilliseconds;
    g_action.heading = bike->m_matrix.GetForward().Heading();
    if (ped->IsPlayer()) {
        g_action.savedControls = CPad::GetPad(0)->DisablePlayerControls;
        CPad::GetPad(0)->DisablePlayerControls = 1;
    }
}

static void PlacePed(CPed *ped, const CVector &pos, float heading) {
    ped->m_fHeadingCurrent = ped->m_fHeadingGoal = heading;
    ped->SetHeading(heading);
    ped->m_matrix.pos = pos;
    ped->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
    UpdatePedFrame(ped);
}

static void StartMount(CPed *ped, CBike *bike) {
    if (!g_animsReady || g_action.ped) return;
    if (ped->m_bInVehicle || bike->m_pDriver || bike->m_nStatus == STATUS_WRECKED) return;
    bool pedOnRight = Dot(ped->m_matrix.pos - bike->m_matrix.pos, bike->m_matrix.right) > 0.0f;
    int anim = pedOnRight ? BA_JUMPON_L : BA_JUMPON_R;   // "L": the bike ends up on the ped's left
    BeginAction(ped, bike, anim, true);
    bike->Ext()->beingPickedUp = true;
    bike->Ext()->isStanding = false;
    ped->bUsesCollision = false;
    ped->bAffectedByGravity = false;   // held in place by us while the jump-on anim plays
    ped->SetMoveState(PEDMOVE_STILL);
    g_action.root = RootForAnim(bike, anim, false);
    PlacePed(ped, g_action.root, g_action.heading);
    Blend(ped, anim, 8.0f);
}

static void FinishMount() {
    CPed *ped = g_action.ped; CBike *bike = g_action.bike;
    if (CAnimBlendAssociation *a = GetAssoc(ped, g_action.anim)) delete a;
    ClearAction();
    if (ped) ped->bAffectedByGravity = true;
    if (!ped || !bike || bike->m_pDriver || bike->m_nStatus == STATUS_WRECKED || ped->m_fHealth <= 0.0f) {
        if (ped) ped->bUsesCollision = true;
        return;
    }
    // same sequence the WARP_PLAYER_INTO_CAR script command uses
    g_objectivePassThrough = true;
    g_origSetObjective(ped, 15 /* OBJECTIVE_ENTER_CAR_AS_DRIVER */, bike);
    g_objectivePassThrough = false;
    CallMethod<addr::PedWarpIntoCar, CPed *, CVehicle *>(ped, bike);
    // swap III's car-sitting anim for the bike riding anim
    if (ped->m_pVehicleAnim) { delete ped->m_pVehicleAnim; ped->m_pVehicleAnim = nullptr; }
    ped->m_pVehicleAnim = Blend(ped, BA_RIDE, 100.0f);
    BikeExt &e = *bike->Ext();
    e.pedLeanLR = e.pedLeanUD = 0.0f;
}

void bikes::DismountInstantly(CPed *ped, CBike *bike) {
    if (!ped || !bike) return;
    CVector pos = RootForAnim(bike, BA_GETOFF_RHS, false);
    if (bike->m_pDriver == ped) bike->RemoveDriver();
    if (bike->m_nStatus == STATUS_PLAYER) bike->m_nStatus = STATUS_ABANDONED;
    ped->m_bInVehicle = false;
    ped->m_ePedState = PEDSTATE_IDLE;
    ped->bUsesCollision = true;
    ped->bAffectedByGravity = true;
    ped->m_eObjective = (eObjective)0;
    RemoveBikeAnims(ped);
    g_driveByModelShown = false;
    // III's ReplaceWeaponWhenExitingVehicle isn't exposed by plugin-sdk: put the weapon back in hand
    if (CWeaponInfo *wi = CWeaponInfo::GetWeaponInfo(ped->GetWeapon()->m_eWeaponType)) {
        ped->RemoveWeaponModel(-1);
        if (wi->m_nModelId > 0) ped->AddWeaponModel(wi->m_nModelId);
    }
    CAnimManager::BlendAnimation(ped->m_pRwClump, 0 /* ASSOCGRP_STD */, 3 /* ANIM_IDLE_STANCE */, 100.0f);
    ped->SetMoveState(PEDMOVE_STILL);
    PlacePed(ped, pos, bike->m_matrix.GetForward().Heading());
}

static void StartDismount(CPed *ped, CBike *bike) {
    if (!g_animsReady || g_action.ped) return;
    float speed = fabsf(Dot(bike->m_vecMoveSpeed, bike->m_matrix.GetForward()));
    // Prefer stepping off to the left (bike on the ped's right); use the right side if blocked.
    int anim = BA_GETOFF_RHS;
    CVector seat = SeatWorldPos(bike);
    CVector left = RootForAnim(bike, BA_GETOFF_RHS, false);
    if (!CWorld::GetIsLineOfSightClear(seat, left, true, true, false, true, false, false, false)) anim = BA_GETOFF_LHS;
    CVector root = RootForAnim(bike, anim, false);
    bikes::DismountInstantly(ped, bike);
    PlacePed(ped, root, g_action.heading = bike->m_matrix.GetForward().Heading());
    if (speed < 0.05f) bike->Ext()->isStanding = true;     // leave it leaning on its stand
    BeginAction(ped, bike, anim, false);
    g_action.root = root;
    Blend(ped, anim, 8.0f);
}

// ------------------------------------------------------------------------------------------
// Falling off (port of VC's CBike::KnockOffRider)
static int StdAnim(const char *name) {
    CAnimBlendAssociation *a = CAnimManager::GetAnimAssociation(0, name);
    return a ? a->m_nAnimID : -1;
}

void bikes::KnockOffRider(CBike *bike, int weapon, int direction, CPed *ped) {
    if (!ped || !bike || !ped->m_bInVehicle || !g_animsReady) return;
    if (g_action.ped == ped) ClearAction();
    CVector bikeSpeed = bike->m_vecMoveSpeed;
    CVector seat = SeatWorldPos(bike);
    float heading = bike->m_matrix.GetForward().Heading();
    if (bike->m_matrix.GetUp().z < 0.0f) heading += PI_F;
    bikes::DismountInstantly(ped, bike);
    PlacePed(ped, seat, heading);   // thrown from the seat, not from beside the bike
    ped->m_vecMoveSpeed = bikeSpeed;

    float forceXY = -0.6f * bike->m_fDamageImpulse * ped->m_fMass / bike->m_fMass;
    const CVector &n = bike->m_vecDamageNormal;
    int anim = -1;
    bool bikeFall = false;
    direction &= 3;
    if (weapon == WEAPONTYPE_RAMMEDBYCAR) {
        ped->ApplyMoveForce(n.x * forceXY, n.y * forceXY, CGeneral::GetRandomNumberInRange(8.0f, 15.0f));
        static const char *names[4] = { "KO_skid_back", "KD_right", "KO_skid_front", "KD_left" };
        anim = StdAnim(names[direction]);
    } else {   // WEAPONTYPE_FALL
        ped->ApplyMoveForce(n.x * forceXY, n.y * forceXY, CGeneral::GetRandomNumberInRange(3.0f, 7.0f));
        static const char *names[4] = { "KO_skid_back", "KD_right", nullptr, "KD_left" };
        if (direction == 2) bikeFall = true; else anim = StdAnim(names[direction]);
    }
    ped->m_pCollidingEntity = bike;
    if (ped->m_fHealth > 0.0f) {
        if (bikeFall) { Blend(ped, BA_FALLBACK, 8.0f); anim = AnimId(BA_FALLBACK); }
        if (anim >= 0) ped->SetFall(1000, anim, 0);
    } else
        ped->SetDie(StdAnim("KO_skid_back"), 4.0f, 0.0f);
    ped->bIsStanding = false;

    CVector p = ped->m_matrix.pos;
    if (CWorld::TestSphereAgainstWorld(p + CVector(0.0f, 0.0f, 0.5f), 0.4f, nullptr, true, false, false, false, false, false) ||
        CWorld::TestSphereAgainstWorld(p + CVector(0.0f, 0.0f, 0.8f), 0.4f, nullptr, true, false, false, false, false, false)) {
        ped->m_pVehicle = bike;
        ped->PositionPedOutOfCollision();
    } else if (weapon == WEAPONTYPE_RAMMEDBYCAR &&
               !CWorld::TestSphereAgainstWorld(p + CVector(0.0f, 0.0f, 1.3f), 0.6f, nullptr, true, false, false, false, false, false))
        ped->m_matrix.pos.z += 0.5f;
    UpdatePedFrame(ped);
    bike->Ext()->isStanding = false;
}

// ------------------------------------------------------------------------------------------
// Drive-by (port of VC's CBike::DoDriveByShootings). Like III's cars, only the Uzi works.
static void StopDriveBy(CPed *ped) {
    for (int a : { BA_DRIVEBY_LHS, BA_DRIVEBY_RHS, BA_DRIVEBY_FT })
        if (CAnimBlendAssociation *as = GetAssoc(ped, a)) as->m_fBlendDelta = -1000.0f;
    if (g_driveByModelShown) { ped->RemoveWeaponModel(-1); g_driveByModelShown = false; }
}

static void DoDriveByShootings(CPed *ped, CBike *bike) {
    CWeapon *weapon = ped->GetWeapon();
    if (weapon->m_eWeaponType != WEAPONTYPE_UZI || CPad::GetPad(0)->DisablePlayerControls) { StopDriveBy(ped); return; }
    weapon->Update(ped->m_nAudioEntityId);
    CCam &cam = TheCamera.m_asCams[TheCamera.m_nActiveCam];
    bool left = cam.m_bLookingLeft, right = cam.m_bLookingRight;
    bool fire = CPad::GetPad(0)->GetCarGunFired();
    if (!(left || right || fire)) { weapon->Reload(); StopDriveBy(ped); return; }

    int want = left ? BA_DRIVEBY_LHS : right ? BA_DRIVEBY_RHS : BA_DRIVEBY_FT;
    for (int a : { BA_DRIVEBY_LHS, BA_DRIVEBY_RHS, BA_DRIVEBY_FT })
        if (a != want)
            if (CAnimBlendAssociation *as = GetAssoc(ped, a)) as->m_fBlendDelta = -1000.0f;
    CAnimBlendAssociation *anim = GetAssoc(ped, want);
    if (!anim || anim->m_fBlendDelta < 0.0f) anim = Add(ped, want);
    if (!g_driveByModelShown) {
        if (CWeaponInfo *wi = CWeaponInfo::GetWeaponInfo(WEAPONTYPE_UZI)) ped->AddWeaponModel(wi->m_nModelId);
        g_driveByModelShown = true;
    }
    // shoot once the gun is raised (the drive-by anim holds on its last frame)
    if ((!anim || !(anim->m_nFlags & ASSOC_RUNNING)) && fire && (unsigned)CTimer::m_snTimeInMilliseconds > weapon->m_nTimer) {
        if (left || right)
            weapon->FireFromCar(bike, left);
        else {
            CVector src = bike->m_matrix.pos + bike->m_matrix.GetForward() * 1.3f + bike->m_matrix.GetUp() * 0.5f;
            weapon->Fire(ped, &src);   // III aims a player's shot along the camera, i.e. ahead
        }
        weapon->m_nTimer = CTimer::m_snTimeInMilliseconds + 70;
    }
}

static void ProcessAction() {
    if (!g_action.ped) return;
    CPed *ped = g_action.ped; CBike *bike = g_action.bike;
    if (!bike || ped->m_fHealth <= 0.0f) { ped->bUsesCollision = true; ped->bAffectedByGravity = true; ClearAction(); return; }
    CAnimBlendAssociation *a = GetAssoc(ped, g_action.anim);
    bool done = !a || a->m_fCurrentTime >= a->m_pAnimBlendHierarchy->m_fTotalLength - 0.02f ||
                CTimer::m_snTimeInMilliseconds - g_action.start > 3000;
    if (g_action.mounting) {
        g_action.root = RootForAnim(bike, g_action.anim, false);   // follow the bike if it rolls
        PlacePed(ped, g_action.root, bike->m_matrix.GetForward().Heading());
        if (done) FinishMount();
    } else {
        PlacePed(ped, g_action.root, g_action.heading);
        if (done) { if (a) a->m_fBlendDelta = -4.0f; ClearAction(); }
    }
}

// Port of the bike branch of VC's CPed::DriveVehicle (lean animations).
static void DriveBike(CPed *ped, CBike *bike) {
    BikeExt &e = *bike->Ext();
    const BikeHandling &bh = bike->BH();
    float step = CTimer::ms_fTimeStep, stepNC = CTimer::ms_fTimeStepNonClipped;
    float blendDelta = 1.0f, targetUDLean = 0.0f;
    CAnimBlendAssociation *leftAssoc = GetAssoc(ped, BA_LEFT), *rightAssoc = GetAssoc(ped, BA_RIGHT);
    CAnimBlendAssociation *stillAssoc = GetAssoc(ped, BA_STILL);
    CAnimBlendAssociation *fwdAssoc = GetAssoc(ped, BA_FWD), *backAssoc = GetAssoc(ped, BA_BACK);
    CAnimBlendAssociation *walkbackAssoc = GetAssoc(ped, BA_PUSHES);
    bool player = ped->IsPlayer();
    float velFwd = Dot(bike->m_vecMoveSpeed, bike->m_matrix.GetForward());

    // VC: a violent spin or ending up upside down throws the rider off
    if (Len(bike->m_vecTurnSpeed) > 0.3f || bike->m_matrix.GetUp().z < -0.3f) {
        bikes::KnockOffRider(bike, WEAPONTYPE_FALL, 2, ped);
        return;
    }
    if (velFwd < 3.5f * bike->m_pHandlingData->m_transmissionData.m_fMaxBackwardsVelocity && bike->m_nWheelsOnGround) {
        bikes::KnockOffRider(bike, WEAPONTYPE_FALL, 2, ped);   // reversing far too fast
        return;
    }

    if (!GetAssoc(ped, BA_RIDE)) ped->m_pVehicleAnim = Blend(ped, BA_RIDE, 100.0f);
    CAnimBlendAssociation *drivebyAssoc = GetAssoc(ped, BA_DRIVEBY_LHS);
    if (!drivebyAssoc) drivebyAssoc = GetAssoc(ped, BA_DRIVEBY_RHS);
    if (!drivebyAssoc) drivebyAssoc = GetAssoc(ped, BA_DRIVEBY_FT);

    if (!drivebyAssoc && fabsf(velFwd) < 0.02f) {
        if (!stillAssoc || (stillAssoc->m_fBlendAmount < 1.0f && stillAssoc->m_fBlendDelta <= 0.0f))
            stillAssoc = Blend(ped, BA_STILL, 2.0f);
    } else if (velFwd >= 0.0f) {
        if (stillAssoc && stillAssoc->m_fBlendDelta >= 0.0f) stillAssoc->m_fBlendDelta = -4.0f;
        if (walkbackAssoc && walkbackAssoc->m_fBlendDelta >= 0.0f) walkbackAssoc->m_fBlendDelta = -4.0f;
    } else {
        float maxReverse = bike->m_pHandlingData->m_transmissionData.m_fMaxBackwardsVelocity;
        if (bike->m_fGasPedal >= 0.0f || velFwd <= maxReverse * 1.5f) {
            if (player && velFwd < maxReverse * 1.5f) targetUDLean = -1.0f;
            if (stillAssoc && stillAssoc->m_fBlendDelta >= 0.0f) stillAssoc->m_fBlendDelta = -4.0f;
            if (walkbackAssoc && walkbackAssoc->m_fBlendDelta >= 0.0f) walkbackAssoc->m_fBlendDelta = -4.0f;
        } else if (!walkbackAssoc || (walkbackAssoc->m_fBlendAmount < 1.0f && walkbackAssoc->m_fBlendDelta <= 0.0f))
            walkbackAssoc = Blend(ped, BA_PUSHES, 4.0f);
    }
    if (stillAssoc)
        blendDelta -= (std::min)(1.0f, stepNC * 0.02f * stillAssoc->m_fBlendDelta + stillAssoc->m_fBlendAmount);
    if (drivebyAssoc)
        blendDelta -= (std::min)(blendDelta, stepNC * 0.02f * drivebyAssoc->m_fBlendDelta + drivebyAssoc->m_fBlendAmount);
    if (walkbackAssoc)
        blendDelta -= (std::min)(blendDelta, stepNC * 0.02f * walkbackAssoc->m_fBlendDelta + walkbackAssoc->m_fBlendAmount);

    float targetLRLean = targetUDLean == -1.0f ? 0.0f : Clampf(e.leanLR / bh.fFullAnimLean, -1.0f, 1.0f);
    float timeBlend = powf(0.86f, step);
    e.pedLeanLR = e.pedLeanLR * timeBlend + (1.0f - timeBlend) * targetLRLean;

    if (!player) targetUDLean = 0.0f;
    else if (targetUDLean > -1.0f) {
        targetUDLean = e.leanInput;
        e.wheelieCam = false;
        float neededAngForWheelie = 1.0f;
        float *t = bike->m_afWheelTimer;
        if (t[0] != 0.0f || t[1] != 0.0f || bike->m_matrix.GetForward().z <= 0.0f || (t[2] == 0.0f && t[3] == 0.0f)) {
            if (t[2] == 0.0f && t[3] == 0.0f && bike->m_matrix.GetForward().z < 0.0f && (t[0] != 0.0f || t[1] != 0.0f))
                if (bh.fStoppieAng - bike->m_matrix.GetForward().z > 0.6f * bh.fStoppieAng) e.wheelieCam = true;
        } else {
            neededAngForWheelie = bh.fWheelieAng - bike->m_matrix.GetForward().z;
            if (neededAngForWheelie < bh.fWheelieAng / 2.0f) e.wheelieCam = true;
        }
        if (neededAngForWheelie >= 0.15f) {
            if (bike->m_fBrakePedal <= 0.5f || velFwd <= 0.01f) {
                if (bike->m_fGasPedal > 0.5f && targetUDLean <= 0.0f &&
                    0.3f * bike->m_pHandlingData->m_transmissionData.m_fMaxTrafficVelocity > velFwd)
                    targetUDLean = (std::min)(0.1f, targetUDLean);
            } else
                targetUDLean = (std::max)(0.1f, targetUDLean);
        } else
            targetUDLean = (std::max)(0.25f, targetUDLean);
        float absLR = fabsf(targetLRLean);
        if (absLR > 0.3f) targetUDLean *= (std::max)(0.0f, 1.0f - (absLR - 0.3f) * 50.0f / 13.0f);
    }
    if (player) {
        float tb = powf(0.89f, step);
        e.pedLeanUD = tb * e.pedLeanUD + (1.0f - tb) * targetUDLean;
    } else
        e.pedLeanUD = 0.0f;

    float fwdBack, leftRight;
    if (fabsf(e.pedLeanLR) <= 0.56f && player) {
        if (fabsf(e.pedLeanUD) <= 0.56f) {
            float mag = sqrtf(e.pedLeanLR * e.pedLeanLR + e.pedLeanUD * e.pedLeanUD);
            if (mag <= 0.01f) { fwdBack = fabsf(e.pedLeanUD); leftRight = fabsf(e.pedLeanLR); }
            else { fwdBack = fabsf(e.pedLeanUD / mag); leftRight = fabsf(e.pedLeanLR / mag); }
        } else { fwdBack = 1.0f; leftRight = 0.0f; }
    } else { fwdBack = 0.0f; leftRight = 1.0f; }
    float fwdBackBlend = fwdBack * blendDelta, leftRightBlend = leftRight * blendDelta;

    auto pose = [](CAnimBlendAssociation *as, float amount, float t) {
        as->m_fBlendAmount = amount;
        as->SetCurrentTime(t * as->m_pAnimBlendHierarchy->m_fTotalLength);
        as->m_nFlags &= ~ASSOC_RUNNING;
    };
    if (player) {
        if (!fwdAssoc) fwdAssoc = Add(ped, BA_FWD);
        if (!backAssoc) backAssoc = Add(ped, BA_BACK);
        if (fwdAssoc && backAssoc) {
            if (e.pedLeanUD < 0.0f) { pose(backAssoc, fwdBackBlend, -e.pedLeanUD); fwdAssoc->m_fBlendAmount = 0.0f; }
            else { pose(fwdAssoc, fwdBackBlend, e.pedLeanUD); backAssoc->m_fBlendAmount = 0.0f; }
        }
    }
    if (!leftAssoc) leftAssoc = Add(ped, BA_LEFT);
    if (!rightAssoc) rightAssoc = Add(ped, BA_RIGHT);
    if (leftAssoc && rightAssoc) {
        if (e.pedLeanLR < 0.0f) { pose(leftAssoc, leftRightBlend, -e.pedLeanLR); rightAssoc->m_fBlendAmount = 0.0f; }
        else { pose(rightAssoc, leftRightBlend, e.pedLeanLR); leftAssoc->m_fBlendAmount = 0.0f; }
    }
}

// ------------------------------------------------------------------------------------------
// Hooks
using AutoCtorFn = CAutomobile *(__thiscall *)(CAutomobile *, int, unsigned char);
static AutoCtorFn g_origAutoCtor;
static CAutomobile *__fastcall Hook_AutomobileCtor(CAutomobile *self, void *, int id, unsigned char createdBy) {
    if (const BikeModelDef *def = bikes::DefForModel(id)) {
        reinterpret_cast<CBike *>(self)->Construct(id, createdBy, def);
        return self;
    }
    return g_origAutoCtor(self, id, createdBy);
}

// III's car audio reads CAutomobile fields that bikes share, so let it treat a bike as a
// car with a donor model's sound set while it runs.
using AudioVehFn = void(__thiscall *)(void *, CVehicle *);
static AudioVehFn g_origAudioVeh;
static void __fastcall Hook_AudioProcessVehicle(void *audio, void *, CVehicle *veh) {
    if (bikes::IsBike(veh)) {
        CBike *b = static_cast<CBike *>(veh);
        short model = veh->m_nModelIndex;
        unsigned int type = veh->m_nVehicleClass;
        veh->m_nModelIndex = (short)b->Ext()->def->audioDonorModel;
        veh->m_nVehicleClass = 0;
        g_origAudioVeh(audio, veh);
        veh->m_nModelIndex = model;
        veh->m_nVehicleClass = type;
        return;
    }
    g_origAudioVeh(audio, veh);
}

static void __fastcall Hook_SetObjective(CPed *ped, void *, int objective, void *target) {
    if (!g_objectivePassThrough) {
        if ((objective == 15 || objective == 14) && target && bikes::IsBike(static_cast<CVehicle *>(target))) {
            // Only the player can ride for now; AI riders arrive in Milestone 3.
            if (objective == 15 && ped->IsPlayer()) StartMount(ped, static_cast<CBike *>(target));
            return;
        }
        if (objective == 13 /* OBJECTIVE_LEAVE_CAR */ && ped->m_bInVehicle && bikes::IsBike(ped->m_pVehicle)) {
            if (ped->IsPlayer()) StartDismount(ped, static_cast<CBike *>(ped->m_pVehicle));
            else bikes::DismountInstantly(ped, static_cast<CBike *>(ped->m_pVehicle));
            return;
        }
    }
    g_origSetObjective(ped, objective, target);
}

using PedVoidFn = void(__thiscall *)(CPed *);
static PedVoidFn g_origSetPedPositionInCar;
static void __fastcall Hook_SetPedPositionInCar(CPed *ped) {
    g_origSetPedPositionInCar(ped);
    if (ped->m_bInVehicle && bikes::IsBike(ped->m_pVehicle)) {
        CBike *bike = static_cast<CBike *>(ped->m_pVehicle);
        CVehicleModelInfo *mi = (CVehicleModelInfo *)CModelInfo::ms_modelInfoPtrs[bike->m_nModelIndex];
        bike->CalculateLeanMatrix();
        CMatrix m(bike->Ext()->leanMatrix);
        m.pos = m.pos + Multiply3x3(m, mi->m_vecDummyPos[2]);
        ped->m_matrix.CopyOnlyMatrix(m);
    }
}

using TextGetFn = wchar_t *(__thiscall *)(CText *, const char *);
static TextGetFn g_origTextGet;
static wchar_t *__fastcall Hook_TextGet(CText *text, void *, const char *key) {
    for (auto *d : g_defs)
        if (key && !_stricmp(key, d->gxtKey)) return const_cast<wchar_t *>(d->displayName);
    return g_origTextGet(text, key);
}

// Built-in test spawner: SpawnKey (default F4) puts the first bike 4 m in front of the player.
static bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static void SpawnTestBike() {
    CPlayerPed *player = FindPlayerPed();
    if (!player || player->m_bInVehicle || g_defs.empty() || FrontEndMenuManager.m_bMenuActive) return;
    int model = g_defs[0]->modelId;
    if (!CStreaming::HasModelLoaded(model)) {
        CStreaming::RequestModel(model, 0);
        CStreaming::LoadAllRequestedModels(false);
        if (!CStreaming::HasModelLoaded(model)) { Log("Spawn: model %d failed to load", model); return; }
    }
    auto *slot = reinterpret_cast<CAutomobile *>(CPools::ms_pVehiclePool->New());   // raw slot sized for CAutomobile
    if (!slot) { Log("Spawn: vehicle pool full"); return; }
    // runs through our CAutomobile ctor hook, so this becomes a CBike
    auto *bike = CallMethodAndReturn<CAutomobile *, addr::CAutomobileCtor, CAutomobile *, int, unsigned char>(slot, model, 1);
    CVector fwd = player->m_matrix.GetForward();
    CVector pos = player->m_matrix.pos + fwd * 4.0f;
    bool found = false;
    float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
    pos.z = (found ? ground : pos.z) + bike->GetHeightAboveRoad();
    bike->SetHeading(fwd.Heading());
    bike->m_matrix.pos = pos;
    bike->m_nStatus = STATUS_ABANDONED;
    bike->m_eDoorLock = CARLOCK_UNLOCKED;
    CWorld::Add(bike);
    Log("Spawned bike (model %d) at %.1f %.1f %.1f", model, pos.x, pos.y, pos.z);
}

static void PerFrame() {
    static bool keyWasDown = false;
    bool keyDown = (GetAsyncKeyState(g_spawnKey) & 0x8000) != 0;
    if (keyDown && !keyWasDown && GameHasFocus()) SpawnTestBike();
    keyWasDown = keyDown;

    ProcessAction();
    CPlayerPed *player = FindPlayerPed();
    if (player && player->m_bInVehicle && bikes::IsBike(player->m_pVehicle) && player->m_ePedState == PEDSTATE_DRIVING)
    {
        CBike *bike = static_cast<CBike *>(player->m_pVehicle);
        DriveBike(player, bike);
        if (player->m_bInVehicle && player->m_ePedState == PEDSTATE_DRIVING) DoDriveByShootings(player, bike);
    }
}

// ------------------------------------------------------------------------------------------
class LibertyBikes {
public:
    LibertyBikes() {
        LoadDefinitions();
        if (g_defs.empty()) { Log("No bikes defined; plugin idle."); return; }
        bikes::InstallVtable();
        g_origAutoCtor = hook::As<AutoCtorFn>(hook::Detour(addr::CAutomobileCtor, (void *)Hook_AutomobileCtor, 10,
                                                           "\x53\x56\x57\x55\x81\xEC\xA8\x00\x00\x00"));
        g_origAudioVeh = hook::As<AudioVehFn>(hook::Detour(addr::AudioProcessVehicle, (void *)Hook_AudioProcessVehicle, 5,
                                                           "\x53\x55\x83\xEC\x18"));
        g_origSetObjective = hook::As<SetObjectiveFn>(hook::Detour(addr::PedSetObjective, (void *)Hook_SetObjective, 9,
                                                                   "\x53\x89\xCB\x8B\x83\x24\x02\x00\x00"));
        g_origSetPedPositionInCar = hook::As<PedVoidFn>(hook::Detour(addr::PedSetPositionInCar, (void *)Hook_SetPedPositionInCar, 10,
                                                                     "\x53\x55\x30\xDB\x81\xEC\x40\x01\x00\x00"));
        g_origLoadAnimFiles = hook::Detour(addr::AnimLoadAnimFiles, (void *)Hook_LoadAnimFiles, 7, "\x53\x56\x57\x55\x83\xEC\x18");
        g_origAnimShutdown = hook::Detour(addr::AnimShutdown, (void *)Hook_AnimShutdown, 5, "\xB9\xDC\x14\x94\x00");
        g_origTextGet = hook::As<TextGetFn>(hook::Detour(addr::TextGet, (void *)Hook_TextGet, 5, "\x8B\x44\x24\x04\x50"));
        Log("Hooks: ctor=%d audio=%d objective=%d seat=%d anims=%d/%d text=%d", !!g_origAutoCtor, !!g_origAudioVeh,
            !!g_origSetObjective, !!g_origSetPedPositionInCar, !!g_origLoadAnimFiles, !!g_origAnimShutdown, !!g_origTextGet);
        if (!g_origAutoCtor || !g_origSetObjective || !g_origLoadAnimFiles || !g_origAnimShutdown) {
            Log("Required hook failed - unsupported gta3.exe? Bikes disabled.");
            g_defs.clear();
            return;
        }
        // III's CPhysical::ProcessCollision still has code for its cut bikes: between collision
        // sub-steps it resets "bike" suspension ratios at offsets 0x3C0..0x3CC of the old layout.
        // In ours those bytes are a wheel col point, which got corrupted every step (spins/launches).
        // Point the four stores at our suspension ratios (0x46C..0x478) instead, as VC does.
        bool patched = true;
        for (int i = 0; i < 4; i++) {
            uint32_t oldDisp = 0x3C0 + 4 * i, newDisp = 0x46C + 4 * i;
            patched &= hook::Patch(0x496C05 + 10 * i + 2, &newDisp, 4, &oldDisp);
        }
        Log("ProcessCollision bike suspension patch: %d", patched);
        Events::gameProcessEvent.after += PerFrame;
    }
} gLibertyBikes;

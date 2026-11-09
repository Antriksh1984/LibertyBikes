#pragma once
// CBike for GTA III: a port of reVC's CBike (src/vehicles/Bike.cpp) onto III's CVehicle.
//
// Memory layout: a bike lives in a normal vehicle-pool slot (sizeof CAutomobile = 0x5A8) and
// keeps CAutomobile's field layout for everything both classes share (wheel col points,
// suspension, wheel state, ground physicals...). That lets III's car audio code read a bike
// as if it were a car. Bike-only state lives in a heap BikeExt, pointed to from the unused
// m_aCarNodes[0] slot. A bike has m_nVehicleClass = 5 (VC's VEHICLE_TYPE_BIKE) and its own vtable.
#include "plugin.h"
#include "CAutomobile.h"
#include "CMatrix.h"
#include "CVector.h"
#include "CColPoint.h"
#include "CReferences.h"
#include "CDamageManager.h"

// III has CEntity::CleanUpOldReference but plugin-sdk doesn't expose it; same logic here.
inline void CleanUpRef(CEntity *ent, CEntity **slot) {
    if (!ent) return;
    CReference **link = &ent->m_pFirstRef;
    for (CReference *r = *link; r; r = *link) {
        if (r->m_ppEntity == slot) {
            *link = r->m_pNext;
            r->m_pNext = CReferences::pEmptyList;
            r->m_ppEntity = nullptr;
            CReferences::pEmptyList = r;
            return;
        }
        link = &r->m_pNext;
    }
}

// ---- small vector helpers (operators + - * / come from plugin-sdk's CVector.h) ----
inline float Dot(const CVector &a, const CVector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline CVector Cross(const CVector &a, const CVector &b) {
    return CVector(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float Len(const CVector &a) { return sqrtf(Dot(a, a)); }
inline CVector Norm(const CVector &a) { float l = Len(a); return l > 0.0f ? a / l : CVector(1.0f, 0.0f, 0.0f); }
template <typename T> inline T Clampf(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
constexpr float PI_F = 3.14159265f;
inline float DegToRad(float d) { return d * PI_F / 180.0f; }
constexpr float GRAVITY_III = 0.008f;

constexpr unsigned int VEHICLE_TYPE_BIKE = 5;

// VC's tBikeHandlingData ("!" lines in VC handling.cfg), already converted to game units.
struct BikeHandling {
    float fLeanFwdCOM, fLeanFwdForce, fLeanBakCOM, fLeanBackForce;
    float fMaxLean, fFullAnimLean, fDesLean, fSpeedSteer, fSlipSteer, fNoPlayerCOMz;
    float fWheelieAng, fStoppieAng, fWheelieSteer, fWheelieStabMult, fStoppieStabMult;
};

// Per-model bike definition (from LibertyBikes.ini).
struct BikeModelDef {
    int modelId = -1;
    tHandlingData handling{};    // III layout, converted by the game's own ConvertDataToGameUnits
    BikeHandling bike{};
    int audioDonorModel = 111;   // car whose engine sounds the bike borrows
    const wchar_t *displayName = L"PCJ-600";
    char gxtKey[8] = "PCJ600";
};

enum eBikeNodes { BIKE_NODE_NONE, BIKE_CHASSIS, BIKE_FORKS_FRONT, BIKE_FORKS_REAR, BIKE_WHEEL_FRONT,
                  BIKE_WHEEL_REAR, BIKE_MUDGUARD, BIKE_HANDLEBARS, BIKE_NUM_NODES };
enum { BIKEWHEEL_FRONT, BIKEWHEEL_REAR };
enum { BIKESUSP_F1, BIKESUSP_F2, BIKESUSP_R1, BIKESUSP_R2 };
// WHEEL_STATUS_OK/BURST/MISSING come from III's CDamageManager.h

struct BikeExt {
    const BikeModelDef *def = nullptr;
    RwFrame *nodes[BIKE_NUM_NODES] = {};
    bool leanMatrixClean = false;
    CMatrix leanMatrix;
    CVector avgSurfaceNormal{0.0f, 0.0f, 1.0f}, avgSurfaceRight{1.0f, 0.0f, 0.0f};
    unsigned char wheelStatus[2] = {};
    float wheelBasePosition[2] = {};
    float rearForkLength = 0.0f, frontForkY = 0.0f, frontForkZ = 0.0f, frontForkSlope = 0.0f;
    float wheelAngle = 0.0f, leanLR = 0.0f, leanLR2 = 0.0f, leanInput = 0.0f;
    float pedLeanLR = 0.0f, pedLeanUD = 0.0f;
    bool isStanding = false, extraSpeed = false, isOnFire = false, wheelieCam = false, beingPickedUp = false;
    short doingBurnout = 0;
    float tireTemperature = 1.0f, brakeDestabilization = 0.0f;
    bool suspensionReady = false;   // per-instance copy of VC's FAKESUSPENSION check
};

class CPed;
class CBike : public CAutomobile {
public:
    BikeExt *&Ext() { return *reinterpret_cast<BikeExt **>(&m_aCarNodes[0]); }
    const BikeHandling &BH() { return Ext()->def->bike; }

    void Construct(int modelId, unsigned char createdBy, const BikeModelDef *def);
    void Destruct();

    // virtuals (called through our vtable)
    void SetModelIndexBike(unsigned int id);
    void ProcessControlBike();
    void TeleportBike(CVector pos);
    void PreRenderBike();
    void RenderBike();
    int ProcessEntityCollisionBike(CEntity *ent, CColPoint *colpoints);
    void ProcessControlInputsBike(unsigned char pad);
    void GetComponentWorldPositionBike(int component, CVector &pos);
    void BlowUpCarBike(CEntity *culprit);
    bool SetUpWheelColModelBike(CColModel *colModel);
    void BurstTyreBike(unsigned char wheel);
    bool IsRoomForPedToLeaveCarBike(unsigned int component, CVector *doorOffset);
    float GetHeightAboveRoadBike() { return m_fHeightAboveRoad; }
    void RemoveRefsToVehicleBike(CEntity *ent);

    // helpers ported from VC
    void SetupModelNodes();
    void SetupSuspensionLines();
    void ResetSuspension();
    void CalculateLeanMatrix();
    void VehicleDamage();
    void ProcessBikeWheel(CVector &wheelFwd, CVector &wheelRight, CVector &wheelContactSpeed, CVector &wheelContactPoint,
                          int wheelsOnGround, float thrust, float brake, float adhesion, float destabTraction, int wheelId,
                          float *wheelSpeed, tWheelState *wheelState, int special, unsigned short wheelStatus);
    CVector GetSpeedAt(const CVector &r);
    float GetMassAt(const CVector &pos, const CVector &dir);
    void TurnForce(const CVector &j, const CVector &p) { ApplyTurnForce(j.x, j.y, j.z, p.x, p.y, p.z); }
    void MoveForce(const CVector &j) { ApplyMoveForce(j.x, j.y, j.z); }
    void SpringCollisionAlt(float springConst, CVector &springDir, CVector &point, float springRatio, float bias, CVector forceDir);
};

namespace bikes {
bool IsBike(const CVehicle *v);
const BikeModelDef *DefForModel(int modelId);
void InstallVtable();
void *Vtable();
void DismountInstantly(CPed *ped, CBike *bike);
void KnockOffRider(CBike *bike, int weapon, int direction, CPed *ped);   // weapon: eWeaponType
}

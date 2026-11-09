// Port of reVC's CBike (MIT-PoU licensed reverse-engineered VC source) onto GTA III v1.0.
// Logic follows reVC's Bike.cpp closely; differences are marked "III:".
#include "Bike.h"
#include "CTimer.h"
#include "CPad.h"
#include "CWorld.h"
#include "CModelInfo.h"
#include "CVehicleModelInfo.h"
#include "CColModel.h"
#include "CCollision.h"
#include "CSurfaceTable.h"
#include "CCarCtrl.h"
#include "CCarAI.h"
#include "CExplosion.h"
#include "CCamera.h"
#include "CGeneral.h"
#include "CPed.h"
#include "CPlayerPed.h"
#include "CClumpModelInfo.h"
#include "common.h"
#include "RpAnimBlend.h"
#include "NodeName.h"
#include "cHandlingDataMgr.h"
#include <cmath>

using namespace plugin;

static inline float Step() { return CTimer::ms_fTimeStep; }
static inline float Pow(float a, float b) { return powf(a, b); }
static inline CMatrix &Mat(CBike *b) { return b->m_matrix; }
static inline CColModel *ColModelOf(CBike *b) { return CModelInfo::ms_modelInfoPtrs[b->m_nModelIndex]->m_pColModel; }
static inline CVehicleModelInfo *MI(CBike *b) { return (CVehicleModelInfo *)CModelInfo::ms_modelInfoPtrs[b->m_nModelIndex]; }
static inline CVector Mul3x3(const CMatrix &m, const CVector &v) { return Multiply3x3(m, v); }

enum { BIKE_WHEELSPEC_0, BIKE_WHEELSPEC_1, BIKE_WHEELSPEC_2, BIKE_WHEELSPEC_3 };


// RW helper from reVC: frame matrix relative to `end` (exclusive)
static void GetRelativeMatrix(RwMatrix *mat, RwFrame *frm, RwFrame *end) {
    *mat = *RwFrameGetMatrix(frm);
    frm = RwFrameGetParent(frm);
    while (frm) {
        RwMatrixTransform(mat, RwFrameGetMatrix(frm), rwCOMBINEPOSTCONCAT);
        frm = RwFrameGetParent(frm);
        if (frm == end) frm = nullptr;
    }
}

CVector CBike::GetSpeedAt(const CVector &r) {
    return m_vecMoveSpeed + m_vecFrictionMoveForce + Cross(m_vecFrictionTurnForce + m_vecTurnSpeed, r);
}
float CBike::GetMassAt(const CVector &pos, const CVector &dir) {
    CVector c = Cross(pos, dir);
    return 1.0f / (Dot(c, c) / m_fTurnMass + 1.0f / m_fMass);
}
void CBike::SpringCollisionAlt(float springConst, CVector &springDir, CVector &point, float springRatio, float bias, CVector forceDir) {
    float compression = 1.0f - springRatio;
    if (compression > 0.0f) {
        if (Dot(springDir, forceDir) > 0.0f) forceDir = forceDir * -1.0f;
        float step = Step() < 3.0f ? Step() : 3.0f;
        float impulse = GRAVITY_III * m_fMass * step * springConst * compression * bias * 2.0f;
        if (bIsHeavy) impulse *= 0.75f;
        MoveForce(forceDir * impulse);
        TurnForce(forceDir * impulse, point);
    }
}

// ------------------------------------------------------------------------------------------
void CBike::Construct(int id, unsigned char createdBy, const BikeModelDef *def) {
    // III: CVehicle::CVehicle(createdBy), then take over the object as a bike.
    CallMethod<0x550A60, CVehicle *, unsigned char>(this, createdBy);
    memset(reinterpret_cast<char *>(this) + sizeof(CVehicle), 0, sizeof(CAutomobile) - sizeof(CVehicle));
    *reinterpret_cast<void **>(this) = bikes::Vtable();
    Ext() = new BikeExt();
    BikeExt &e = *Ext();
    e.def = def;
    m_nVehicleClass = VEHICLE_TYPE_BIKE;

    SetModelIndexBike(id);
    CVehicleModelInfo *mi = MI(this);

    m_pHandlingData = const_cast<tHandlingData *>(&def->handling);
    mi->ChooseVehicleColour(&m_nPrimaryColor, &m_nSecondaryColor);

    e.frontForkSlope = tanf(DegToRad(mi->m_fBikeSteerAngle));
    m_fMass = m_pHandlingData->m_fMass;
    m_fTurnMass = m_pHandlingData->m_fTurnMass;
    m_vecCentreOfMass = m_pHandlingData->m_vCentreOfMass;
    m_vecCentreOfMass.z = 0.1f;
    m_fAirResistance = m_pHandlingData->m_vDimensions.x * m_pHandlingData->m_vDimensions.z / m_fMass;
    m_fElasticity = 0.05f;
    m_fBuoyancy = m_pHandlingData->m_fBuoyancyConstant;

    m_fSteerAngle = m_fGasPedal = m_fBrakePedal = 0.0f;
    m_fGasPedalAudio = 0.0f;
    for (int i = 0; i < 4; i++) {
        m_aWheelState[i] = WHEEL_STATE_NORMAL;
        m_aGroundPhysical[i] = nullptr;
        m_avecGroundOffset[i] = CVector(0.0f, 0.0f, 0.0f);
        m_afSuspensionSpringRatioPrev[i] = m_afSuspensionSpringRatio[i] = 1.0f;
        m_afWheelTimer[i] = 0.0f;
    }
    m_nWheelsOnGround = m_nDriveWheelsOnGround = m_nDriveWheelsOnGroundPrev = 0;
    m_fHeightAboveRoad = 0.0f;
    m_fTraction = 1.0f;

    CColModel *colModel = mi->m_pColModel;
    if (!colModel->m_pLines) {
        colModel->m_pLines = (CColLine *)RwMalloc(4 * sizeof(CColLine));
        colModel->m_nNumOfLines = 4;
    }
    SetupSuspensionLines();

    m_autoPilot.m_nCarMission = MISSION_NONE;
    m_autoPilot.m_nTimeToStartMission = CTimer::m_snTimeInMilliseconds;
    m_nStatus = STATUS_SIMPLE;
    bUseCollisionRecords = true;
    m_nNumPassengers = 0;
    bIsVan = bIsBus = bIsBig = bLowVehicle = false;
    bPedPhysics = false;
    e.leanMatrixClean = false;
    e.leanMatrix = m_matrix;
    e.isStanding = true;   // VC's CREATE_CAR does this for bikes: start on the kickstand
}

void CBike::Destruct() {
    for (int i = 0; i < 4; i++)
        if (m_aGroundPhysical[i]) CleanUpRef(m_aGroundPhysical[i], reinterpret_cast<CEntity **>(&m_aGroundPhysical[i]));
    if (m_pSetOnFireEntity) CleanUpRef(m_pSetOnFireEntity, &m_pSetOnFireEntity);
    delete Ext();
    Ext() = nullptr;
}

void CBike::SetModelIndexBike(unsigned int id) {
    // III: call CVehicle::SetModelIndex by address (plugin-sdk's wrapper is a virtual call -> recursion)
    CallMethod<0x551170, CVehicle *, unsigned int>(this, id);
    SetupModelNodes();
}

void CBike::SetupModelNodes() {
    BikeExt &e = *Ext();
    for (int i = 0; i < BIKE_NUM_NODES; i++) e.nodes[i] = nullptr;
    CClumpModelInfo::FillFrameArray(m_pRwClump, e.nodes);
    // III's bike frame table has no "handlebars" entry; find it by name (it's a child of the front fork)
    if (!e.nodes[BIKE_HANDLEBARS] && m_pRwClump) {
        struct Find { static RwFrame *CB(RwFrame *f, void *d) {
            auto **out = (RwFrame **)d;
            const char *n = GetFrameNodeName(f);
            if (n && _stricmp(n, "handlebars") == 0) { *out = f; return nullptr; }
            RwFrameForAllChildren(f, CB, d);
            return *out ? nullptr : f;
        } };
        RwFrameForAllChildren(RpClumpGetFrame(m_pRwClump), Find::CB, &e.nodes[BIKE_HANDLEBARS]);
    }
}

void CBike::ResetSuspension() {
    for (int i = 0; i < 2; i++) { m_afWheelRotation[i] = 0.0f; m_aWheelState[i] = WHEEL_STATE_NORMAL; }
    for (int i = 0; i < 4; i++) { m_afSuspensionSpringRatio[i] = 1.0f; m_afWheelTimer[i] = 0.0f; }
}

void CBike::SetupSuspensionLines() {
    BikeExt &e = *Ext();
    CVector posn;
    float suspOffset = 0.0f;
    RwFrame *node = nullptr;
    CVehicleModelInfo *mi = MI(this);
    CColModel *colModel = mi->m_pColModel;
    tHandlingData *h = m_pHandlingData;
    RwMatrix *mat = RwMatrixCreate();
    bool initialized = e.suspensionReady;
    for (int i = 0; i < 4; i++) {
        if (initialized) {
            posn = colModel->m_pLines[i].m_vecStart;
            posn.z = i < 2 ? e.wheelBasePosition[0] : e.wheelBasePosition[1];
        } else {
            switch (i) {
            case BIKESUSP_F1: node = e.nodes[BIKE_WHEEL_FRONT]; suspOffset = 0.25f * mi->m_fWheelScale; break;
            case BIKESUSP_F2: node = e.nodes[BIKE_WHEEL_FRONT]; suspOffset = -0.25f * mi->m_fWheelScale; break;
            case BIKESUSP_R1: node = e.nodes[BIKE_WHEEL_REAR]; suspOffset = 0.25f * mi->m_fWheelScale; break;
            case BIKESUSP_R2: node = e.nodes[BIKE_WHEEL_REAR]; suspOffset = -0.25f * mi->m_fWheelScale; break;
            }
            GetRelativeMatrix(mat, node, node);
            posn = CVector(mat->pos.x, mat->pos.y, mat->pos.z);
            if (i == BIKESUSP_F1)
                e.wheelBasePosition[BIKEWHEEL_FRONT] = posn.z;
            else if (i == BIKESUSP_R1) {
                e.wheelBasePosition[BIKEWHEEL_REAR] = posn.z;
                GetRelativeMatrix(mat, e.nodes[BIKE_FORKS_REAR], e.nodes[BIKE_FORKS_REAR]);
                float dz = posn.z - mat->pos.z, dy = posn.y - mat->pos.y;
                e.rearForkLength = sqrtf(dy * dy + dz * dz);
                if (e.rearForkLength == 0.0f) e.rearForkLength = 0.5f;
            }
            posn.y += suspOffset;
        }
        posn.z += h->m_fSuspensionUpperLimit;
        colModel->m_pLines[i].m_vecStart = posn;
        posn.z += h->m_fSuspensionLowerLimit - h->m_fSuspensionUpperLimit;
        posn.z -= mi->m_fWheelScale * 0.5f;
        colModel->m_pLines[i].m_vecEnd = posn;
        m_afSuspensionSpringLength[i] = h->m_fSuspensionUpperLimit - h->m_fSuspensionLowerLimit;
        m_afSuspensionLineLength[i] = colModel->m_pLines[i].m_vecStart.z - colModel->m_pLines[i].m_vecEnd.z;
    }
    if (!initialized) {
        GetRelativeMatrix(mat, e.nodes[BIKE_FORKS_FRONT], e.nodes[BIKE_FORKS_FRONT]);
        e.frontForkY = mat->pos.y;
        e.frontForkZ = mat->pos.z;
        e.suspensionReady = true;
    }
    m_fHeightAboveRoad = m_afSuspensionSpringLength[0] * (1.0f - 1.0f / (4.0f * h->m_fSuspensionForceLevel))
        - colModel->m_pLines[0].m_vecStart.z + mi->m_fWheelScale * 0.5f;
    for (int i = 0; i < 2; i++) m_afWheelPosition[i] = mi->m_fWheelScale * 0.5f - m_fHeightAboveRoad;
    if (colModel->m_boundBox.m_vecMin.z > colModel->m_pLines[0].m_vecEnd.z)
        colModel->m_boundBox.m_vecMin.z = colModel->m_pLines[0].m_vecEnd.z;
    float radius = (std::max)(Len(colModel->m_boundBox.m_vecMin), Len(colModel->m_boundBox.m_vecMax));
    if (colModel->m_boundSphere.m_fRadius < radius) colModel->m_boundSphere.m_fRadius = radius;
    RwMatrixDestroy(mat);
}

void CBike::CalculateLeanMatrix() {
    BikeExt &e = *Ext();
    if (e.leanMatrixClean) return;
    CMatrix mat;
    mat.SetRotateX(-0.05f * fabsf(e.leanLR));
    mat.RotateY(e.leanLR);
    e.leanMatrix = m_matrix;
    e.leanMatrix = e.leanMatrix * mat;
    e.leanMatrix.pos = e.leanMatrix.pos + m_matrix.at * ((1.0f - cosf(e.leanLR)) * ColModelOf(this)->m_boundBox.m_vecMin.z);
    e.leanMatrixClean = true;
}

// ------------------------------------------------------------------------------------------
static CVector vecTestResistance(0.9995f, 0.9f, 0.95f);
static float fDAxisX = 1.0f, fDAxisXExtra = 100.0f, fDAxisY = 1000.0f, fInAirXRes = 0.98f;
static float fTweakBikeWheelTurnForce = 2.0f, fBurstBikeSpeedMax = 0.12f, fBurstBikeTyreMod = 0.05f;
#define SAND_SLOWDOWN (0.02f)

static bool IsSand(unsigned char s) { return s == SURFACE_SAND || s == SURFACE_SAND_BEACH; }

#pragma optimize("", off)   // reVC: original had optimisation off for this function too
void CBike::ProcessControlBike() {
    BikeExt &e = *Ext();
    const BikeHandling &bh = BH();
    tHandlingData *h = m_pHandlingData;
    int i;
    float wheelRot, acceleration = 0.0f, brake = 0.0f;
    bool bBalancedByRider = false, bStuckInSand = false, restingOnPhysical = false;
    CColModel *colModel = ColModelOf(this);
    float wheelScale = MI(this)->m_fWheelScale;
    bWarnedPeds = false;
    e.leanMatrixClean = false;
    e.doingBurnout = 0;
    e.extraSpeed = false;

    ProcessCarAlarm();

    // III: bikes have no AI rider yet (M3), so a "simple" bike is just a parked one
    if (m_nStatus == STATUS_SIMPLE && !m_pDriver) m_nStatus = STATUS_ABANDONED;

    switch (m_nStatus) {
    case STATUS_PLAYER: {
        bBalancedByRider = true;
        e.beingPickedUp = false;
        CPlayerPed *player = FindPlayerPed();
        if (player && player->m_ePedState != PEDSTATE_EXIT_CAR && player->m_ePedState != PEDSTATE_DRAG_FROM_CAR) {
            ProcessControlInputsBike(0);
            float speedMag = (std::min)(Len(m_vecMoveSpeed), 0.1f);
            if (e.leanInput < 0.0f) {
                m_vecCentreOfMass.y = h->m_vCentreOfMass.y + bh.fLeanBakCOM * e.leanInput;
                // FIX_BUGS from reVC: centre of mass in world orientation
                CVector com = m_vecCentreOfMass; com.y = -com.y; com = Mul3x3(m_matrix, com);
                if ((m_fBrakePedal == 0.0f && !bIsHandbrakeOn) || m_nWheelsOnGround == 0) {
                    float force = e.leanInput * m_fTurnMass * bh.fLeanBackForce * speedMag;
                    force *= 0.5f * m_fGasPedal + 0.5f;
                    TurnForce(m_matrix.at * (-force * Step()), com + m_matrix.up);
                }
            } else {
                m_vecCentreOfMass.y = h->m_vCentreOfMass.y + bh.fLeanFwdCOM * e.leanInput;
                CVector com = m_vecCentreOfMass; com.y = -com.y; com = Mul3x3(m_matrix, com);
                if (m_fBrakePedal < 0.0f || m_nWheelsOnGround == 0) {
                    float force = e.leanInput * m_fTurnMass * bh.fLeanFwdForce * speedMag;
                    TurnForce(m_matrix.at * (-force * Step()), com + m_matrix.up);
                }
            }
            bool sand = false;
            for (i = 0; i < 4; i++)
                if (m_afSuspensionSpringRatio[i] < 1.0f && IsSand(m_aWheelColPoints[i].m_nSurfaceTypeB)) sand = true;
            if (sand) {
                CVector parallelSpeed = m_vecMoveSpeed - m_matrix.at * Dot(m_vecMoveSpeed, m_matrix.at);
                if (m_fGasPedal > 0.3f) {
                    if (Dot(parallelSpeed, parallelSpeed) < 0.3f * 0.3f) bStuckInSand = true;
                    parallelSpeed = parallelSpeed - m_matrix.up * Dot(parallelSpeed, m_matrix.up);
                }
                MoveForce(parallelSpeed * (-Step() * SAND_SLOWDOWN * m_fMass));
            }
        }
        break;
    }
    case STATUS_SIMPLE:
        CCarAI::UpdateCarAI(this);
        CallMethod<0x495F10, CPhysical *>(this);   // CPhysical::ProcessControl (non-virtual)
        CCarCtrl::UpdateCarOnRails(this);
        m_nWheelsOnGround = 2;
        m_nDriveWheelsOnGroundPrev = m_nDriveWheelsOnGround;
        m_nDriveWheelsOnGround = 2;
        h->m_transmissionData.CalculateGearForSimpleCar(m_autoPilot.m_fMaxTrafficSpeed / 50.0f, m_nCurrentGear);
        wheelRot = ProcessWheelRotation(WHEEL_STATE_NORMAL, m_matrix.up, m_vecMoveSpeed, 0.5f * wheelScale);
        for (i = 0; i < 2; i++) m_afWheelRotation[i] += wheelRot;
        bVehicleColProcessed = false;
        e.wheelieCam = false;
        return;
    case STATUS_PHYSICS:
        CCarAI::UpdateCarAI(this);
        CCarCtrl::SteerAICarWithPhysics(this);
        bBalancedByRider = true;
        e.wheelieCam = false;
        if (bIsBeingCarJacked) { m_fGasPedal = 0.0f; m_fBrakePedal = 1.0f; bIsHandbrakeOn = true; }
        else e.beingPickedUp = false;
        break;
    case STATUS_ABANDONED:
        m_fBrakePedal = 0.0f;
        bIsHandbrakeOn = Dot(m_vecMoveSpeed, m_vecMoveSpeed) < 0.1f * 0.1f || e.isStanding;
        m_fGasPedal = 0.0f;
        m_nCarHornTimer = 0;
        bBalancedByRider = (m_pDriver || m_apPassengers[0] || bIsBeingCarJacked) && !e.isStanding;
        e.pedLeanLR = e.pedLeanUD = 0.0f;
        e.wheelieCam = false;
        if (bIsBeingCarJacked) { m_fGasPedal = 0.0f; m_fBrakePedal = 1.0f; bIsHandbrakeOn = true; }
        break;
    case STATUS_WRECKED:
        m_fBrakePedal = 0.05f;
        bIsHandbrakeOn = true;
        m_fSteerAngle = 0.0f;
        m_fGasPedal = 0.0f;
        m_nCarHornTimer = 0;
        bBalancedByRider = false;
        e.wheelieCam = false;
        e.pedLeanLR = e.pedLeanUD = 0.0f;
        break;
    case STATUS_PLAYER_DISABLED:
        if (Dot(m_vecMoveSpeed, m_vecMoveSpeed) < 0.1f * 0.1f) { m_fBrakePedal = 1.0f; bIsHandbrakeOn = true; }
        else { m_fBrakePedal = 0.0f; bIsHandbrakeOn = false; }
        m_fSteerAngle = 0.0f;
        m_fGasPedal = 0.0f;
        m_nCarHornTimer = 0;
        bBalancedByRider = true;
        e.wheelieCam = false;
        break;
    }

    if (e.isStanding && (fabsf(m_matrix.right.z) > 0.35f || fabsf(m_matrix.up.z) > 0.5f)) e.isStanding = false;

    if (bBalancedByRider || e.beingPickedUp || e.isStanding) {
        float fDx = fDAxisX;
        CVector res = vecTestResistance;
        CVector localTurnSpeed = Multiply3x3(m_vecTurnSpeed, m_matrix);
        if (m_nStatus == STATUS_PLAYER) {
            if (m_afWheelTimer[BIKESUSP_F1] == 0.0f && m_afWheelTimer[BIKESUSP_F2] == 0.0f) {
                fDx = fDAxisXExtra;
                if (!(m_afWheelTimer[BIKESUSP_R1] == 0.0f && m_afWheelTimer[BIKESUSP_R2] == 0.0f) && m_matrix.up.z > 0.0f)
                    res.x -= (std::min)(0.25f * fabsf(bh.fWheelieAng - m_matrix.up.z), 0.07f);
                else
                    res.x = fInAirXRes;
            } else if (m_afWheelTimer[BIKESUSP_R1] == 0.0f && m_afWheelTimer[BIKESUSP_R2] == 0.0f) {
                fDx = fDAxisXExtra;
                if (m_matrix.up.z < 0.0f)
                    res.x *= (std::min)(0.3f * fabsf(bh.fStoppieAng - m_matrix.up.z), 0.1f) + 0.9f;
            }
        }
        res.x *= 1.0f / (fDx * localTurnSpeed.x * localTurnSpeed.x + 1.0f);
        res.y *= 1.0f / (fDAxisY * localTurnSpeed.y * localTurnSpeed.y + 1.0f);
        res.x = Pow(res.x, Step());
        res.y = Pow(res.y, Step());
        float turnX = localTurnSpeed.x * (res.x - 1.0f);
        float turnY = localTurnSpeed.y * (res.y - 1.0f);
        TurnForce(m_matrix.at * (-turnY * m_fTurnMass), m_matrix.right + Mul3x3(m_matrix, m_vecCentreOfMass));
        TurnForce(m_matrix.at * (turnX * m_fTurnMass), m_matrix.up + Mul3x3(m_matrix, m_vecCentreOfMass));
        if (m_nStatus != STATUS_PLAYER) m_vecCentreOfMass = h->m_vCentreOfMass;
    } else {
        m_vecCentreOfMass = h->m_vCentreOfMass;
        m_vecCentreOfMass.z = bh.fNoPlayerCOMz;
    }

    // Skip physics if object is found to have been static recently
    bool skipPhysics = false;
    if (!bIsStuck && (m_nStatus == STATUS_ABANDONED || m_nStatus == STATUS_WRECKED) && !e.beingPickedUp) {
        bool makeStatic = false;
        float moveSpeedLimit, turnSpeedLimit, distanceLimit;
        if (!bVehicleColProcessed && m_vecMoveSpeed.x == 0.0f && m_vecMoveSpeed.y == 0.0f && m_vecMoveSpeed.z == 0.0f &&
            m_afSuspensionSpringRatioPrev[3] != 1.0f)
            makeStatic = true;
        if (m_nStatus == STATUS_WRECKED) { moveSpeedLimit = 0.006f; turnSpeedLimit = 0.0015f; distanceLimit = 0.015f; }
        else { moveSpeedLimit = 0.003f; turnSpeedLimit = 0.0009f; distanceLimit = 0.005f; }
        m_vecMoveSpeedAvg = (m_vecMoveSpeedAvg + m_vecMoveSpeed) / 2.0f;
        m_vecTurnSpeedAvg = (m_vecTurnSpeedAvg + m_vecTurnSpeed) / 2.0f;
        float ml = moveSpeedLimit * Step(), tl = turnSpeedLimit * Step();
        if ((Dot(m_vecMoveSpeedAvg, m_vecMoveSpeedAvg) <= ml * ml && Dot(m_vecTurnSpeedAvg, m_vecTurnSpeedAvg) <= tl * tl &&
             m_fDistanceTravelled < distanceLimit) || makeStatic) {
            m_nStaticFrames++;
            if (m_nStaticFrames > 10 || makeStatic)
                if (!CCarCtrl::MapCouldMoveInThisArea(m_matrix.pos.x, m_matrix.pos.y)) {
                    if (!makeStatic || m_nStaticFrames > 10) m_nStaticFrames = 10;
                    skipPhysics = true;
                    m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
                    m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
                }
        } else
            m_nStaticFrames = 0;
    }

    // Postpone
    for (i = 0; i < 4; i++)
        if (m_aGroundPhysical[i]) {
            restingOnPhysical = true;
            if (!CWorld::bForceProcessControl && m_aGroundPhysical[i]->bIsInSafePosition) { bWasPostponed = true; return; }
        }
    if (restingOnPhysical) { skipPhysics = false; m_nStaticFrames = 0; }

    VehicleDamage();

    float fwdSpeed = 0.0f;
    if (skipPhysics) {
        bHasContacted = false;
        bIsInSafePosition = false;
        bWasPostponed = false;
        bHasHitWall = false;
        m_nNumCollisionRecords = 0;
        bHasCollided = false;
        bVehicleColProcessed = false;
        m_nDamagePieceType = 0;
        m_fDamageImpulse = 0.0f;
        m_pDamageEntity = nullptr;
        m_vecFrictionTurnForce = CVector(0.0f, 0.0f, 0.0f);
        m_vecFrictionMoveForce = CVector(0.0f, 0.0f, 0.0f);
        if (e.isStanding && e.wheelAngle < DegToRad(20.0f)) e.wheelAngle += DegToRad(1.0f) * Step();
        if (e.isStanding) {
            float f = Pow(0.97f, Step());
            e.leanLR2 = e.leanLR2 * f - (asinf(Clampf(m_matrix.right.z, -1.0f, 1.0f)) + DegToRad(15.0f)) * (1.0f - f);
            e.leanLR = e.leanLR2;
        }
    } else {
        // This has to be done if ProcessEntityCollision wasn't called
        if (!bVehicleColProcessed) {
            CMatrix mat(m_matrix);
            bIsStuck = false;
            bHasContacted = false;
            bIsInSafePosition = false;
            bWasPostponed = false;
            bHasHitWall = false;
            m_fDistanceTravelled = 0.0f;
            m_bIsVehicleBeingShifted = false;
            bSkipLineCol = false;
            ApplyMoveSpeed();
            ApplyTurnSpeed();
            for (i = 0; CheckCollision() && i < 5; i++) {
                m_matrix = mat;
                ApplyMoveSpeed();
                ApplyTurnSpeed();
            }
            bIsInSafePosition = true;
            bIsStuck = false;
        }

        if (!(bBalancedByRider || e.beingPickedUp || e.isStanding)) {
            if (m_matrix.right.z < 0.0f) { if (m_fSteerAngle > -DegToRad(25.0f)) m_fSteerAngle -= DegToRad(0.5f) * Step(); }
            else { if (m_fSteerAngle < DegToRad(25.0f)) m_fSteerAngle += DegToRad(0.5f) * Step(); }
        }

        // Lean forward speed up
        float savedAirResistance = m_fAirResistance;
        if (m_nStatus == STATUS_PLAYER && m_pDriver && m_pDriver->m_pRwClump) {
            CAnimBlendAssociation *assoc = RpAnimBlendClumpGetAssociation(m_pDriver->m_pRwClump, 200 + 4 /* BIKE_FWD */);
            if (assoc && assoc->m_fBlendAmount > 0.5f && assoc->m_fCurrentTime > 0.06f && assoc->m_fCurrentTime < 0.14f) {
                m_fAirResistance *= 0.6f;
                if (m_fGasPedal > 0.5f && Dot(m_vecMoveSpeed, m_matrix.up) > 0.25f) {
                    MoveForce(m_matrix.up * (0.2f * m_fMass * GRAVITY_III * Step()));
                    e.extraSpeed = true;
                }
            }
        }

        CallMethod<0x495F10, CPhysical *>(this);   // CPhysical::ProcessControl (non-virtual)
        m_fAirResistance = savedAirResistance;

        // Rescale spring ratios, i.e. subtract wheel radius
        for (i = 0; i < 4; i++) {
            float wheelRadius = 1.0f - m_afSuspensionSpringLength[i] / m_afSuspensionLineLength[i];
            m_afSuspensionSpringRatio[i] = (m_afSuspensionSpringRatio[i] - wheelRadius) / (1.0f - wheelRadius);
        }

        int rnd = 0;
        fwdSpeed = fabsf(Dot(m_vecMoveSpeed, m_matrix.up));
        CVector contactPoints[4], contactSpeeds[4], springDirections[4];
        for (i = 0; i < 4; i++) {
            if (e.wheelStatus[i / 2] == WHEEL_STATUS_MISSING)
                m_afSuspensionSpringRatio[i] = 1.0f;
            else if (e.wheelStatus[i / 2] == WHEEL_STATUS_BURST) {
                if (i == BIKESUSP_F1 || i == BIKESUSP_R1)
                    rnd = CGeneral::GetRandomNumberInRange(0, (int)(40 * fwdSpeed) + 98) < 100;
                if (rnd) {
                    m_afSuspensionSpringRatio[i] += 0.3f * (m_afSuspensionLineLength[i] - m_afSuspensionSpringLength[i]) / m_afSuspensionSpringLength[i];
                    if (m_afSuspensionSpringRatio[i] > 1.0f) m_afSuspensionSpringRatio[i] = 1.0f;
                }
            }
            if (m_afSuspensionSpringRatio[i] < 1.0f) {
                contactPoints[i] = m_aWheelColPoints[i].m_vecPoint - m_matrix.pos;
                springDirections[i] = Norm(Mul3x3(m_matrix, colModel->m_pLines[i].m_vecEnd - colModel->m_pLines[i].m_vecStart));
            }
        }

        // Make springs push up vehicle
        for (i = 0; i < 4; i++) {
            if (m_afSuspensionSpringRatio[i] < 1.0f) {
                float bias = h->m_fSuspensionBiasBetweenFrontAndRear;
                if (i == BIKESUSP_R1 || i == BIKESUSP_R2) bias = 1.0f - bias;
                if (m_aWheelColPoints[i].m_vecNormal.z > 0.35f)
                    SpringCollisionAlt(h->m_fSuspensionForceLevel, springDirections[i], contactPoints[i],
                                       m_afSuspensionSpringRatio[i], bias, m_aWheelColPoints[i].m_vecNormal);
                else
                    ApplySpringCollision(h->m_fSuspensionForceLevel, springDirections[i], contactPoints[i],
                                         m_afSuspensionSpringRatio[i], bias);
                m_abWheelSkidmarkMuddy[i < 2 ? 0 : 1] =
                    m_aWheelColPoints[i].m_nSurfaceTypeB == SURFACE_GRASS || m_aWheelColPoints[i].m_nSurfaceTypeB == SURFACE_MUD_DRY ||
                    IsSand(m_aWheelColPoints[i].m_nSurfaceTypeB);
            } else {
                contactPoints[i] = Mul3x3(m_matrix, colModel->m_pLines[i].m_vecEnd);
            }
        }

        // Get speed at contact points
        for (i = 0; i < 4; i++) {
            contactSpeeds[i] = GetSpeedAt(contactPoints[i]);
            if (m_aGroundPhysical[i]) {
                CPhysical *gp = m_aGroundPhysical[i];
                CVector off = m_avecGroundOffset[i];
                contactSpeeds[i] = contactSpeeds[i] - (gp->m_vecMoveSpeed + Cross(gp->m_vecTurnSpeed, off));
            }
        }

        CVector normal;
        if (m_afSuspensionSpringRatio[0] < 1.0f || m_afSuspensionSpringRatio[1] < 1.0f) {
            normal = m_afSuspensionSpringRatio[0] < 1.0f ? m_aWheelColPoints[0].m_vecNormal : m_aWheelColPoints[1].m_vecNormal;
            if (normal.z > 0.35f) springDirections[0] = normal * -1.0f;
            normal = m_afSuspensionSpringRatio[1] < 1.0f ? m_aWheelColPoints[1].m_vecNormal : m_aWheelColPoints[0].m_vecNormal;
            if (normal.z > 0.35f) springDirections[1] = normal * -1.0f;
        }
        if (m_afSuspensionSpringRatio[2] < 1.0f || m_afSuspensionSpringRatio[3] < 1.0f) {
            normal = m_afSuspensionSpringRatio[2] < 1.0f ? m_aWheelColPoints[2].m_vecNormal : m_aWheelColPoints[3].m_vecNormal;
            if (normal.z > 0.35f) springDirections[2] = normal * -1.0f;
            normal = m_afSuspensionSpringRatio[3] < 1.0f ? m_aWheelColPoints[3].m_vecNormal : m_aWheelColPoints[2].m_vecNormal;
            if (normal.z > 0.35f) springDirections[3] = normal * -1.0f;
        }

        // dampen springs
        for (i = 0; i < 4; i++)
            if (m_afSuspensionSpringRatio[i] < 1.0f)
                ApplySpringDampening(h->m_fSuspensionDampingLevel, springDirections[i], contactPoints[i], contactSpeeds[i]);

        // Get speed at contact points again
        for (i = 0; i < 4; i++) {
            contactSpeeds[i] = GetSpeedAt(contactPoints[i]);
            if (m_aGroundPhysical[i]) {
                CPhysical *gp = m_aGroundPhysical[i];
                CVector off = m_avecGroundOffset[i];
                contactSpeeds[i] = contactSpeeds[i] - (gp->m_vecMoveSpeed + Cross(gp->m_vecTurnSpeed, off));
                CleanUpRef(gp, reinterpret_cast<CEntity **>(&m_aGroundPhysical[i]));
                m_aGroundPhysical[i] = nullptr;
            }
        }

        fwdSpeed = Dot(m_vecMoveSpeed, m_matrix.up);
        float speedForGear = m_fChangeGearTime;
        acceleration = h->m_transmissionData.CalculateDriveAcceleration(m_fGasPedal, m_nCurrentGear, speedForGear, fwdSpeed, false);
        m_fChangeGearTime = speedForGear;
        acceleration /= m_fForceMultiplier;

        brake = m_fBrakePedal * h->m_fBrakeDeceleration * Step();
        bool neutralHandling = m_nStatus != STATUS_PLAYER && (h->m_nModelFlags & 0x1000);
        float brakeBiasFront = neutralHandling ? 1.0f : 2.0f * h->m_fBrakeBias;
        float brakeBiasRear = neutralHandling ? 1.0f : 2.0f * (1.0f - h->m_fBrakeBias);
        float tractionBiasFront = neutralHandling ? 1.0f : 2.0f * h->m_fTractionBias;
        float tractionBiasRear = neutralHandling ? 1.0f : 2.0f - tractionBiasFront;

        // Count how many wheels are touching the ground
        m_nWheelsOnGround = 0;
        m_nDriveWheelsOnGroundPrev = m_nDriveWheelsOnGround;
        m_nDriveWheelsOnGround = 0;
        for (i = 0; i < 4; i++) {
            if (m_afSuspensionSpringRatio[i] < 1.0f) m_afWheelTimer[i] = 4.0f;
            else m_afWheelTimer[i] = (std::max)(m_afWheelTimer[i] - Step(), 0.0f);
            if (m_afWheelTimer[i] > 0.0f) {
                m_nWheelsOnGround++;
                if (i == BIKESUSP_R1 || i == BIKESUSP_R2) m_nDriveWheelsOnGround = 1;
                if (m_nWheelsOnGround == 1) e.avgSurfaceNormal = m_aWheelColPoints[i].m_vecNormal;
                else e.avgSurfaceNormal = e.avgSurfaceNormal + m_aWheelColPoints[i].m_vecNormal;
            }
        }
        if (m_nWheelsOnGround == 0) e.avgSurfaceNormal = CVector(0.0f, 0.0f, 1.0f);
        else {
            e.avgSurfaceNormal = e.avgSurfaceNormal / (float)m_nWheelsOnGround;
            if (Dot(e.avgSurfaceNormal, m_matrix.at) < -0.5f) e.avgSurfaceNormal = e.avgSurfaceNormal * -1.0f;
        }

        // Find contact points for wheel processing
        int frontLine = m_afSuspensionSpringRatio[BIKESUSP_F1] < m_afSuspensionSpringRatio[BIKESUSP_F2] ? BIKESUSP_F1 : BIKESUSP_F2;
        CVector frontContact(0.0f, colModel->m_pLines[BIKESUSP_F1].m_vecStart.y,
            colModel->m_pLines[BIKESUSP_F1].m_vecStart.z - m_afSuspensionSpringRatio[frontLine] * m_afSuspensionSpringLength[BIKESUSP_F1] - 0.5f * wheelScale);
        frontContact = Mul3x3(m_matrix, frontContact);
        int rearLine = m_afSuspensionSpringRatio[BIKESUSP_R1] < m_afSuspensionSpringRatio[BIKESUSP_R2] ? BIKESUSP_R1 : BIKESUSP_R2;
        CVector rearContact(0.0f, colModel->m_pLines[BIKESUSP_R1].m_vecStart.y,
            colModel->m_pLines[BIKESUSP_R1].m_vecStart.z - m_afSuspensionSpringRatio[rearLine] * m_afSuspensionSpringLength[BIKESUSP_R1] - 0.5f * wheelScale);
        rearContact = Mul3x3(m_matrix, rearContact);

        float traction = 0.004f * m_fTraction;
        traction *= h->m_fTractionMultiplier / 4.0f;

        // Turn wheel
        if (m_nStatus == STATUS_PLAYER || !e.isStanding || e.beingPickedUp) {
            if (fabsf(m_vecMoveSpeed.x) < 0.01f && fabsf(m_vecMoveSpeed.y) < 0.01f && m_fSteerAngle == 0.0f) {
                e.wheelAngle *= Pow(0.96f, Step());
            } else {
                float f;
                if (fwdSpeed > 0.01f && m_afWheelTimer[BIKESUSP_F1] > 0.0f && m_afWheelTimer[BIKESUSP_F2] > 0.0f && m_nStatus == STATUS_PLAYER) {
                    CColPoint point;
                    point.m_nSurfaceTypeA = SURFACE_WHEELBASE;
                    point.m_nSurfaceTypeB = SURFACE_TARMAC;
                    float steer = CSurfaceTable::GetAdhesiveLimit(point) * 4.0f * bh.fSpeedSteer * traction;
                    int grp = CSurfaceTable::GetAdhesionGroup(m_aWheelColPoints[rearLine].m_nSurfaceTypeB);
                    if (grp == ADHESIVE_LOOSE) steer *= bh.fSlipSteer;   // III: sand is "loose" too
                    f = asinf((std::min)(steer / (fwdSpeed * fwdSpeed), 1.0f)) / DegToRad(h->m_fSteeringLock);
                    if ((m_fSteerAngle < 0.0f && e.leanLR < 0.0f) || (m_fSteerAngle > 0.0f && e.leanLR > 0.0f)) f *= 2.0f;
                    f = (std::min)(f, 1.0f);
                } else
                    f = 1.0f;
                if (m_nStatus != STATUS_PLAYER) f = 1.0f;
                e.wheelAngle = m_fSteerAngle * f;
            }
        } else if (e.wheelAngle < DegToRad(20.0f))
            e.wheelAngle += DegToRad(1.5f) * Step();

        static float fThrust;
        static tWheelState WheelState[2];
        CVector initialMoveSpeed = m_vecMoveSpeed;
        bool rearWheelsFirst = false;   // VC flag REARWHEEL_1ST is not set for the PCJ

        auto processFront = [&]() {
            if (m_afWheelTimer[BIKESUSP_F1] > 0.0f || m_afWheelTimer[BIKESUSP_F2] > 0.0f) {
                int spec = (m_afWheelTimer[BIKESUSP_R1] > 0.0f || m_afWheelTimer[BIKESUSP_R2] > 0.0f) ? BIKE_WHEELSPEC_0 : BIKE_WHEELSPEC_2;
                CVector wheelFwd = Mul3x3(m_matrix, CVector(-sinf(e.wheelAngle), cosf(e.wheelAngle), 0.0f));
                CVector n = m_aWheelColPoints[frontLine].m_vecNormal;
                wheelFwd = Norm(wheelFwd - n * Dot(wheelFwd, n));
                CVector wheelRight = Norm(Cross(wheelFwd, n));
                fThrust = 0.0f;
                m_aWheelColPoints[frontLine].m_nSurfaceTypeA = SURFACE_WHEELBASE;
                float adhesion = CSurfaceTable::GetAdhesiveLimit(m_aWheelColPoints[frontLine]) * traction;
                float adhesionDestab = 1.0f;
                if (e.brakeDestabilization > 0.0f)
                    switch (CSurfaceTable::GetAdhesionGroup(m_aWheelColPoints[frontLine].m_nSurfaceTypeB)) {
                    case ADHESIVE_HARD: case ADHESIVE_LOOSE: adhesionDestab = 0.9f; break;
                    case ADHESIVE_ROAD: adhesionDestab = 0.7f; break;
                    }
                if (m_nStatus == STATUS_PLAYER) adhesion *= CSurfaceTable::GetWetMultiplier(m_aWheelColPoints[frontLine].m_nSurfaceTypeB);
                if (e.wheelStatus[BIKEWHEEL_FRONT] == WHEEL_STATUS_BURST) adhesion *= 0.4f;
                WheelState[BIKEWHEEL_FRONT] = m_aWheelState[BIKEWHEEL_FRONT];
                CVector contactSpeed = GetSpeedAt(frontContact);
                ProcessBikeWheel(wheelFwd, wheelRight, contactSpeed, frontContact, 2, fThrust, brake * brakeBiasFront,
                                 adhesion * tractionBiasFront, adhesionDestab, BIKEWHEEL_FRONT, &m_afWheelSpeed[BIKEWHEEL_FRONT],
                                 &WheelState[BIKEWHEEL_FRONT], spec, e.wheelStatus[BIKEWHEEL_FRONT]);
                if (bStuckInSand && (WheelState[BIKEWHEEL_FRONT] == WHEEL_STATE_SPINNING || WheelState[BIKEWHEEL_FRONT] == WHEEL_STATE_SKIDDING))
                    WheelState[BIKEWHEEL_FRONT] = WHEEL_STATE_NORMAL;
            } else {
                m_afWheelSpeed[BIKEWHEEL_FRONT] *= 0.95f;
                m_afWheelRotation[BIKEWHEEL_FRONT] += m_afWheelSpeed[BIKEWHEEL_FRONT];
            }
        };

        if (!rearWheelsFirst) processFront();

        // Process rear wheel
        if (m_afWheelTimer[BIKESUSP_R1] > 0.0f || m_afWheelTimer[BIKESUSP_R2] > 0.0f) {
            float rearBrake = brake, rearTraction = traction;
            CVector n = m_aWheelColPoints[rearLine].m_vecNormal;
            CVector wheelFwd = Norm(m_matrix.up - n * Dot(m_matrix.up, n));
            CVector wheelRight = Norm(Cross(wheelFwd, n));
            if (bIsHandbrakeOn) {
                rearBrake = 20000.0f * Step();   // reVC FIX_BUGS: brake has timestep as a factor
                e.tireTemperature = 1.0f;
            } else if (e.doingBurnout) {
                rearBrake = 0.0f;
                rearTraction = 0.0f;
                TurnForce(contactPoints[BIKESUSP_R1], m_matrix.right * (-0.0007f * m_fTurnMass * m_fSteerAngle * Step()));
            } else if (e.tireTemperature < 1.0f && m_fGasPedal > 0.75f) {
                rearTraction *= e.tireTemperature;
                TurnForce(contactPoints[BIKESUSP_R1], m_matrix.right * ((1.0f - e.tireTemperature) * -0.0007f * m_fTurnMass * m_fSteerAngle * Step()));
            }
            if (fThrust > 0.0f && brake > 0.0f) brake = 0.0f;
            fThrust = acceleration;
            m_aWheelColPoints[rearLine].m_nSurfaceTypeA = SURFACE_WHEELBASE;
            float adhesion = CSurfaceTable::GetAdhesiveLimit(m_aWheelColPoints[rearLine]) * rearTraction;
            float adhesionDestab = 1.0f;
            if (e.brakeDestabilization > 0.0f)
                switch (CSurfaceTable::GetAdhesionGroup(m_aWheelColPoints[rearLine].m_nSurfaceTypeB)) {
                case ADHESIVE_HARD: case ADHESIVE_LOOSE: adhesionDestab = 0.9f; break;
                case ADHESIVE_ROAD: adhesionDestab = 0.7f; break;
                }
            if (m_nStatus == STATUS_PLAYER) adhesion *= CSurfaceTable::GetWetMultiplier(m_aWheelColPoints[rearLine].m_nSurfaceTypeB);
            if (e.wheelStatus[BIKEWHEEL_REAR] == WHEEL_STATUS_BURST) adhesion *= 0.4f;
            WheelState[BIKEWHEEL_REAR] = m_aWheelState[BIKEWHEEL_REAR];
            CVector contactSpeed = GetSpeedAt(rearContact);
            ProcessBikeWheel(wheelFwd, wheelRight, contactSpeed, rearContact, 2, fThrust, rearBrake * brakeBiasRear,
                             adhesion * tractionBiasRear, adhesionDestab, BIKEWHEEL_REAR, &m_afWheelSpeed[BIKEWHEEL_REAR],
                             &WheelState[BIKEWHEEL_REAR], BIKE_WHEELSPEC_1, e.wheelStatus[BIKEWHEEL_REAR]);
            if (bStuckInSand && (WheelState[BIKEWHEEL_REAR] == WHEEL_STATE_SPINNING || WheelState[BIKEWHEEL_REAR] == WHEEL_STATE_SKIDDING))
                WheelState[BIKEWHEEL_REAR] = WHEEL_STATE_NORMAL;
        } else {
            if (bIsHandbrakeOn) m_afWheelSpeed[BIKEWHEEL_REAR] = 0.0f;
            else if (acceleration > 0.0f) { if (m_afWheelSpeed[BIKEWHEEL_REAR] < 2.0f) m_afWheelSpeed[BIKEWHEEL_REAR] -= 0.2f; }
            else { if (m_afWheelSpeed[BIKEWHEEL_REAR] > -2.0f) m_afWheelSpeed[BIKEWHEEL_REAR] += 0.1f; }
            m_afWheelRotation[BIKEWHEEL_REAR] += m_afWheelSpeed[BIKEWHEEL_REAR];
        }

        if (e.doingBurnout && m_aWheelState[BIKEWHEEL_REAR] == WHEEL_STATE_SPINNING) {
            e.tireTemperature += 0.001f * Step();
            if (e.tireTemperature > 3.0f) e.tireTemperature = 3.0f;
        } else if (e.tireTemperature > 1.0f)
            e.tireTemperature = (e.tireTemperature - 1.0f) * Pow(0.995f, Step()) + 1.0f;

        if (rearWheelsFirst) processFront();

        // Process leaning
        float idleAngle = 0.0f;
        if (m_pDriver && m_pDriver->m_pRwClump) {
            CAnimBlendAssociation *assoc = RpAnimBlendClumpGetAssociation(m_pDriver->m_pRwClump, 200 + 17 /* BIKE_STILL */);
            if (assoc) idleAngle = DegToRad(10.0f) * assoc->m_fBlendAmount;
        }
        if (bBalancedByRider || e.beingPickedUp) {
            e.avgSurfaceRight = Norm(Cross(m_matrix.up, e.avgSurfaceNormal));
            float lean;
            if (m_nWheelsOnGround == 0)
                lean = -(m_fSteerAngle / DegToRad(h->m_fSteeringLock)) * 0.5f * GRAVITY_III * Step();
            else
                lean = Dot(m_vecMoveSpeed - initialMoveSpeed, e.avgSurfaceRight);
            lean /= GRAVITY_III * (std::max)(Step(), 0.01f);
            if (e.wheelStatus[BIKEWHEEL_FRONT] == WHEEL_STATUS_BURST)
                lean = Clampf(lean, -0.4f * bh.fMaxLean, 0.4f * bh.fMaxLean);
            else
                lean = Clampf(lean, -bh.fMaxLean, bh.fMaxLean);
            float f = Pow(bh.fDesLean, Step());
            e.leanLR2 = (asinf(lean) - idleAngle) * (1.0f - f) + e.leanLR2 * f;
        } else if (e.isStanding) {
            float f = Pow(0.97f, Step());
            e.leanLR2 = e.leanLR2 * f - (asinf(Clampf(m_matrix.right.z, -1.0f, 1.0f)) + DegToRad(15.0f) + idleAngle) * (1.0f - f);
        } else {
            e.leanLR2 = e.leanLR2 * Pow(0.95f, Step());
        }
        e.leanLR = e.leanLR2;

        // Destabilize steering when braking
        if ((m_afSuspensionSpringRatio[BIKESUSP_F1] < 1.0f || m_afSuspensionSpringRatio[BIKESUSP_F2] < 1.0f) &&
            m_fBrakePedal - m_fGasPedal > 0.9f && fwdSpeed > 0.02f && !bIsHandbrakeOn) {
            e.brakeDestabilization += CGeneral::GetRandomNumberInRange(0.5f, 1.0f) * 0.2f * Step();
            float k = (m_afSuspensionSpringRatio[BIKESUSP_R1] < 1.0f || m_afSuspensionSpringRatio[BIKESUSP_R2] < 1.0f) ? 0.013f : 0.003f;
            // reVC notes the clamp below is nonsense in the original; kept for identical feel
            float c = fwdSpeed < 0.5f ? 0.5f : (fwdSpeed > 0.2f ? 0.2f : fwdSpeed);
            TurnForce(contactPoints[BIKESUSP_R1], m_matrix.right * (m_fTurnMass * sinf(e.brakeDestabilization) * c * k * Step()));
        } else
            e.brakeDestabilization = 0.0f;

        // Update wheel positions from suspension
        float frontWheelPos = colModel->m_pLines[frontLine].m_vecStart.z;
        if (m_afSuspensionSpringRatio[frontLine] > 0.0f)
            frontWheelPos -= m_afSuspensionSpringRatio[frontLine] * m_afSuspensionSpringLength[frontLine];
        m_afWheelPosition[BIKEWHEEL_FRONT] += (frontWheelPos - m_afWheelPosition[BIKEWHEEL_FRONT]) * 0.75f;
        float rearWheelPos = colModel->m_pLines[rearLine].m_vecStart.z;
        if (m_afSuspensionSpringRatio[rearLine] > 0.0f)
            rearWheelPos -= m_afSuspensionSpringRatio[rearLine] * m_afSuspensionSpringLength[rearLine];
        m_afWheelPosition[BIKEWHEEL_REAR] += (rearWheelPos - m_afWheelPosition[BIKEWHEEL_REAR]) * 0.75f;

        for (i = 0; i < 2; i++) m_aWheelState[i] = WheelState[i];
        if (m_fGasPedal < 0.0f && m_aWheelState[BIKEWHEEL_REAR] == WHEEL_STATE_SPINNING)
            m_aWheelState[BIKEWHEEL_REAR] = WHEEL_STATE_NORMAL;
        // III: car audio reads all four wheel states and the 4-wheel counters; mirror the bike's two
        m_aWheelState[2] = m_aWheelState[BIKEWHEEL_FRONT];
        m_aWheelState[3] = m_aWheelState[BIKEWHEEL_REAR];

        // Horn
        if (m_nStatus != STATUS_PLAYER) { if (m_nCarHornTimer) m_nCarHornTimer--; }
        else m_nCarHornTimer = CPad::GetPad(0)->GetHorn() ? 1 : 0;
    }

    // Engine on fire -> blow up after 5 seconds
    if (m_fHealth < 250.0f && m_nStatus != STATUS_WRECKED) {
        m_fFireBlowUpTimer += CTimer::ms_fTimeStep * 1000.0f / 50.0f;
        if (m_fFireBlowUpTimer > 5000.0f) BlowUpCarBike(m_pSetOnFireEntity);
    } else
        m_fFireBlowUpTimer = 0.0f;

    ProcessDelayedExplosion();

    for (i = 0; i < 4; i++) {
        m_afSuspensionSpringRatioPrev[i] = m_afSuspensionSpringRatio[i];
        m_afSuspensionSpringRatio[i] = 1.0f;
    }

    bVehicleColProcessed = false;
    if (!bWarnedPeds) CCarCtrl::ScanForPedDanger(this);

    if (bInfiniteMass) {
        m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f); m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
        m_vecFrictionMoveForce = CVector(0.0f, 0.0f, 0.0f); m_vecFrictionTurnForce = CVector(0.0f, 0.0f, 0.0f);
    } else if (!skipPhysics && ((acceleration == 0.0f && brake == 0.0f) || m_nStatus == STATUS_WRECKED)) {
        if (fabsf(m_vecMoveSpeed.x) < 0.005f && fabsf(m_vecMoveSpeed.y) < 0.005f && fabsf(m_vecMoveSpeed.z) < 0.005f) {
            m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
            m_vecTurnSpeed.z = 0.0f;
        }
    }

    // Balance bike
    if (bBalancedByRider || e.beingPickedUp || e.isStanding) {
        float onSideness = Clampf(Dot(m_matrix.right, e.avgSurfaceNormal), -1.0f, 1.0f);
        CVector worldCOM = Mul3x3(m_matrix, m_vecCentreOfMass);
        if (bBalancedByRider) {
            TurnForce(m_matrix.at * (-0.07f * onSideness * m_fTurnMass * Step()), worldCOM + m_matrix.right);
            e.isStanding = false;
        } else
            TurnForce(m_matrix.at * (-0.1f * onSideness * m_fTurnMass * Step()), worldCOM + m_matrix.right);

        if (m_nStatus == STATUS_PLAYER) {
            if (m_afWheelTimer[BIKESUSP_F1] == 0.0f && m_afWheelTimer[BIKESUSP_F2] == 0.0f && m_matrix.up.z > 0.0f &&
                !(m_afWheelTimer[BIKESUSP_R1] == 0.0f && m_afWheelTimer[BIKESUSP_R2] == 0.0f)) {
                float wheelie = bh.fWheelieAng - m_matrix.up.z;
                if (wheelie > 0.15f) wheelie = (std::max)(0.3f - wheelie, 0.0f);
                else if (wheelie < -0.08f) wheelie = (std::min)(-0.15f - wheelie, 0.0f);
                float wheelieStab = bh.fWheelieStabMult * (std::min)(Len(m_vecMoveSpeed), 0.1f) * wheelie;
                TurnForce(m_matrix.at * (0.5f * Step() * wheelieStab * m_fTurnMass), worldCOM + m_matrix.up);
                TurnForce(m_matrix.right * (0.5f * Step() * e.wheelAngle * bh.fWheelieSteer * m_fTurnMass), worldCOM + m_matrix.up);
            } else if (m_afWheelTimer[BIKESUSP_R1] == 0.0f && m_afWheelTimer[BIKESUSP_R2] == 0.0f && m_matrix.up.z < 0.0f &&
                       !(m_afWheelTimer[BIKESUSP_F1] == 0.0f && m_afWheelTimer[BIKESUSP_F2] == 0.0f)) {
                float stoppie = bh.fStoppieAng - m_matrix.up.z;
                if (stoppie > 0.15f) stoppie = (std::max)(0.3f - stoppie, 0.0f);
                else if (stoppie < -0.15f) stoppie = (std::min)(-0.3f - stoppie, 0.0f);
                float speed = Len(m_vecMoveSpeed);
                float stoppieStab = bh.fStoppieStabMult * (std::min)(speed, 0.1f) * stoppie;
                TurnForce(m_matrix.at * (0.5f * Step() * stoppieStab * m_fTurnMass), worldCOM + m_matrix.up);
                TurnForce(m_matrix.right * (0.5f * (std::min)(5.0f * speed, 1.0f) * Step() * e.wheelAngle * bh.fWheelieSteer * m_fTurnMass),
                          worldCOM + m_matrix.up);
            }
        }
    }

    // III: keep the audio-side "gas pedal" in step like CAutomobile does
    m_fGasPedalAudio = m_fGasPedalAudio * 0.8f + fabsf(m_fGasPedal) * 0.2f;
}
#pragma optimize("", on)

void CBike::ProcessBikeWheel(CVector &wheelFwd, CVector &wheelRight, CVector &wheelContactSpeed, CVector &wheelContactPoint,
                             int wheelsOnGround, float thrust, float brake, float adhesion, float destabTraction, int wheelId,
                             float *wheelSpeed, tWheelState *wheelState, int special, unsigned short wheelStatus) {
    bool bAlreadySkidding = false;   // reVC FIX_SIGNIFICANT_BUGS: reset every call
    tHandlingData *h = m_pHandlingData;
    float fwd = 0.0f, right = 0.0f;
    bool bBraking = brake != 0.0f;
    if (bBraking) thrust = 0.0f;
    bool bDriving = thrust != 0.0f;
    bool bReversing = thrust < 0.0f;
    float contactSpeedFwd = Dot(wheelContactSpeed, wheelFwd);
    float contactSpeedRight;
    if (*wheelState != WHEEL_STATE_NORMAL) bAlreadySkidding = true;
    *wheelState = WHEEL_STATE_NORMAL;
    adhesion *= Step();
    if (bAlreadySkidding) adhesion *= h->m_fTractionLoss;
    if (special == BIKE_WHEELSPEC_2 || special == BIKE_WHEELSPEC_3) contactSpeedRight = 0.0f;
    else contactSpeedRight = Dot(wheelContactSpeed, wheelRight);

    if (contactSpeedRight != 0.0f) {
        right = -contactSpeedRight / wheelsOnGround;
        if (wheelStatus == WHEEL_STATUS_BURST) {
            float fwdspeed = (std::min)(contactSpeedFwd, fBurstBikeSpeedMax);
            right += fwdspeed * CGeneral::GetRandomNumberInRange(-fBurstBikeTyreMod, fBurstBikeTyreMod);
        }
    }
    if (bDriving) {
        fwd = thrust;
        right = Clampf(right, -adhesion, adhesion);
    } else if (contactSpeedFwd != 0.0f) {
        fwd = -contactSpeedFwd / wheelsOnGround;
        if (!bBraking && m_fGasPedal < 0.01f)
            brake = 0.6f * mod_HandlingManager.fWheelFriction / (h->m_fMass + 200.0f) * Step();
        if (brake > adhesion) {
            if (fabsf(contactSpeedFwd) > 0.005f) *wheelState = WHEEL_STATE_FIXED;
        } else
            fwd = Clampf(fwd, -brake, brake);
    }

    float speedSq = right * right + fwd * fwd;
    if (adhesion * adhesion < speedSq) {
        if (*wheelState != WHEEL_STATE_FIXED)
            *wheelState = (bDriving && contactSpeedFwd < 0.2f) ? WHEEL_STATE_SPINNING : WHEEL_STATE_SKIDDING;
        float l = sqrtf(speedSq);
        float tractionLoss = bAlreadySkidding ? 1.0f : h->m_fTractionLoss;
        right *= adhesion * tractionLoss / l;
        fwd *= adhesion * tractionLoss / l;
        if (destabTraction < 1.0f) right *= destabTraction;
    } else if (destabTraction < 1.0f) {
        if (!bAlreadySkidding) destabTraction *= h->m_fTractionLoss;
        if ((adhesion * destabTraction) * (adhesion * destabTraction) < speedSq)
            right *= adhesion * destabTraction / sqrtf(speedSq);
    }

    if (fwd != 0.0f || right != 0.0f) {
        CVector direction = wheelFwd * fwd + wheelRight * right;
        float speed = Len(direction);
        direction = Norm(direction);
        float impulse = speed * m_fMass;
        float turnImpulse = speed * GetMassAt(wheelContactPoint, direction);
        CVector vTurnImpulse = direction * turnImpulse;
        MoveForce(direction * impulse);
        float turnRight = Dot(vTurnImpulse, m_matrix.right);
        float contactRight = Dot(wheelContactPoint, m_matrix.right);
        float contactFwd = Dot(wheelContactPoint, m_matrix.up);
        if (wheelId != BIKEWHEEL_REAR || (!bBraking && !bReversing))
            TurnForce((vTurnImpulse - m_matrix.right * turnRight) * fTweakBikeWheelTurnForce, wheelContactPoint - m_matrix.right * contactRight);
        TurnForce(m_matrix.right * turnRight, m_matrix.up * contactFwd);
    }
}

void CBike::VehicleDamage() {
    BikeExt &e = *Ext();
    float impulse = m_fDamageImpulse;
    float colSpeed = 800.0f * impulse / m_fMass;
    if (m_nStatus == STATUS_PLAYER) colSpeed *= 0.65f;
    else if (m_nCreatedBy == 2 /* MISSION_VEHICLE */) colSpeed *= 0.4f;
    if (!bCanBeDamaged) return;
    if (e.isStanding && impulse > 20.0f) e.isStanding = false;

    // Hurt the rider and throw them off on a hard hit (cars, objects, walls)
    if (m_pDriver && m_pDriver->m_ePedState == PEDSTATE_DRIVING && colSpeed > 10.0f) {
        const CVector &n = m_vecDamageNormal;
        float fwd = 0.6f;
        if (fabsf(Dot(n, m_matrix.GetForward())) > 0.85f) {
            float u = (std::max)(n.z, 0.0f);
            if (u < 0.85f) u = 0.0f;
            fwd += 7.0f * u * u;
        }
        float up = 0.05f;
        float total = fwd * fabsf(Dot(n, m_matrix.GetForward())) + 0.45f * fabsf(Dot(n, m_matrix.GetRight())) +
                      up * (std::max)(Dot(n, m_matrix.GetUp()), 0.0f);
        float damage = (total - 1.5f * (std::min)(Dot(n, m_matrix.GetUp()), 0.0f)) * colSpeed;
        if (damage > 75.0f) {
            CPed *rider = m_pDriver;
            int dir = rider->GetLocalDirection(CVector2D(-n.x, -n.y));
            if (rider->m_fHealth > 0.0f)
                rider->InflictDamage(m_pDamageEntity, WEAPONTYPE_RAMMEDBYCAR, 0.05f * damage, PEDPIECE_TORSO, (unsigned char)dir);
            if (m_pDriver == rider && rider->m_ePedState == PEDSTATE_DRIVING)
                bikes::KnockOffRider(this, WEAPONTYPE_RAMMEDBYCAR, dir, rider);
        }
    }
    if (impulse > 25.0f && m_nStatus != STATUS_WRECKED) {
        float damage = (impulse - 25.0f) * m_pHandlingData->m_fCollisionDamageMultiplier;
        if (damage > 0.0f) {
            float oldHealth = m_fHealth;
            if (this == FindPlayerVehicle()) m_fHealth -= bTakeLessDamage ? damage / 6.0f : damage / 2.0f;
            else if (bTakeLessDamage) m_fHealth -= damage / 12.0f;
            else if (m_pDamageEntity && m_pDamageEntity == FindPlayerVehicle()) m_fHealth -= damage / 1.5f;
            else m_fHealth -= damage / 4.0f;
            if (m_fHealth <= 0.0f && oldHealth > 0.0f) m_fHealth = 1.0f;
        }
    }
    if (m_fHealth < 250.0f && !e.isOnFire) {
        e.isOnFire = true;
        m_fFireBlowUpTimer = 0.0f;
        m_pSetOnFireEntity = m_pDamageEntity;
        if (m_pSetOnFireEntity) m_pSetOnFireEntity->RegisterReference(&m_pSetOnFireEntity);
    }
}

// ------------------------------------------------------------------------------------------
void CBike::TeleportBike(CVector pos) {
    CWorld::Remove(this);
    m_matrix.pos = pos;
    m_matrix.SetRotate(0.0f, 0.0f, 0.0f);
    m_matrix.pos = pos;
    m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
    m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
    ResetSuspension();
    CWorld::Add(this);
}

void CBike::PreRenderBike() {
    BikeExt &e = *Ext();
    CVehicleModelInfo *mi = MI(this);
    CColModel *colModel = mi->m_pColModel;
    CMatrix mat;
    CVector pos;

    // Wheel rotation
    CVector frontWheelFwd = Mul3x3(m_matrix, CVector(-sinf(m_fSteerAngle), cosf(m_fSteerAngle), 0.0f));
    CVector rearWheelFwd = m_matrix.up;
    if (m_afWheelTimer[BIKESUSP_F1] > 0.0f || m_afWheelTimer[BIKESUSP_F2] > 0.0f) {
        float springRatio = (std::min)(m_afSuspensionSpringRatioPrev[BIKESUSP_F1], m_afSuspensionSpringRatioPrev[BIKESUSP_F2]);
        CVector contactPoint(0.0f, (colModel->m_pLines[BIKESUSP_F1].m_vecStart.y - colModel->m_pLines[BIKESUSP_F2].m_vecStart.y) / 2.0f,
            colModel->m_pLines[BIKESUSP_F1].m_vecStart.z - m_afSuspensionSpringLength[BIKESUSP_F1] * springRatio - 0.5f * mi->m_fWheelScale);
        CVector contactSpeed = GetSpeedAt(contactPoint);
        m_afWheelSpeed[BIKEWHEEL_FRONT] = ProcessWheelRotation(WHEEL_STATE_NORMAL, frontWheelFwd, contactSpeed, 0.5f * mi->m_fWheelScale);
        m_afWheelRotation[BIKEWHEEL_FRONT] += m_afWheelSpeed[BIKEWHEEL_FRONT];
    }
    if (m_afWheelTimer[BIKESUSP_R1] > 0.0f || m_afWheelTimer[BIKESUSP_R2] > 0.0f) {
        float springRatio = (std::min)(m_afSuspensionSpringRatioPrev[BIKESUSP_R1], m_afSuspensionSpringRatioPrev[BIKESUSP_R2]);
        CVector contactPoint(0.0f, (colModel->m_pLines[BIKESUSP_R1].m_vecStart.y - colModel->m_pLines[BIKESUSP_R2].m_vecStart.y) / 2.0f,
            colModel->m_pLines[BIKESUSP_R1].m_vecStart.z - m_afSuspensionSpringLength[BIKESUSP_R1] * springRatio - 0.5f * mi->m_fWheelScale);
        CVector contactSpeed = GetSpeedAt(contactPoint);
        m_afWheelSpeed[BIKEWHEEL_REAR] = ProcessWheelRotation(m_aWheelState[BIKEWHEEL_REAR], rearWheelFwd, contactSpeed, 0.5f * mi->m_fWheelScale);
        m_afWheelRotation[BIKEWHEEL_REAR] += m_afWheelSpeed[BIKEWHEEL_REAR];
    }

    // Front fork: rotate about the (raked) steering axis
    if (e.nodes[BIKE_FORKS_FRONT]) {
        mat.Attach(RwFrameGetMatrix(e.nodes[BIKE_FORKS_FRONT]), false);
        pos = mat.pos;
        CVector forkAxis = Norm(CVector(0.0f, sinf(DegToRad(mi->m_fBikeSteerAngle)), -cosf(DegToRad(mi->m_fBikeSteerAngle))));
        RwMatrix *rw = RwMatrixCreate();
        RwMatrixRotate(rw, (RwV3d *)&forkAxis, -e.wheelAngle * 180.0f / PI_F, rwCOMBINEREPLACE);
        CMatrix rot(rw, false);
        mat.SetUnity();
        mat = mat * rot;
        mat.Translate(pos.x, pos.y, pos.z);
        mat.UpdateRW();
        if (e.nodes[BIKE_HANDLEBARS]) {
            CMatrix hm;
            hm.Attach(RwFrameGetMatrix(e.nodes[BIKE_HANDLEBARS]), false);
            CVector hp = hm.pos;
            if (m_nStatus == STATUS_ABANDONED || m_nStatus == STATUS_WRECKED) {
                hm.SetUnity(); hm = hm * rot; hm.Translate(hp.x, hp.y, hp.z);
            } else
                hm.SetTranslate(hp.x, hp.y, hp.z);
            hm.UpdateRW();
            hm.Detach();
        }
        rot.Detach();
        RwMatrixDestroy(rw);
        mat.Detach();
    }

    // Rear fork (swing arm)
    if (e.nodes[BIKE_FORKS_REAR]) {
        float sine = Clampf((m_afWheelPosition[BIKEWHEEL_REAR] - e.wheelBasePosition[BIKEWHEEL_REAR]) / e.rearForkLength, -1.0f, 1.0f);
        mat.Attach(RwFrameGetMatrix(e.nodes[BIKE_FORKS_REAR]), false);
        pos = mat.pos;
        mat.SetRotate(-asinf(sine), 0.0f, 0.0f);
        mat.Translate(pos.x, pos.y, pos.z);
        mat.UpdateRW();
        mat.Detach();
    }

    // Front wheel + mudguard
    if (e.nodes[BIKE_WHEEL_FRONT]) {
        mat.Attach(RwFrameGetMatrix(e.nodes[BIKE_WHEEL_FRONT]), false);
        pos.x = mat.pos.x;
        pos.z = m_afWheelPosition[BIKEWHEEL_FRONT] - e.frontForkZ;
        float y = (colModel->m_pLines[BIKESUSP_F1].m_vecStart.y + colModel->m_pLines[BIKESUSP_F2].m_vecStart.y) / 2.0f - e.frontForkY;
        pos.y = y - (m_afWheelPosition[BIKEWHEEL_FRONT] - e.wheelBasePosition[BIKEWHEEL_FRONT]) * e.frontForkSlope;
        if (e.wheelStatus[BIKEWHEEL_FRONT] == WHEEL_STATUS_BURST)
            mat.SetRotate(m_afWheelRotation[BIKEWHEEL_FRONT], 0.0f, 0.05f * sinf(m_afWheelRotation[BIKEWHEEL_FRONT]));
        else
            mat.SetRotateX(m_afWheelRotation[BIKEWHEEL_FRONT]);
        mat.Translate(pos.x, pos.y, pos.z);
        mat.UpdateRW();
        mat.Detach();
        if (e.nodes[BIKE_MUDGUARD]) {
            mat.Attach(RwFrameGetMatrix(e.nodes[BIKE_MUDGUARD]), false);
            mat.pos = pos;
            mat.UpdateRW();
            mat.Detach();
        }
    }

    // Rear wheel
    if (e.nodes[BIKE_WHEEL_REAR]) {
        mat.Attach(RwFrameGetMatrix(e.nodes[BIKE_WHEEL_REAR]), false);
        pos = mat.pos;
        if (e.wheelStatus[BIKEWHEEL_REAR] == WHEEL_STATUS_BURST)
            mat.SetRotate(m_afWheelRotation[BIKEWHEEL_REAR], 0.0f, 0.07f * sinf(m_afWheelRotation[BIKEWHEEL_REAR]));
        else
            mat.SetRotateX(m_afWheelRotation[BIKEWHEEL_REAR]);
        mat.Translate(pos.x, pos.y, pos.z);
        mat.UpdateRW();
        mat.Detach();
    }

    // Chassis lean
    if (e.nodes[BIKE_CHASSIS]) {
        mat.Attach(RwFrameGetMatrix(e.nodes[BIKE_CHASSIS]), false);
        pos = mat.pos;
        pos.z = (1.0f - cosf(e.leanLR)) * (0.9f * colModel->m_boundBox.m_vecMin.z);
        mat.SetRotateX(-0.05f * fabsf(e.leanLR));
        mat.RotateY(e.leanLR);
        mat.Translate(pos.x, pos.y, pos.z);
        mat.UpdateRW();
        mat.Detach();
    }
    CalculateLeanMatrix();
}

void CBike::RenderBike() {
    MI(this)->SetVehicleColour(m_nPrimaryColor, m_nSecondaryColor);
    CallMethod<0x474BD0, CEntity *>(this);   // CEntity::Render (non-virtual)
}

int CBike::ProcessEntityCollisionBike(CEntity *ent, CColPoint *colpoints) {
    if (m_nStatus != STATUS_SIMPLE) bVehicleColProcessed = true;
    CColModel *colModel = ColModelOf(this);
    int numWheelCollisions = 0;
    float prevRatios[4];
    for (int i = 0; i < 4; i++) prevRatios[i] = m_afSuspensionSpringRatio[i];
    if (m_bIsVehicleBeingShifted || bSkipLineCol || ent->m_nType == ENTITY_TYPE_PED) colModel->m_nNumOfLines = 0;

    CColModel *entCol = CModelInfo::ms_modelInfoPtrs[ent->m_nModelIndex]->m_pColModel;
    int numCollisions = CCollision::ProcessColModels(m_matrix, *colModel, ent->m_matrix, *entCol, colpoints,
                                                     m_aWheelColPoints, m_afSuspensionSpringRatio);
    if (colModel->m_nNumOfLines) {
        for (int i = 0; i < 4; i++) {
            if (m_afSuspensionSpringRatio[i] < 1.0f && m_afSuspensionSpringRatio[i] < prevRatios[i]) {
                numWheelCollisions++;
                if (ent->m_nType == ENTITY_TYPE_VEHICLE || ent->m_nType == ENTITY_TYPE_OBJECT) {
                    CPhysical *phys = (CPhysical *)ent;
                    if (m_aGroundPhysical[i] != phys) {
                        if (m_aGroundPhysical[i]) CleanUpRef(m_aGroundPhysical[i], reinterpret_cast<CEntity **>(&m_aGroundPhysical[i]));
                        m_aGroundPhysical[i] = phys;
                        phys->RegisterReference(reinterpret_cast<CEntity **>(&m_aGroundPhysical[i]));
                    }
                    m_avecGroundOffset[i] = m_aWheelColPoints[i].m_vecPoint - phys->m_matrix.pos;
                }
                m_nSurfaceTouched = m_aWheelColPoints[i].m_nSurfaceTypeB;
                if (ent->m_nType == ENTITY_TYPE_BUILDING) m_pCurGroundEntity = ent;
            }
        }
    } else
        colModel->m_nNumOfLines = 4;

    if (numCollisions > 0 || numWheelCollisions > 0) {
        AddCollisionRecord(ent);
        if (ent->m_nType != ENTITY_TYPE_BUILDING) ((CPhysical *)ent)->AddCollisionRecord(this);
        if (numCollisions > 0 && (ent->m_nType == ENTITY_TYPE_BUILDING ||
                                  (ent->m_nType == ENTITY_TYPE_OBJECT && ((CPhysical *)ent)->bInfiniteMass)))
            bHasHitWall = true;
    }
    return numCollisions;
}

void CBike::ProcessControlInputsBike(unsigned char padId) {
    BikeExt &e = *Ext();
    CPad *pad = CPad::GetPad(padId);
    float speed = Dot(m_vecMoveSpeed, m_matrix.up);
    bIsHandbrakeOn = pad->GetExitVehicle() ? true : !!pad->GetHandBrake();

    m_fSteerInput += (-pad->GetSteeringLeftRight() / 128.0f - m_fSteerInput) * 0.2f * Step();
    m_fSteerInput = Clampf(m_fSteerInput, -1.0f, 1.0f);

    float updown = -pad->GetSteeringUpDown() / 128.0f + pad->GetCarGunUpDown() / 128.0f;
    e.leanInput += (updown - e.leanInput) * 0.2f * Step();
    e.leanInput = Clampf(e.leanInput, -1.0f, 1.0f);

    float acceleration = (pad->GetAccelerate() - pad->GetBrake()) / 255.0f;
    if (fabsf(speed) < 0.01f) {
        if (pad->GetAccelerate() > 150 && pad->GetBrake() > 150) {
            m_fGasPedal = pad->GetAccelerate() / 255.0f;
            m_fBrakePedal = pad->GetBrake() / 255.0f;
            e.doingBurnout = 1;
        } else {
            m_fGasPedal = acceleration;
            m_fBrakePedal = 0.0f;
        }
    } else if (speed * acceleration < 0.0f) {
        m_fGasPedal = 0.0f;
        m_fBrakePedal = fabsf(acceleration);
    } else {
        m_fGasPedal = acceleration;
        m_fBrakePedal = 0.0f;
    }

    float v = m_fSteerInput < 0.0f ? -m_fSteerInput * m_fSteerInput : m_fSteerInput * m_fSteerInput;
    m_fSteerAngle = DegToRad(m_pHandlingData->m_fSteeringLock) * v;

    if (pad->DisablePlayerControls) {
        m_fBrakePedal = 1.0f;
        bIsHandbrakeOn = true;
        m_fGasPedal = 0.0f;
        float s = Len(m_vecMoveSpeed);
        if (s > 0.28f) m_vecMoveSpeed = m_vecMoveSpeed * (0.28f / s);
    }
}

void CBike::GetComponentWorldPositionBike(int component, CVector &pos) {
    BikeExt &e = *Ext();
    if (component < 0 || component >= BIKE_NUM_NODES || !e.nodes[component]) { pos = m_matrix.pos; return; }
    RwMatrix *ltm = RwFrameGetLTM(e.nodes[component]);
    pos = CVector(ltm->pos.x, ltm->pos.y, ltm->pos.z);
}

void CBike::RemoveRefsToVehicleBike(CEntity *ent) {
    for (int i = 0; i < 4; i++)
        if (m_aGroundPhysical[i] == ent) m_aGroundPhysical[i] = nullptr;
}

void CBike::BlowUpCarBike(CEntity *culprit) {
    if (!bCanBeDamaged) return;
    // III: throw the rider off first; the explosion then hurts them like any ped nearby
    if (m_pDriver) bikes::DismountInstantly(m_pDriver, this);
    m_vecMoveSpeed.z += 0.13f;
    m_nStatus = STATUS_WRECKED;
    bRenderScorched = true;
    m_fHealth = 0.0f;
    m_nBombTimer = 0;
    TheCamera.CamShake(0.7f, m_matrix.pos.x, m_matrix.pos.y, m_matrix.pos.z);
    bEngineOn = false;
    bLightsOn = false;
    ChangeLawEnforcerState(false);
    CExplosion::AddExplosion(this, culprit, EXPLOSION_CAR, m_matrix.pos, 0);
}

bool CBike::SetUpWheelColModelBike(CColModel *colModel) {
    BikeExt &e = *Ext();
    if (!e.nodes[BIKE_WHEEL_FRONT] || !e.nodes[BIKE_WHEEL_REAR] || !colModel->m_pSpheres) return false;
    RwMatrix *mat = RwMatrixCreate();
    CVehicleModelInfo *mi = MI(this);
    CColModel *vehColModel = mi->m_pColModel;
    colModel->m_boundSphere = vehColModel->m_boundSphere;
    colModel->m_boundBox = vehColModel->m_boundBox;
    GetRelativeMatrix(mat, e.nodes[BIKE_WHEEL_FRONT], e.nodes[BIKE_CHASSIS]);
    colModel->m_pSpheres[0].m_vecCenter = CVector(mat->pos.x, mat->pos.y, mat->pos.z);
    colModel->m_pSpheres[0].m_fRadius = 0.5f * mi->m_fWheelScale;
    GetRelativeMatrix(mat, e.nodes[BIKE_WHEEL_REAR], e.nodes[BIKE_CHASSIS]);
    colModel->m_pSpheres[1].m_vecCenter = CVector(mat->pos.x, mat->pos.y, mat->pos.z);
    colModel->m_pSpheres[1].m_fRadius = 0.5f * mi->m_fWheelScale;
    colModel->m_nNumOfSpheres = 2;
    RwMatrixDestroy(mat);
    return true;
}

void CBike::BurstTyreBike(unsigned char wheel) {
    BikeExt &e = *Ext();
    int w = (wheel == 0 || wheel == 2) ? BIKEWHEEL_FRONT : BIKEWHEEL_REAR;   // III passes car wheel ids
    if (e.wheelStatus[w] == WHEEL_STATUS_OK) {
        e.wheelStatus[w] = WHEEL_STATUS_BURST;
        if (m_nStatus == STATUS_SIMPLE) m_nStatus = STATUS_PHYSICS;
        TurnForce(m_matrix.right * (CGeneral::GetRandomNumberInRange(-0.02f, 0.02f) * m_fTurnMass), m_matrix.up);
    }
}

bool CBike::IsRoomForPedToLeaveCarBike(unsigned int, CVector *) { return true; }

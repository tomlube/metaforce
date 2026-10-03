#include "Metaforce/Randomizer/DoorLocks.hpp"

#include "Metaforce/MergedWorld.hpp"
#include "Metaforce/Randomizer/DoorStyles.hpp"
#include "Metaforce/Randomizer/DoorTables.hpp"
#include "Metaforce/Randomizer/Hooks.hpp"
#include "Metaforce/Randomizer/Randomizer.hpp"
#include "Metaforce/Randomizer/Seed.hpp"

#include "Collision/CMaterialList.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "MetroidPrime/CActorParameters.hpp"
#include "MetroidPrime/CDamageVulnerability.hpp"
#include "MetroidPrime/CEntity.hpp"
#include "MetroidPrime/CEntityInfo.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMappableObject.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CCameraShakeData.hpp"
#include "MetroidPrime/ScriptObjects/CScriptActor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDamageableTrigger.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPointOfInterest.hpp"
#include "MetroidPrime/ScriptObjects/CScriptStreamedMusic.hpp"

#include <borealis/log.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <vector>

namespace metaforce::randomizer {

const DoorLocationInfo* FindDoorLocation(uint32_t mrea, int dock) {
  static const std::unordered_map< uint64_t, const DoorLocationInfo* > kByDock = [] {
    std::unordered_map< uint64_t, const DoorLocationInfo* > map;
    for (int i = 0; i < kDoorLocationCount; ++i) {
      const DoorLocationInfo& door = kDoorLocations[i];
      map.emplace((static_cast< uint64_t >(door.mrea) << 32) | static_cast< uint32_t >(door.dock),
                  &door);
    }
    return map;
  }();
  const auto it = kByDock.find((static_cast< uint64_t >(mrea) << 32) | static_cast< uint32_t >(dock));
  return it == kByDock.end() ? nullptr : it->second;
}

namespace {
constexpr borealis::Log Log{"randomizer"};

// The explosion randomprime's blast shields make when they break, and the jingle after it.
constexpr ushort kBreakSfx = 3621;
constexpr const char* kBreakJingle = "/audio/evt_x_event_00.dsp";
constexpr uint kBreakJingleVolume = 92;

uint64_t ObjectKey(uint32_t world, uint32_t editorId) {
  return (static_cast< uint64_t >(world) << 32) | (editorId & 0x3FFFFFF);
}

struct DoorStyle {
  DoorColor color;
  bool vertical;
};

struct State {
  bool active = false;
  bool lockOn = false;
  // Restyled doors' shield actors, damageable triggers and Door objects, by ObjectKey.
  std::unordered_map< uint64_t, DoorStyle > shieldActors;
  std::unordered_map< uint64_t, DoorStyle > forces;
  std::unordered_map< uint64_t, DoorStyle > doors;
  // Blast shields by room (MREA): the dock and the shield.
  std::unordered_map< uint32_t, std::vector< std::pair< int, BlastShield > > > blastShields;
  std::array< std::array< uint8_t, kVulnerabilitySize >, static_cast< size_t >(DoorColor::Count) >
      vulnerabilities;
};

State& S() {
  static State state;
  return state;
}

// Whether a model or texture can be used: one of the game's, or a custom asset that builds.
bool IsAvailable(uint32_t asset) {
  if (GetCustomAssetType(asset) == 0) {
    return true;
  }
  const unsigned char* data;
  unsigned int size;
  return GetCustomAsset(asset, data, size);
}

CDamageVulnerability MakeVulnerability(DoorColor color) {
  const auto& bytes = S().vulnerabilities[static_cast< size_t >(color)];
  CMemoryInStream in(bytes.data(), bytes.size());
  return CDamageVulnerability(in);
}

// As ScriptLoader builds the transforms of script objects from their editor rotations.
CTransform4f EditorTransform(const CVector3f& degrees, const CVector3f& position) {
  const CQuaternion quat = CQuaternion::ZRotation(CRelAngle::FromDegrees(degrees.GetZ())) *
                           CQuaternion::YRotation(CRelAngle::FromDegrees(degrees.GetY())) *
                           CQuaternion::XRotation(CRelAngle::FromDegrees(degrees.GetX()));
  const CMatrix3f mat = quat.BuildTransform();
  const CVector3f& r0 = mat.GetRow(kDX);
  const CVector3f& r1 = mat.GetRow(kDY);
  const CVector3f& r2 = mat.GetRow(kDZ);
  return CTransform4f::FromColumns(CVector3f(r0[kDX], r1[kDX], r2[kDX]),
                                   CVector3f(r0[kDY], r1[kDY], r2[kDY]),
                                   CVector3f(r0[kDZ], r1[kDZ], r2[kDZ]), position);
}

// Where randomprime puts a blast shield and the damageable trigger taking its shots, from the
// door's shield actor and the door's rotation. randomprime's offsets, case for case.
struct Placement {
  CVector3f position;
  CVector3f rotation; // degrees
  CVector3f scale;
  CVector3f triggerPosition;
  CVector3f triggerScale;
};

std::optional< Placement > PlaceBlastShield(const DoorLocationInfo& door, const CVector3f& shield) {
  const float r0 = door.rotation[0];
  const float r1 = door.rotation[1];
  const float r2 = door.rotation[2];
  const CVector3f doorRotation(r0, r1, r2);
  Placement p{shield, doorRotation, CVector3f(1.f, 1.5f, 1.5f), shield, CVector3f(1.f, 1.f, 1.f)};
  const auto offset = [&shield](float x, float y, float z) {
    return CVector3f(shield.GetX() + x, shield.GetY() + y, shield.GetZ() + z);
  };
  bool ceiling = false;
  bool floor = false;
  if (door.vertical) {
    p.scale = CVector3f(1.1776f, 1.8f, 1.8f);
    if (r0 > -90.f && r0 < 90.f) {
      ceiling = true;
      p.position = offset(0.016708f, -2.141243f, 0.40522f);
      p.rotation = CVector3f(0.f, -90.f, -90.f);
    } else if (r0 < -90.f && r0 > -270.f) {
      floor = true;
      p.position = offset(-0.0112f, -2.140015f, -0.371151f);
      p.rotation = CVector3f(-90.f, 90.f, 0.f);
    } else {
      return std::nullopt;
    }
  } else if (r0 >= 11.f && r0 < 13.f) {
    p.position = offset(0.374077f, -0.406525f, -1.762893f); // Biotech Research Area 1, south
  } else if (r0 >= -13.f && r0 < -11.f) {
    p.position = offset(0.374184f, 0.392502f, -1.763191f); // Biotech Research Area 1, north
  } else if (r1 >= 8.f && r1 < 9.f) {
    p.position = offset(-0.00595f, 0.383209f, -1.801748f); // Hive Totem, north
  } else if (r0 >= 8.f && r0 < 9.f) {
    p.position = offset(-0.406285f, -0.27829f, -1.780129f); // Hive Totem, west
  } else if (r0 >= -9.f && r0 < -7.f) {
    p.position = offset(0.392498f, -0.27829f, -1.780126f); // Hive Totem, east
  } else if (r2 >= 45.f && r2 < 135.f) {
    p.position = offset(-0.00595f, 0.383209f, -1.801748f); // north
  } else if ((r2 >= 135.f && r2 < 225.f) || (r2 < -135.f && r2 > -225.f)) {
    p.position = offset(-0.383225f, 0.f, -1.80175f); // west
  } else if (r2 >= -135.f && r2 < -45.f) {
    p.position = offset(-0.00769f, -0.383224f, -1.801752f); // south
  } else if (r2 >= -45.f && r2 < 45.f) {
    p.position = offset(0.392517f, 0.f, -1.801746f); // east
  } else {
    return std::nullopt;
  }

  // The damageable trigger, a slab just behind the blast shield.
  constexpr float kSide = 0.35f;
  constexpr float kUp = 2.f;
  constexpr float kBack = 1.f;
  const CVector3f& pos = p.position;
  const auto at = [&pos](float x, float y, float z) {
    return CVector3f(pos.GetX() + x, pos.GetY() + y, pos.GetZ() + z);
  };
  const CVector3f facingY(5.f, 0.875f, 4.f);
  const CVector3f facingX(0.875f, 5.f, 4.f);
  if (ceiling) {
    p.triggerPosition = at(0.f, kUp, -kBack);
    p.triggerScale = CVector3f(5.f, 5.f, 0.875f);
  } else if (floor) {
    p.triggerPosition = at(0.f, kUp, kBack);
    p.triggerScale = CVector3f(5.f, 5.f, 0.875f);
  } else if (r0 >= -15.f && r0 < -10.f) {
    p.triggerPosition = at(-kSide, -kBack, kUp);
    p.triggerScale = facingY;
  } else if (r0 >= 10.f && r0 < 15.f) {
    p.triggerPosition = at(-kSide, kBack, kUp);
    p.triggerScale = facingY;
  } else if (r0 >= 8.f && r0 < 9.f) {
    p.triggerPosition = at(kBack, kSide, kUp);
    p.triggerScale = facingX;
  } else if (r0 >= -9.f && r0 < -7.f) {
    p.triggerPosition = at(-kBack, kSide, kUp);
    p.triggerScale = facingX;
  } else if (r2 >= 45.f && r2 < 135.f) {
    p.triggerPosition = at(0.f, -kBack, kUp);
    p.triggerScale = facingY;
  } else if ((r2 >= 135.f && r2 < 225.f) || (r2 < -135.f && r2 > -225.f)) {
    p.triggerPosition = at(kBack, 0.f, kUp);
    p.triggerScale = facingX;
  } else if (r2 >= -135.f && r2 < -45.f) {
    p.triggerPosition = at(0.f, kBack, kUp);
    p.triggerScale = facingY;
  } else {
    p.triggerPosition = at(-kBack, 0.f, kUp);
    p.triggerScale = facingX;
  }
  return p;
}

// Runs one blast shield: keeps the door's own damageable triggers from taking damage while it
// stands, and breaks it when its damageable trigger is destroyed, opening the door the way a
// shot to the door would. Opening the door from the other side removes it quietly, like
// randomprime's blast shields.
class CDoorLockShield : public CEntity {
public:
  CDoorLockShield(TUniqueId uid, const CEntityInfo& info, uint32_t mrea, int dock,
                  const CVector3f& position, TUniqueId actor, TUniqueId trigger, TUniqueId scan,
                  TUniqueId jingle, TUniqueId door, const TUniqueId (&forces)[2])
  : CEntity(uid, info, true, rstl::string_l("Randomizer Blast Shield"))
  , mMrea(mrea)
  , mDock(dock)
  , mPosition(position)
  , mActor(actor)
  , mTrigger(trigger)
  , mScan(scan)
  , mJingle(jingle)
  , mDoor(door) {
    mForces[0] = forces[0];
    mForces[1] = forces[1];
  }

#ifndef HAS_TYPES_MATCH
  void Accept(IVisitor&) override {}
#endif

  void Think(float, CStateManager& mgr) override {
    if (mDone) {
      return;
    }
    if (!mPrimed) {
      mPrimed = true;
      SendToForces(kSM_Increment, mgr);
    }
    if (const CScriptDoor* door = TCastToConstPtr< CScriptDoor >(mgr.ObjectById(mDoor))) {
      if (door->IsOpen()) {
        Remove(mgr);
        return;
      }
    }
    if (CScriptDamageableTrigger* trigger =
            TCastToPtr< CScriptDamageableTrigger >(mgr.ObjectById(mTrigger))) {
      if (trigger->HealthInfo(mgr)->GetHP() <= 0.f) {
        Break(mgr);
      }
    }
  }

private:
  void SendToForces(EScriptObjectMessage msg, CStateManager& mgr) {
    for (const TUniqueId force : mForces) {
      if (CEntity* entity = mgr.ObjectById(force)) {
        entity->AcceptScriptMsg(msg, GetUniqueId(), mgr);
      }
    }
  }

  void Remove(CStateManager& mgr) {
    mDone = true;
    MarkBlastShieldDestroyed(mMrea, mDock);
    for (const TUniqueId id : {mActor, mTrigger, mScan}) {
      if (id != kInvalidUniqueId) {
        mgr.DeleteObjectRequest(id);
      }
    }
    SendToForces(kSM_Decrement, mgr);
  }

  void Break(CStateManager& mgr) {
    Remove(mgr);
    CSfxManager::AddEmitter(kBreakSfx, mPosition, CVector3f::Zero(), true, false,
                            CSfxManager::kMedPriority, GetAreaId().Value());
    if (CEntity* jingle = mgr.ObjectById(mJingle)) {
      jingle->AcceptScriptMsg(kSM_Play, GetUniqueId(), mgr);
    }
    mgr.CameraManager()->AddCameraShaker(CCameraShakeData::HardBothAxesShake(0.5f, 0.2f), true);
    // A door without power keeps its triggers off; it opens normally once it has power.
    for (const TUniqueId force : mForces) {
      if (CScriptDamageableTrigger* trigger =
              TCastToPtr< CScriptDamageableTrigger >(mgr.ObjectById(force))) {
        if (trigger->GetActive()) {
          trigger->HealthInfo(mgr)->SetHP(0.f);
        }
      }
    }
  }

  uint32_t mMrea;
  int mDock;
  CVector3f mPosition;
  TUniqueId mActor;
  TUniqueId mTrigger;
  TUniqueId mScan;
  TUniqueId mJingle;
  TUniqueId mDoor;
  TUniqueId mForces[2];
  bool mPrimed = false;
  bool mDone = false;
};

TUniqueId FindLoadedObject(const CStateManager& mgr, TAreaId area, uint32_t editorId) {
  if (editorId == 0) {
    return kInvalidUniqueId;
  }
  return mgr.GetIdForScript(TEditorId(merged::ToLoadedEditorId(area, editorId)));
}

void SpawnBlastShield(CStateManager& mgr, TAreaId area, uint32_t mrea, int dock,
                      BlastShield shield) {
  const DoorLocationInfo* door = FindDoorLocation(mrea, dock);
  if (door == nullptr) {
    Log.warn("No door to put a blast shield on at dock {} of room {:08X}", dock, mrea);
    return;
  }
  const CActor* shieldActor =
      TCastToConstPtr< CActor >(mgr.GetObjectById(FindLoadedObject(mgr, area, door->shields[0])));
  if (shieldActor == nullptr) {
    Log.warn("No shield actor for the blast shield at dock {} of room {:08X}", dock, mrea);
    return;
  }
  const std::optional< Placement > placement =
      PlaceBlastShield(*door, shieldActor->GetTransform().GetTranslation());
  const BlastShieldStyle& style = GetBlastShieldStyle(shield);
  if (!placement || !IsAvailable(style.model)) {
    Log.warn("Can't place the blast shield at dock {} of room {:08X}", dock, mrea);
    return;
  }

  const CEntityInfo info(area, CEntity::NullConnectionList, kInvalidEditorId);
  const CDamageVulnerability vulnerability = MakeVulnerability(style.counterpart);
  const CVisorParameters visor(0xF, false, true);

  // The blast shield itself, only for show: shots go through it to the trigger behind.
  const CTransform4f xf = EditorTransform(placement->rotation, placement->position);
  const CModelData model(CStaticRes(style.model, placement->scale));
  CMaterialList materials;
  materials.Add(kMT_Immovable);
  const CLightParameters lights(true, 1.f, CLightParameters::kST_Zero, 1.f, 20.f, CColor::White(),
                                true, CLightParameters::kLO_NormalWorld,
                                CLightParameters::kLR_EightFrames, CVector3f::Zero(), 4, 4, false,
                                0);
  const CActorParameters actorParms(
      lights, CScannableParameters(kInvalidAssetId),
      rstl::pair< CAssetId, CAssetId >(kInvalidAssetId, kInvalidAssetId),
      rstl::pair< CAssetId, CAssetId >(kInvalidAssetId, kInvalidAssetId), visor, true, false,
      false, false, 1.f, 1.f, 1.f);
  CScriptActor* actor = rs_new CScriptActor(
      mgr.AllocateUniqueId(), rstl::string_l("Randomizer Blast Shield Model"), info, xf, model,
      model.GetBounds(xf.GetRotation()), materials, 1.f, 0.f, CHealthInfo(1.f, 1.f),
      CDamageVulnerability::PassThroughVulnerability(), actorParms, true, true, 0, 1.f, false,
      false, false, false);
  mgr.AddObject(actor);

  CScriptDamageableTrigger* trigger = rs_new CScriptDamageableTrigger(
      mgr.AllocateUniqueId(), rstl::string_l("Randomizer Blast Shield Trigger"), info,
      placement->triggerPosition, placement->triggerScale, CHealthInfo(1.f, 1.f), vulnerability, 0,
      kInvalidAssetId, kInvalidAssetId, kInvalidAssetId,
      S().lockOn && style.lockOn ? CScriptDamageableTrigger::kCO_Orbit
                                 : CScriptDamageableTrigger::kCO_NoOrbit,
      true, visor);
  mgr.AddObject(trigger);

  TUniqueId scan = kInvalidUniqueId;
  if (style.scan != 0) {
    const CVector3f scanPosition = placement->triggerPosition + CVector3f(0.f, 0.f, 0.5f);
    CScriptPointOfInterest* poi = rs_new CScriptPointOfInterest(
        mgr.AllocateUniqueId(), rstl::string_l("Randomizer Blast Shield Scan"), info,
        CTransform4f::Translate(scanPosition), true, CScannableParameters(style.scan), 0.f);
    mgr.AddObject(poi);
    scan = poi->GetUniqueId();
  }

  CScriptStreamedMusic* jingle = rs_new CScriptStreamedMusic(
      mgr.AllocateUniqueId(), info, rstl::string_l("Randomizer Blast Shield Jingle"), true,
      rstl::string_l(kBreakJingle), false, 0.f, 0.f, kBreakJingleVolume, false, true);
  mgr.AddObject(jingle);

  const TUniqueId forces[2] = {FindLoadedObject(mgr, area, door->forces[0]),
                               FindLoadedObject(mgr, area, door->forces[1])};
  mgr.AddObject(rs_new CDoorLockShield(mgr.AllocateUniqueId(), info, mrea, dock,
                                       placement->position, actor->GetUniqueId(),
                                       trigger->GetUniqueId(), scan, jingle->GetUniqueId(),
                                       FindLoadedObject(mgr, area, door->door), forces));
}

// The map icon type for a door color, keeping the floor and ceiling variants the game has for
// Ice, Wave and Plasma doors.
CMappableObject::EMappableObjectType MapType(MapDoorColor color, int original) {
  int variant = -1; // ceiling, floor, second floor kind
  switch (original) {
  case CMappableObject::kMOT_IceDoorCeiling:
  case CMappableObject::kMOT_WaveDoorCeiling:
  case CMappableObject::kMOT_PlasmaDoorCeiling:
    variant = 0;
    break;
  case CMappableObject::kMOT_IceDoorFloor:
  case CMappableObject::kMOT_WaveDoorFloor:
  case CMappableObject::kMOT_PlasmaDoorFloor:
    variant = 1;
    break;
  case CMappableObject::kMOT_IceDoorFloor2:
  case CMappableObject::kMOT_WaveDoorFloor2:
  case CMappableObject::kMOT_PlasmaDoorFloor2:
    variant = 2;
    break;
  default:
    break;
  }
  using T = CMappableObject;
  static const T::EMappableObjectType kVariants[3][3] = {
      {T::kMOT_IceDoorCeiling, T::kMOT_IceDoorFloor, T::kMOT_IceDoorFloor2},
      {T::kMOT_WaveDoorCeiling, T::kMOT_WaveDoorFloor, T::kMOT_WaveDoorFloor2},
      {T::kMOT_PlasmaDoorCeiling, T::kMOT_PlasmaDoorFloor, T::kMOT_PlasmaDoorFloor2},
  };
  switch (color) {
  case MapDoorColor::Blue:
    return T::kMOT_BlueDoor;
  case MapDoorColor::Shield:
    return T::kMOT_ShieldDoor;
  case MapDoorColor::Ice:
    return variant >= 0 ? kVariants[0][variant] : T::kMOT_IceDoor;
  case MapDoorColor::Wave:
    return variant >= 0 ? kVariants[1][variant] : T::kMOT_WaveDoor;
  case MapDoorColor::Plasma:
    return variant >= 0 ? kVariants[2][variant] : T::kMOT_PlasmaDoor;
  }
  return T::kMOT_BlueDoor;
}

const DoorStyle* FindStyle(const std::unordered_map< uint64_t, DoorStyle >& map,
                           unsigned int worldId, unsigned int editorId) {
  if (!S().active) {
    return nullptr;
  }
  editorId = merged::ToSourceEditorId(editorId, worldId);
  const auto it = map.find(ObjectKey(worldId, editorId));
  return it == map.end() ? nullptr : &it->second;
}

} // namespace

namespace door_locks {

void Activate(const Seed& seed) {
  Deactivate();
  if (!seed.doorLocksRandomized) {
    return;
  }
  State& s = S();
  s.active = true;
  s.lockOn = seed.blastShieldLockOn;
  for (size_t color = 0; color < s.vulnerabilities.size(); ++color) {
    uint8_t bytes[kVulnerabilitySize];
    WriteDoorVulnerability(static_cast< DoorColor >(color), bytes);
    std::copy(std::begin(bytes), std::end(bytes), s.vulnerabilities[color].begin());
  }
  for (const DoorLock& lock : seed.doorLocks) {
    const DoorLocationInfo* door = FindDoorLocation(lock.area, lock.dock);
    DoorColor color;
    if (door == nullptr || !ParseDoorColor(lock.shield, color)) {
      Log.warn("Can't restyle {} as a {} door", lock.name, lock.shield);
      continue;
    }
    const DoorStyle style{color, door->vertical};
    s.doors[ObjectKey(lock.world, door->door)] = style;
    for (const uint32_t id : door->shields) {
      if (id != 0) {
        s.shieldActors[ObjectKey(lock.world, id)] = style;
      }
    }
    for (const uint32_t id : door->forces) {
      if (id != 0) {
        s.forces[ObjectKey(lock.world, id)] = style;
      }
    }
    BlastShield shield;
    if (!lock.blastShield.empty()) {
      if (ParseBlastShield(lock.blastShield, shield)) {
        s.blastShields[lock.area].emplace_back(lock.dock, shield);
      } else {
        Log.warn("Unknown blast shield {} on {}", lock.blastShield, lock.name);
      }
    }
  }
}

void Deactivate() {
  State& s = S();
  s.active = false;
  s.lockOn = false;
  s.shieldActors.clear();
  s.forces.clear();
  s.doors.clear();
  s.blastShields.clear();
}

} // namespace door_locks

bool GetDoorShieldModel(unsigned int worldId, unsigned int editorId, unsigned int& model) {
  const DoorStyle* style = FindStyle(S().shieldActors, worldId, editorId);
  if (style == nullptr) {
    return false;
  }
  const DoorColorStyle& color = GetDoorColorStyle(style->color);
  const uint32_t shieldModel = style->vertical ? color.verticalShieldModel : color.shieldModel;
  if (!IsAvailable(shieldModel)) {
    return false;
  }
  model = shieldModel;
  return true;
}

bool RemoveBlastShieldActor(unsigned int model) {
  return S().active && model == kMissileBlastShieldModel;
}

bool RemoveBlastShieldScan(unsigned int scan) {
  return S().active && scan == GetBlastShieldStyle(BlastShield::Missile).scan;
}

bool GetDoorForceOverride(unsigned int worldId, unsigned int editorId, DoorForceOverride& out) {
  const DoorStyle* style = FindStyle(S().forces, worldId, editorId);
  if (style == nullptr) {
    return false;
  }
  const DoorColorStyle& color = GetDoorColorStyle(style->color);
  out.vulnerability = S().vulnerabilities[static_cast< size_t >(style->color)].data();
  out.vulnerabilitySize = kVulnerabilitySize;
  out.texturesChanged =
      IsAvailable(color.pattern0) && IsAvailable(color.pattern1) && IsAvailable(color.color);
  out.pattern0 = color.pattern0;
  out.pattern1 = color.pattern1;
  out.color = color.color;
  return true;
}

void SpawnDoorLocks(CStateManager& mgr, int areaIndex) {
  const State& s = S();
  if (!s.active || mgr.GetWorld() == nullptr || areaIndex < 0) {
    return;
  }
  const TAreaId area(areaIndex);
  const uint32_t mrea = mgr.GetWorld()->GetAreaAlways(area).GetAreaAssetId();
  const auto it = s.blastShields.find(mrea);
  if (it == s.blastShields.end()) {
    return;
  }
  for (const auto& [dock, shield] : it->second) {
    if (!IsBlastShieldDestroyed(mrea, dock)) {
      SpawnBlastShield(mgr, area, mrea, dock, shield);
    }
  }
}

bool GetDoorMapType(unsigned int worldId, unsigned int editorId, int& type) {
  // The map already passes the region and editor id the door was read from.
  const State& s = S();
  const auto it = s.doors.find(ObjectKey(worldId, editorId));
  if (!s.active || it == s.doors.end()) {
    return false;
  }
  type = MapType(GetDoorColorStyle(it->second.color).map, type);
  return true;
}

} // namespace metaforce::randomizer

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
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/ScriptObjects/CScriptActor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDamageableTrigger.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptEffect.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPointOfInterest.hpp"
#include "MetroidPrime/ScriptObjects/CScriptStreamedMusic.hpp"
#include "MetroidPrime/ScriptObjects/CScriptTrigger.hpp"

#include <borealis/log.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
// The gibs a blast shield breaks into: randomprime's "gibbs effect", a particle that plays once
// from the shield's place. randomprime leaves it out of its patched game for memory, which isn't
// a worry here.
constexpr CAssetId kGibsPart = 0xCDCBDF04;

uint64_t ObjectKey(uint32_t world, uint32_t editorId) {
  return (static_cast< uint64_t >(world) << 32) | (editorId & 0x3FFFFFF);
}

struct DoorStyle {
  DoorColor color;
  bool vertical;
  // The door's dock and whether it has a blast shield. Like the lock's requirements say, the
  // door behind a blast shield is a blue door once the shield is gone.
  uint32_t mrea;
  int dock;
  bool blastShield;
};

// Colors of locks rather than doors: randomprime gives the door behind a blast shield the
// shield's color, but in Randovania's logic that door opens like a blue one. They're only shown
// while the blast shield stands, and the door always takes shots like a blue one.
bool IsLockColor(DoorColor color) {
  switch (color) {
  case DoorColor::PowerBomb:
  case DoorColor::Bomb:
  case DoorColor::Missile:
  case DoorColor::Charge:
  case DoorColor::Super:
  case DoorColor::Wavebuster:
  case DoorColor::IceSpreader:
  case DoorColor::Flamethrower:
    return true;
  default:
    return false;
  }
}

// The color the door shows now.
DoorColor CurrentColor(const DoorStyle& style) {
  if (style.blastShield && IsBlastShieldDestroyed(style.mrea, style.dock)) {
    return DoorColor::Blue;
  }
  return IsLockColor(style.color) && !style.blastShield ? DoorColor::Blue : style.color;
}

// The color whose vulnerability the door takes.
DoorColor VulnerabilityColor(DoorColor shown) {
  return IsLockColor(shown) ? DoorColor::Blue : shown;
}

uint32_t ShieldModel(DoorColor color, bool vertical) {
  const DoorColorStyle& style = GetDoorColorStyle(color);
  return vertical ? style.verticalShieldModel : style.shieldModel;
}

struct State {
  bool active = false;
  bool lockOn = false;
  bool mainPlazaDoor = false;
  bool backwardsUpperMines = false;
  bool backwardsFrigate = false;
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
                  TUniqueId jingle, TUniqueId gibs, TUniqueId door,
                  const TUniqueId (&forces)[2], const TUniqueId (&doorShields)[2], bool vertical)
  : CEntity(uid, info, true, rstl::string_l("Randomizer Blast Shield"))
  , mMrea(mrea)
  , mDock(dock)
  , mPosition(position)
  , mActor(actor)
  , mTrigger(trigger)
  , mScan(scan)
  , mJingle(jingle)
  , mGibs(gibs)
  , mDoor(door)
  , mVertical(vertical) {
    mForces[0] = forces[0];
    mForces[1] = forces[1];
    mDoorShields[0] = doorShields[0];
    mDoorShields[1] = doorShields[1];
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
    // As a CActor: TCastToPtr isn't generated for CScriptDamageableTrigger.
    if (CActor* trigger = TCastToPtr< CActor >(mgr.ObjectById(mTrigger))) {
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
    TurnDoorBlue(mgr);
  }

  // The door now opens like a blue door. Its triggers keep their old textures until the room
  // is loaded again.
  void TurnDoorBlue(CStateManager& mgr) {
    const CDamageVulnerability blue = MakeVulnerability(DoorColor::Blue);
    for (const TUniqueId force : mForces) {
      if (CScriptDamageableTrigger* trigger =
              dynamic_cast< CScriptDamageableTrigger* >(mgr.ObjectById(force))) {
        trigger->SetDamageVulnerability(blue);
      }
    }
    const uint32_t model = ShieldModel(DoorColor::Blue, mVertical);
    for (const TUniqueId id : mDoorShields) {
      if (CActor* shield = TCastToPtr< CActor >(mgr.ObjectById(id))) {
        if (shield->GetModelData() != nullptr) {
          shield->SetModelData(CModelData(CStaticRes(model, shield->GetModelData()->GetScale())));
        }
      }
    }
  }

  void Break(CStateManager& mgr) {
    Remove(mgr);
    CSfxManager::AddEmitter(kBreakSfx, mPosition, CVector3f::Zero(), true, false,
                            CSfxManager::kMedPriority, GetAreaId().Value());
    if (CEntity* jingle = mgr.ObjectById(mJingle)) {
      jingle->AcceptScriptMsg(kSM_Play, GetUniqueId(), mgr);
    }
    if (CEntity* gibs = mgr.ObjectById(mGibs)) {
      gibs->AcceptScriptMsg(kSM_Activate, GetUniqueId(), mgr);
    }
    mgr.CameraManager()->AddCameraShaker(CCameraShakeData::HardBothAxesShake(0.5f, 0.2f), true);
    // A door without power keeps its triggers off; it opens normally once it has power.
    for (const TUniqueId force : mForces) {
      if (CActor* trigger = TCastToPtr< CActor >(mgr.ObjectById(force))) {
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
  TUniqueId mGibs;
  TUniqueId mDoor;
  TUniqueId mForces[2];
  TUniqueId mDoorShields[2];
  bool mVertical;
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
  const CActor* shieldActor = nullptr;
  for (const uint32_t id : door->shields) {
    if (shieldActor == nullptr) {
      shieldActor = TCastToConstPtr< CActor >(mgr.GetObjectById(FindLoadedObject(mgr, area, id)));
    }
  }
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
      // No X-ray or thermal models: CActor takes 0 for none.
      rstl::pair< CAssetId, CAssetId >(0, 0), rstl::pair< CAssetId, CAssetId >(0, 0), visor, true,
      false,
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

  // The gibs, where the shield stands and turned the same way. Inactive until the shield breaks,
  // then played until its particles are done, and removed after that.
  CScriptEffect* gibs = rs_new CScriptEffect(
      mgr.AllocateUniqueId(), rstl::string_l("Randomizer Blast Shield Gibs"), info, xf,
      placement->scale, kGibsPart, kInvalidAssetId, false, true, true, false, false, 0.f, 0.f,
      0.f, 0.f, false, 0.f, 0.f, 0.f, true, true, true, lights, true);
  mgr.AddObject(gibs);

  const TUniqueId forces[2] = {FindLoadedObject(mgr, area, door->forces[0]),
                               FindLoadedObject(mgr, area, door->forces[1])};
  const TUniqueId doorShields[2] = {FindLoadedObject(mgr, area, door->shields[0]),
                                    FindLoadedObject(mgr, area, door->shields[1])};
  mgr.AddObject(rs_new CDoorLockShield(mgr.AllocateUniqueId(), info, mrea, dock,
                                       placement->position, actor->GetUniqueId(),
                                       trigger->GetUniqueId(), scan, jingle->GetUniqueId(),
                                       gibs->GetUniqueId(),
                                       FindLoadedObject(mgr, area, door->door), forces,
                                       doorShields, door->vertical));
}

// Main Plaza Vault Door, after Randovania's "Main Plaza Vault Door". Main Plaza's side of the door
// to Plaza Access is a bare door that only opens from the other side. It gets the blue door kit
// Plaza Access's side has (shield, unlock trigger, open trigger, close timer), mirrored onto the
// ledge and run here instead of with script connections.
constexpr uint32_t kMainPlazaMrea = 0xD5CDB809;
constexpr uint32_t kMainPlazaVaultDoor = 0x00020060;
constexpr uint32_t kChozoWorld = 0x83F6FF6F;
// Scan points the door makes untrue: the door's "one way door", and the Energy Tank ledge's
// "cannot be reached from this room", which also covers the pickup's own scan.
constexpr uint32_t kMainPlazaScanPoints[] = {0x000202F4, 0x000202B6};
// The Energy Tank ledge's hardcoded "Energy Tank" scan, shown up close in place of the pickup's
// own. Left out of every randomized game.
constexpr uint32_t kMainPlazaEnergyTankScan = 0x000202B7;
// Plaza Access's kit, relative to its door, with X mirrored.
const CVector3f kVaultShieldOffset(-0.4059f, 0.f, 1.8018f);
const CVector3f kVaultUnlockOffset(-0.2187f, 0.f, 1.9996f);
const CVector3f kVaultUnlockExtent(0.25f, 4.5f, 4.f);
const CVector3f kVaultOpenOffset(-5.f, 0.f, 1.9996f);
const CVector3f kVaultOpenExtent(20.f, 10.f, 4.f);
constexpr float kVaultCloseDelay = 0.25f;

class CMainPlazaDoor : public CEntity {
public:
  CMainPlazaDoor(TUniqueId uid, const CEntityInfo& info, TUniqueId door, TUniqueId shield,
                 TUniqueId unlock, const CAABox& openBox)
  : CEntity(uid, info, true, rstl::string_l("Randomizer Main Plaza Door"))
  , mDoor(door)
  , mShield(shield)
  , mUnlock(unlock)
  , mOpenBox(openBox) {}

#ifndef HAS_TYPES_MATCH
  void Accept(IVisitor&) override {}
#endif

  void Think(float dt, CStateManager& mgr) override {
    CScriptDoor* door = TCastToPtr< CScriptDoor >(mgr.ObjectById(mDoor));
    if (door == nullptr) {
      return;
    }
    // As a CActor: TCastToPtr isn't generated for CScriptDamageableTrigger.
    CActor* unlock = TCastToPtr< CActor >(mgr.ObjectById(mUnlock));
    if (!mUnlocked && unlock != nullptr && unlock->GetActive() &&
        unlock->HealthInfo(mgr)->GetHP() <= 0.f) {
      mUnlocked = true;
      door->AcceptScriptMsg(kSM_SetToZero, GetUniqueId(), mgr);
    }
    // Opened from Plaza Access, the door closes from there too.
    SetShieldUp(!mUnlocked && !door->IsOpen(), mgr);
    if (!mUnlocked) {
      return;
    }
    if (PlayerInside(mgr)) {
      door->AcceptScriptMsg(kSM_Open, GetUniqueId(), mgr);
      mCloseTimer = kVaultCloseDelay;
    } else if (mCloseTimer > 0.f) {
      mCloseTimer -= dt;
      if (mCloseTimer <= 0.f) {
        door->AcceptScriptMsg(kSM_Close, GetUniqueId(), mgr);
        mUnlocked = false;
      }
    }
  }

private:
  bool PlayerInside(const CStateManager& mgr) const {
    const CPlayer* player = mgr.GetPlayer();
    if (player == nullptr) {
      return false;
    }
    const rstl::optional_object< CAABox > bounds = player->GetTouchBounds();
    return bounds.valid() && bounds->DoBoundsOverlap(mOpenBox);
  }

  // Activating the unlock trigger again also restores its health.
  void SetShieldUp(bool up, CStateManager& mgr) {
    if (up == mShieldUp) {
      return;
    }
    mShieldUp = up;
    for (const TUniqueId id : {mShield, mUnlock}) {
      if (CEntity* entity = mgr.ObjectById(id)) {
        entity->AcceptScriptMsg(up ? kSM_Activate : kSM_Deactivate, GetUniqueId(), mgr);
      }
    }
  }

  TUniqueId mDoor;
  TUniqueId mShield;
  TUniqueId mUnlock;
  CAABox mOpenBox;
  bool mShieldUp = true;
  bool mUnlocked = false;
  float mCloseTimer = 0.f;
};

// The damageable trigger face toward `dir`, as ScriptLoader turns a room's face flags into
// world ones.
uint FaceFlag(const CVector3f& dir) {
  const float x = std::fabs(dir.GetX());
  const float y = std::fabs(dir.GetY());
  const float z = std::fabs(dir.GetZ());
  if (x >= y && x >= z) {
    return dir.GetX() < 0.f ? 4 : 8;
  }
  if (y >= z) {
    return dir.GetY() < 0.f ? 2 : 1;
  }
  return dir.GetZ() < 0.f ? 0x20 : 0x10;
}

void SpawnMainPlazaDoor(CStateManager& mgr, TAreaId area) {
  const TUniqueId doorId = FindLoadedObject(mgr, area, kMainPlazaVaultDoor);
  const CActor* door = TCastToConstPtr< CActor >(mgr.GetObjectById(doorId));
  const uint32_t shieldModel = ShieldModel(DoorColor::Blue, false);
  if (door == nullptr || !IsAvailable(shieldModel)) {
    Log.warn("Can't put a blue door on Main Plaza's Vault door");
    return;
  }
  const CVector3f origin = door->GetTransform().GetTranslation();
  const CEntityInfo info(area, CEntity::NullConnectionList, kInvalidEditorId);
  const CVisorParameters visor(0xF, false, true);

  const CTransform4f xf =
      EditorTransform(CVector3f(0.f, 0.f, 180.f), origin + kVaultShieldOffset);
  const CModelData model(CStaticRes(shieldModel, CVector3f(1.f, 1.f, 1.f)));
  CMaterialList materials;
  materials.Add(kMT_Immovable);
  const CLightParameters lights(true, 1.f, CLightParameters::kST_Zero, 1.f, 20.f, CColor::White(),
                                true, CLightParameters::kLO_NormalWorld,
                                CLightParameters::kLR_EightFrames, CVector3f::Zero(), 4, 4, false,
                                0);
  const CActorParameters actorParms(
      lights, CScannableParameters(kInvalidAssetId),
      rstl::pair< CAssetId, CAssetId >(0, 0), rstl::pair< CAssetId, CAssetId >(0, 0), visor, true,
      false, false, false, 1.f, 1.f, 1.f);
  CScriptActor* shield = rs_new CScriptActor(
      mgr.AllocateUniqueId(), rstl::string_l("Randomizer Main Plaza Door Shield"), info, xf, model,
      model.GetBounds(xf.GetRotation()), materials, 1.f, 0.f, CHealthInfo(1.f, 1.f),
      CDamageVulnerability::PassThroughVulnerability(), actorParms, true, true, 0, 1.f, false,
      false, false, false);
  mgr.AddObject(shield);

  // Facing the ledge, the way Plaza Access's faces its room.
  const CTransform4f areaRotation = mgr.GetWorld()->GetAreaAlways(area).GetTM().GetRotation();
  const DoorColorStyle& blue = GetDoorColorStyle(DoorColor::Blue);
  CScriptDamageableTrigger* unlock = rs_new CScriptDamageableTrigger(
      mgr.AllocateUniqueId(), rstl::string_l("Randomizer Main Plaza Door Unlock"), info,
      origin + kVaultUnlockOffset, kVaultUnlockExtent, CHealthInfo(1.f, 1.f),
      MakeVulnerability(DoorColor::Blue), FaceFlag(areaRotation * CVector3f::Right()),
      blue.pattern0, blue.pattern1, blue.color, CScriptDamageableTrigger::kCO_NoOrbit, true,
      visor);
  mgr.AddObject(unlock);

  const CVector3f openCenter = origin + kVaultOpenOffset;
  const CVector3f openHalf = kVaultOpenExtent * 0.5f;
  mgr.AddObject(rs_new CMainPlazaDoor(mgr.AllocateUniqueId(), info, doorId,
                                      shield->GetUniqueId(), unlock->GetUniqueId(),
                                      CAABox(openCenter - openHalf, openCenter + openHalf)));
}

// Backwards Upper Mines, after randomprime's Main Quarry barrier patch: a trigger in the doorway
// from Security Access A sets the relays the barrier's two panel scans set, which runs the game's
// own shutdown and turns the shield's layer off for good.
constexpr uint32_t kMainQuarryMrea = 0x643D038F;
constexpr uint32_t kMainQuarryForceShield = 0x100201DA;
constexpr uint32_t kMainQuarryScanRelays[] = {0x10020261, 0x10020263}; // Relay One Shot In
const CVector3f kMainQuarryTriggerCenter(82.412056f, 9.354454f, 2.807631f);
const CVector3f kMainQuarryTriggerExtent(10.f, 5.f, 7.f);

void SpawnMainQuarryTrigger(CStateManager& mgr, TAreaId area) {
  // Once the shield is down, its layer no longer loads.
  if (FindLoadedObject(mgr, area, kMainQuarryForceShield) == kInvalidUniqueId) {
    return;
  }
  rstl::vector< SConnection > conns;
  for (const uint32_t relay : kMainQuarryScanRelays) {
    conns.push_back(SConnection(kSS_Entered, kSM_SetToZero,
                                TEditorId(merged::ToLoadedEditorId(area, relay))));
  }
  const CVector3f half = kMainQuarryTriggerExtent * 0.5f;
  mgr.AddObject(rs_new CScriptTrigger(
      mgr.AllocateUniqueId(), rstl::string_l("Randomizer Main Quarry Barrier Trigger"),
      CEntityInfo(area, conns, kInvalidEditorId), kMainQuarryTriggerCenter,
      CAABox(-half, half), CDamageInfo(), CVector3f::Zero(), kTFL_DetectPlayer, true, true,
      false));
}

// Backwards Frigate, after randomprime's Main Ventilation Shaft Section B patch: the doors by the
// conduit only unlock when the conduit is hit from the Section C side. A trigger on the Section A
// side sets both relays the conduit's hit powers the doors with and records the room as solved.
constexpr uint32_t kMainVentShaftBMrea = 0xAFD4E038;
constexpr uint32_t kMainVentShaftBConduitRelay = 0x0015006B; // Relay - [IN] Turn On Door
constexpr uint32_t kMainVentShaftBInitialRelay = 0x0015006F; // Relay - Turn On Door Initially
constexpr uint32_t kMainVentShaftBStaySolved = 0x0015006E;   // Memory Relay - Stay Solved
const CVector3f kMainVentShaftBTriggerCenter(31.25f, 462.f, -60.5f);
const CVector3f kMainVentShaftBTriggerExtent(10.f, 20.f, 14.f);

void SpawnMainVentShaftBTrigger(CStateManager& mgr, TAreaId area) {
  rstl::vector< SConnection > conns;
  const TEditorId relay(merged::ToLoadedEditorId(area, kMainVentShaftBConduitRelay));
  const TEditorId initial(merged::ToLoadedEditorId(area, kMainVentShaftBInitialRelay));
  const TEditorId staySolved(merged::ToLoadedEditorId(area, kMainVentShaftBStaySolved));
  conns.push_back(SConnection(kSS_Entered, kSM_SetToZero, relay));
  conns.push_back(SConnection(kSS_Entered, kSM_SetToZero, initial));
  conns.push_back(SConnection(kSS_Entered, kSM_Activate, staySolved));
  const CVector3f half = kMainVentShaftBTriggerExtent * 0.5f;
  mgr.AddObject(rs_new CScriptTrigger(
      mgr.AllocateUniqueId(), rstl::string_l("Randomizer Main Vent Shaft B Door Trigger"),
      CEntityInfo(area, conns, kInvalidEditorId), kMainVentShaftBTriggerCenter,
      CAABox(-half, half), CDamageInfo(), CVector3f::Zero(), kTFL_DetectPlayer, true, true,
      false));
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
  State& s = S();
  for (size_t color = 0; color < s.vulnerabilities.size(); ++color) {
    uint8_t bytes[kVulnerabilitySize];
    WriteDoorVulnerability(static_cast< DoorColor >(color), bytes);
    std::copy(std::begin(bytes), std::end(bytes), s.vulnerabilities[color].begin());
  }
  s.mainPlazaDoor = seed.settings.is_object() && seed.settings.value("main_plaza_door", false);
  s.backwardsUpperMines =
      seed.settings.is_object() && seed.settings.value("backwards_upper_mines", false);
  s.backwardsFrigate = seed.settings.is_object() && seed.settings.value("backwards_frigate", false);
  if (!seed.doorLocksRandomized) {
    return;
  }
  s.active = true;
  s.lockOn = seed.blastShieldLockOn;
  for (const DoorLock& lock : seed.doorLocks) {
    const DoorLocationInfo* door = FindDoorLocation(lock.area, lock.dock);
    DoorColor color;
    if (door == nullptr || !ParseDoorColor(lock.shield, color)) {
      Log.warn("Can't restyle {} as a {} door", lock.name, lock.shield);
      continue;
    }
    const DoorStyle style{color, door->vertical, lock.area, lock.dock, !lock.blastShield.empty()};
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
  s.mainPlazaDoor = false;
  s.backwardsUpperMines = false;
  s.backwardsFrigate = false;
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
  const uint32_t shieldModel = ShieldModel(CurrentColor(*style), style->vertical);
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

bool RemoveSeedScanPoint(unsigned int worldId, unsigned int editorId) {
  if (GetActiveSeed() == nullptr) {
    return false;
  }
  editorId = merged::ToSourceEditorId(editorId, worldId) & 0x3FFFFFF;
  if (worldId != kChozoWorld) {
    return false;
  }
  return editorId == kMainPlazaEnergyTankScan ||
         (S().mainPlazaDoor && std::find(std::begin(kMainPlazaScanPoints),
                                         std::end(kMainPlazaScanPoints),
                                         editorId) != std::end(kMainPlazaScanPoints));
}

bool GetDoorForceOverride(unsigned int worldId, unsigned int editorId, DoorForceOverride& out) {
  const DoorStyle* style = FindStyle(S().forces, worldId, editorId);
  if (style == nullptr) {
    return false;
  }
  const DoorColor current = CurrentColor(*style);
  const DoorColorStyle& color = GetDoorColorStyle(current);
  out.vulnerability =
      S().vulnerabilities[static_cast< size_t >(VulnerabilityColor(current))].data();
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
  if (mgr.GetWorld() == nullptr || areaIndex < 0) {
    return;
  }
  const TAreaId area(areaIndex);
  const uint32_t mrea = mgr.GetWorld()->GetAreaAlways(area).GetAreaAssetId();
  if (s.mainPlazaDoor && mrea == kMainPlazaMrea) {
    SpawnMainPlazaDoor(mgr, area);
  }
  if (s.backwardsUpperMines && mrea == kMainQuarryMrea) {
    SpawnMainQuarryTrigger(mgr, area);
  }
  if (s.backwardsFrigate && mrea == kMainVentShaftBMrea) {
    SpawnMainVentShaftBTrigger(mgr, area);
  }
  if (!s.active) {
    return;
  }
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
  type = MapType(GetDoorColorStyle(CurrentColor(it->second)).map, type);
  return true;
}

} // namespace metaforce::randomizer

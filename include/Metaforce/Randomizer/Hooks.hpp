#pragma once

// Entry points called from game code under TARGET_PC. Every hook is a no-op unless the game
// in progress was started with a randomizer seed.

#include <cstddef>

class CStateManager;

namespace metaforce::randomizer {

struct PickupOverride {
  int itemType;
  int capacity;
  int amount;
  // Serialized Pickup script object to take the model from, or null to keep the original.
  const unsigned char* modelData;
  size_t modelSize;
};

// CWorld load: where dock `dock` of an area now leads, when the room randomizer moved it. Areas
// are MREA asset ids.
bool GetDockOverride(unsigned int worldId, unsigned int areaAssetId, int dock,
                     unsigned int& targetAreaAssetId, int& targetDock);

// CScriptDoor::GetDoorOpenCondition: true when the door's dock (by TAreaId and dock number)
// leads into another world. The door then stays shut; walking up to it takes the player there.
bool OnCrossWorldDoorOpen(CStateManager& mgr, int area, int dock);

// CStateManager::Update, after actors think: takes the player through cross-world doors.
void UpdateCrossWorldDoors(CStateManager& mgr, float dt);

// CMFGame, for every input on port 0 while gameplay runs: watches for the shortcut chords, held in
// any order. R + Z + D-pad Right opens the save screen (Save Anywhere); R + Z + D-pad Left leaves
// the game and reloads the last save, like choosing Continue after dying. True when a shortcut
// was taken and the input should go no further.
bool OnShortcutInput(bool r, bool z, bool dpadLeft, bool dpadRight);

// True while R is held and a shortcut is on, so that Z doesn't open the map before the D-pad
// direction is pressed.
bool ShortcutsHoldMap(bool r);

// CMFGameLoader construction: picks the seed for the game being loaded, and moves a new game
// to the seed's starting location.
void OnGameLoad();

// End of CStateManager::InitializeState: gives a new game its starting items.
void OnWorldInitialized(CStateManager& mgr);

// CAutoMapper: whether the map screen offers to warp to the seed's starting location in place of
// the world map, and the warp itself. The game is left and reloaded at the start, keeping the
// player's progress.
bool CanWarpToStart();
void WarpToStart();

// ScriptLoader::LoadPickup. editorId is the object's editor id without layer bits.
bool GetPickupOverride(unsigned int worldId, unsigned int editorId, PickupOverride& out);

// Model-space center of a pickup CMDL's bounds. False for models without known bounds.
bool GetPickupModelCenter(unsigned int cmdl, float out[3]);

// CScriptHUDMemo SetToZero: replacement text for a randomized pickup's memo, or null.
const wchar_t* GetHudMemoOverride(unsigned int worldId, unsigned int editorId);

// Load-time edits to a room's script objects, standing in for what randomprime patches into the
// game files. Values are EScriptObjectState / EScriptObjectMessage numbers and editor ids
// without layer bits.
struct ScriptConnection {
  int state;
  int message;
  unsigned int target;
};

struct ScriptObjectPatch {
  int active = -1; // -1 keeps the object's own value
  bool clearConnections = false;
  unsigned int removeTarget = 0xFFFFFFFF; // drop connections to this object
  const ScriptConnection* add = nullptr;
  int addCount = 0;
};

// CStateManager::LoadScriptObject, before the object is built.
bool GetScriptObjectPatch(unsigned int worldId, unsigned int editorId, ScriptObjectPatch& out);

struct ScriptTimerSpawn {
  unsigned int editorId;
  float startTime;
  bool autoStart;
  const ScriptConnection* connections;
  int connectionCount;
};

// CStateManager::LoadScriptObjects, after a room's objects are loaded: timers to add to it.
int GetScriptTimerSpawns(unsigned int worldId, unsigned int areaAssetId,
                         const ScriptTimerSpawn** out);

// CScriptSpawnPoint Reset: false keeps the player's inventory instead of replacing it with the
// spawn point's. The randomizer gives the starting items itself, and the vanilla resets (like the
// one that takes Samus's upgrades after the frigate) would take them away again.
bool AllowSpawnPointInventoryReset();

// CHintOptions::Update and CAutoMapper: false turns the hint system off. Its hints follow the
// vanilla item route, which means nothing in a randomized game, and the Z prompt can softlock.
bool AllowHintSystem();

// DockPortals::OnPlayerCrossedDock, whenever the player walks through dock `dock` of `area`:
// marks the doors on both sides of it as gone through.
void OnPlayerCrossedDock(CStateManager& mgr, int area, int dock);

// CMapWorld: whether the player ever went through the door with this editor id of `worldId` (the
// region it was read from) in this game. The map draws those doors green.
bool IsDoorTraversed(unsigned int worldId, unsigned int editorId);

// MapLayout: whether the player ever went through dock `dock` of the area with this MREA, either
// way, in this game, and how many docks they've gone through, which grows whenever one is added.
bool IsDockTraversed(unsigned int areaAssetId, int dock);
int GetTraversedDockCount();

// Door lock randomizer (see DoorLocks.hpp). Editor ids are without layer bits.

// ScriptLoader::LoadActor: the shield model of a restyled door's shield actor.
bool GetDoorShieldModel(unsigned int worldId, unsigned int editorId, unsigned int& model);

// ScriptLoader::LoadActor and LoadPointOfInterest: true to leave out one of the game's own
// missile blast shields (by model) or its scan point (by SCAN), which the randomizer replaces.
bool RemoveBlastShieldActor(unsigned int model);
bool RemoveBlastShieldScan(unsigned int scan);

// ScriptLoader::LoadDamageableTrigger: a restyled door's damageable trigger. The vulnerability
// is a serialized DamageVulnerability property. The textures are only to be used when
// texturesChanged is set (they could all be loaded).
struct DoorForceOverride {
  const unsigned char* vulnerability;
  unsigned int vulnerabilitySize;
  bool texturesChanged;
  unsigned int pattern0;
  unsigned int pattern1;
  unsigned int color;
};
bool GetDoorForceOverride(unsigned int worldId, unsigned int editorId, DoorForceOverride& out);

// CStateManager::LoadScriptObjects, after a room's objects are loaded: puts up its blast shields.
void SpawnDoorLocks(CStateManager& mgr, int area);

// CMapWorld: `type` (a CMappableObject type) becomes the map icon of a restyled door. Takes the
// region and editor id the door was read from, like IsDoorTraversed.
bool GetDoorMapType(unsigned int worldId, unsigned int editorId, int& type);

// SGameFileSlot::InitializeFromGameState: the game is being saved, so the blast shields broken
// since the last save stay broken.
void OnGameSaved();

// CResLoader: assets the randomizer adds to the game (door lock styles and randomprime's pickup
// models), served as if they were in a pak. GetCustomAssetType is the asset's FourCC, 0 for ids
// that aren't custom assets. GetCustomAsset builds the asset on first use and keeps it; false
// when it can't be built (a texture file or the model it's built from is missing).
unsigned int GetCustomAssetType(unsigned int id);
bool GetCustomAsset(unsigned int id, const unsigned char*& data, unsigned int& size);

// CScriptPickup::Touch, after the item was given.
void OnPickupCollected(CStateManager& mgr, int itemType);

// CScriptPickup::Touch: true to take the "disable input" flag off the player hints the pickup's
// acquisition sequence turns on. With the message box skipped, those hints only drop the
// player's held inputs for a moment.
bool StripPickupInputLocks();

// CScriptSpecialFunction layer controller. False blocks the layer change.
bool AllowLayerChange(unsigned int areaSaveId, unsigned int layer);

// CStateManager::SendScriptMsg. False drops the message.
bool AllowScriptMsg(unsigned int worldId, unsigned int senderEditorId,
                    unsigned int targetEditorId);

} // namespace metaforce::randomizer

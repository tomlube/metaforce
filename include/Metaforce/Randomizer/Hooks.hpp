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

// CMFGameLoader construction: picks the seed for the game being loaded, and moves a new game
// to the seed's starting location.
void OnGameLoad();

// End of CStateManager::InitializeState: gives a new game its starting items.
void OnWorldInitialized(CStateManager& mgr);

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

// CScriptPickup::Touch, after the item was given.
void OnPickupCollected(CStateManager& mgr, int itemType);

// CScriptSpecialFunction layer controller. False blocks the layer change.
bool AllowLayerChange(unsigned int areaSaveId, unsigned int layer);

// CStateManager::SendScriptMsg. False drops the message.
bool AllowScriptMsg(unsigned int worldId, unsigned int senderEditorId,
                    unsigned int targetEditorId);

} // namespace metaforce::randomizer

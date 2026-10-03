#pragma once

#include <cstdint>

namespace metaforce::randomizer {

// The script objects that make up a dock's door in a room, as randomprime restyles them.
// Script object ids are editor ids without the layer bits, 0 where there are fewer.
struct DoorLocationInfo {
  uint32_t mrea;
  int dock;
  uint32_t door;
  // Damageable triggers taking the shots that open the door (the shield's surface).
  uint32_t forces[2];
  // Actors drawing the shield.
  uint32_t shields[2];
  float rotation[3]; // the door's, in degrees
  bool vertical;     // in a floor or ceiling
};

extern const DoorLocationInfo kDoorLocations[];
extern const int kDoorLocationCount;

// The door at dock `dock` of the room with this MREA, or null.
const DoorLocationInfo* FindDoorLocation(uint32_t mrea, int dock);

} // namespace metaforce::randomizer

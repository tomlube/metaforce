#include "Metaforce/Randomizer/ShareCode.hpp"

#include "Metaforce/Randomizer/Generator.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <iterator>

namespace metaforce::randomizer {
namespace {

// Bumped when the bit layout below changes.
constexpr uint32_t kFormat = 1;
constexpr int kFormatBits = 4;
constexpr size_t kMaxSeedString = 32;

// Widths of the pickup fields, wide enough for the menu's ranges.
constexpr int kShuffledBits = 4;    // 0-14 Energy Tanks
constexpr int kStartingBits = 4;    // 0-14 Energy Tanks
constexpr int kStandardAmmoBits = 8; // 0-250 Missiles
constexpr int kAmmoCountBits = 7;   // 0-100 expansions
constexpr int kAmmoAmountBits = 10; // 1-999

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

uint64_t Fnv(std::string_view str, uint64_t hash = 0xcbf29ce484222325ull) {
  for (const char c : str) {
    hash ^= static_cast< unsigned char >(c);
    hash *= 0x100000001b3ull;
  }
  // A separator, so ("ab", "c") and ("a", "bc") differ.
  hash ^= 0x1f;
  hash *= 0x100000001b3ull;
  return hash;
}

uint32_t Fold16(uint64_t hash) {
  return static_cast< uint32_t >((hash ^ (hash >> 16) ^ (hash >> 32) ^ (hash >> 48)) & 0xffff);
}

int BitsFor(size_t maxValue) { return static_cast< int >(std::bit_width(maxValue)); }

class BitWriter {
public:
  void Write(uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      if (mBits % 8 == 0) {
        mBytes.push_back(0);
      }
      if ((value >> i) & 1) {
        mBytes.back() |= 0x80 >> (mBits % 8);
      }
      ++mBits;
    }
  }
  // Values the field can't hold are clamped; the settings checksum catches it.
  void WriteClamped(int value, int count) {
    Write(static_cast< uint32_t >(std::clamp(value, 0, (1 << count) - 1)), count);
  }
  void WriteBool(bool value) { Write(value ? 1 : 0, 1); }
  const std::vector< uint8_t >& Bytes() const { return mBytes; }

private:
  std::vector< uint8_t > mBytes;
  size_t mBits = 0;
};

class BitReader {
public:
  explicit BitReader(const std::vector< uint8_t >& bytes) : mBytes(bytes) {}

  uint32_t Read(int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      value <<= 1;
      if (mBit >= mBytes.size() * 8) {
        mOverrun = true;
        continue;
      }
      value |= (mBytes[mBit / 8] >> (7 - mBit % 8)) & 1;
      ++mBit;
    }
    return value;
  }
  int ReadInt(int count) { return static_cast< int >(Read(count)); }
  bool ReadBool() { return Read(1) != 0; }
  bool Overrun() const { return mOverrun; }
  // Whether everything left is the zero padding of the last byte.
  bool AtEnd() const {
    if (mBytes.size() * 8 - mBit >= 8) {
      return false;
    }
    for (size_t bit = mBit; bit < mBytes.size() * 8; ++bit) {
      if ((mBytes[bit / 8] >> (7 - bit % 8)) & 1) {
        return false;
      }
    }
    return true;
  }

private:
  const std::vector< uint8_t >& mBytes;
  size_t mBit = 0;
  bool mOverrun = false;
};

std::string Base64Encode(const std::vector< uint8_t >& bytes) {
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (const uint8_t byte : bytes) {
    buffer = (buffer << 8) | byte;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += kBase64[(buffer >> bits) & 0x3f];
    }
  }
  if (bits > 0) {
    out += kBase64[(buffer << (6 - bits)) & 0x3f];
  }
  return out;
}

std::optional< std::vector< uint8_t > > Base64Decode(std::string_view text) {
  std::vector< uint8_t > out;
  uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    const char* found = std::char_traits< char >::find(kBase64, 64, c);
    if (found == nullptr) {
      return std::nullopt;
    }
    buffer = (buffer << 6) | static_cast< uint32_t >(found - kBase64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast< uint8_t >((buffer >> bits) & 0xff));
    }
  }
  return out;
}

// What the code's fields are counted against. Both ends have to build the same one, which the
// hash checks.
struct Schema {
  std::vector< std::string > starts;
  std::vector< std::string > regions;
  const DockTypeDistributor* doors = nullptr;
  std::vector< std::string > changeFrom;
  std::vector< std::string > changeTo;
  std::vector< std::string > tricks;
  uint32_t hash = 0;
};

Schema MakeSchema(const Database& db, const PickupDatabase& pickups) {
  Schema schema;
  for (auto& [key, name] : StartingLocations(db)) {
    schema.starts.push_back(std::move(key));
  }
  for (const auto& region : db.Regions()) {
    schema.regions.push_back(region.name);
  }
  schema.doors = db.FindDockType("door");
  if (schema.doors != nullptr) {
    for (const int weakness : schema.doors->changeFrom) {
      schema.changeFrom.push_back(db.Weaknesses()[weakness].name);
    }
    for (const int weakness : schema.doors->changeTo) {
      schema.changeTo.push_back(db.Weaknesses()[weakness].name);
    }
  }
  for (const auto& trick : db.Tricks()) {
    schema.tricks.push_back(trick.name);
  }

  uint64_t hash = Fnv(std::to_string(Settings::kVersion));
  const auto mixList = [&hash](std::string_view tag, const std::vector< std::string >& list) {
    hash = Fnv(tag, hash);
    hash = Fnv(std::to_string(list.size()), hash);
    for (const auto& item : list) {
      hash = Fnv(item, hash);
    }
  };
  mixList("starts", schema.starts);
  mixList("regions", schema.regions);
  mixList("from", schema.changeFrom);
  mixList("to", schema.changeTo);
  hash = Fnv(std::to_string(std::size(kRoomRandoNames)), hash);
  hash = Fnv(std::to_string(std::size(kDoorLockModeNames)), hash);
  hash = Fnv(std::to_string(pickups.defaultArtifactTarget), hash);
  // Fields added to the code after it shipped, so older codes are told apart.
  hash = Fnv("main_plaza_door", hash);
  hash = Fnv("bigger_thermal_conduits", hash);
  mixList("tricks", schema.tricks);
  for (const auto& def : pickups.standard) {
    hash = Fnv(def.name, hash);
    hash = Fnv(std::to_string(def.defaultShuffled) + "/" + std::to_string(def.defaultStarting) +
                   "/" + std::to_string(def.defaultAmmo),
               hash);
  }
  for (const auto& def : pickups.ammo) {
    hash = Fnv(def.name, hash);
    hash = Fnv(std::to_string(def.defaultCount) + "/" + std::to_string(def.defaultAmmo), hash);
  }
  schema.hash = Fold16(hash);
  return schema;
}

uint32_t SettingsChecksum(const Settings& settings) {
  return Fold16(Fnv(settings.ToJson().dump()));
}

void WriteSet(BitWriter& w, const std::set< std::string >& set,
              const std::vector< std::string >& universe) {
  for (const auto& name : universe) {
    w.WriteBool(set.contains(name));
  }
}

// Names outside `universe` can't be changed in the menu, so they're taken to be as they are by
// default.
std::set< std::string > ReadSet(BitReader& r, const std::vector< std::string >& universe,
                                const std::set< std::string >& defaults) {
  std::set< std::string > set;
  for (const auto& name : universe) {
    if (r.ReadBool()) {
      set.insert(name);
    }
  }
  for (const auto& name : defaults) {
    if (std::find(universe.begin(), universe.end(), name) == universe.end()) {
      set.insert(name);
    }
  }
  return set;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast< unsigned char >(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast< unsigned char >(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

bool StartsWithPrefix(std::string_view text) {
  if (text.size() < kShareCodePrefix.size()) {
    return false;
  }
  for (size_t i = 0; i < kShareCodePrefix.size(); ++i) {
    if (std::toupper(static_cast< unsigned char >(text[i])) != kShareCodePrefix[i]) {
      return false;
    }
  }
  return true;
}

} // namespace

std::string EncodeShareCode(const Database& db, const PickupDatabase& pickups,
                            const Settings& settings) {
  const Schema schema = MakeSchema(db, pickups);
  BitWriter w;
  w.Write(kFormat, kFormatBits);
  w.Write(schema.hash, 16);
  w.Write(SettingsChecksum(settings), 16);

  const std::string_view seed =
      std::string_view(settings.seedString).substr(0, kMaxSeedString);
  w.Write(static_cast< uint32_t >(seed.size()), BitsFor(kMaxSeedString));
  for (const char c : seed) {
    w.Write(static_cast< unsigned char >(c), 8);
  }

  w.WriteClamped(settings.trickLevel, BitsFor(std::size(kTrickLevelNames) - 1));
  // One bit per trick when it's at the base level.
  for (const auto& trick : schema.tricks) {
    const int level = settings.TrickLevel(trick);
    w.WriteBool(level != settings.trickLevel);
    if (level != settings.trickLevel) {
      w.WriteClamped(level, BitsFor(std::size(kTrickLevelNames) - 1));
    }
  }
  w.WriteClamped(settings.damageStrictness, BitsFor(std::size(kDamageStrictnessNames) - 1));
  w.WriteClamped(settings.artifactTarget, BitsFor(12));
  w.WriteClamped(settings.artifactHints, BitsFor(std::size(kArtifactHintNames) - 1));
  w.WriteClamped(settings.phazonSuitHints, BitsFor(std::size(kArtifactHintNames) - 1));

  // 0 is the database's default start, 1 a random one, then the starting locations in order.
  uint32_t start = 0;
  if (settings.startingLocation == kRandomStartingLocation) {
    start = 1;
  } else if (!settings.startingLocation.empty()) {
    const auto it =
        std::find(schema.starts.begin(), schema.starts.end(), settings.startingLocation);
    if (it != schema.starts.end()) {
      start = 2 + static_cast< uint32_t >(it - schema.starts.begin());
    }
  }
  w.Write(start, BitsFor(schema.starts.size() + 1));

  w.WriteClamped(settings.roomRando, BitsFor(std::size(kRoomRandoNames) - 1));
  WriteSet(w, settings.roomRandoExcludedRegions, schema.regions);
  w.WriteBool(settings.roomRandoMorphBallDoors);

  w.WriteClamped(settings.doorLockMode, BitsFor(std::size(kDoorLockModeNames) - 1));
  WriteSet(w, settings.doorLockChangeFrom, schema.changeFrom);
  WriteSet(w, settings.doorLockChangeTo, schema.changeTo);
  w.WriteBool(settings.unlockSaveStationDoors);
  w.WriteBool(settings.blastShieldLockOn);
  w.WriteBool(settings.logicDisabled);
  w.WriteBool(settings.mainPlazaDoor);
  w.WriteBool(settings.biggerThermalConduits);

  // Pickups are one bit when they're as the database has them, which most are.
  for (const auto& def : pickups.standard) {
    StandardPickupState state{def.defaultShuffled, def.defaultStarting, def.defaultAmmo};
    if (const auto it = settings.standard.find(def.name); it != settings.standard.end()) {
      state = it->second;
    }
    const bool changed = state.shuffled != def.defaultShuffled ||
                         state.starting != def.defaultStarting || state.ammo != def.defaultAmmo;
    w.WriteBool(changed);
    if (changed) {
      w.WriteClamped(state.shuffled, kShuffledBits);
      w.WriteClamped(state.starting, kStartingBits);
      w.WriteClamped(state.ammo, kStandardAmmoBits);
    }
  }
  for (const auto& def : pickups.ammo) {
    AmmoPickupState state{def.defaultCount, def.defaultAmmo};
    if (const auto it = settings.ammo.find(def.name); it != settings.ammo.end()) {
      state = it->second;
    }
    const bool changed = state.count != def.defaultCount || state.ammo != def.defaultAmmo;
    w.WriteBool(changed);
    if (changed) {
      w.WriteClamped(state.count, kAmmoCountBits);
      w.WriteClamped(state.ammo, kAmmoAmountBits);
    }
  }

  return std::string(kShareCodePrefix) + Base64Encode(w.Bytes());
}

std::optional< DecodedShareCode > DecodeShareCode(const Database& db, const PickupDatabase& pickups,
                                                  std::string_view code, std::string& error) {
  // Codes get pasted from chat with line breaks and spaces in them.
  std::string compact;
  for (const char c : Trim(code)) {
    if (!std::isspace(static_cast< unsigned char >(c))) {
      compact += c;
    }
  }
  if (!StartsWithPrefix(compact)) {
    error = "That isn't a share code. Share codes start with PRIME-.";
    return std::nullopt;
  }
  const auto bytes = Base64Decode(std::string_view(compact).substr(kShareCodePrefix.size()));
  if (!bytes || bytes->empty()) {
    error = "The share code has characters a share code can't have. It was likely miscopied.";
    return std::nullopt;
  }

  BitReader r(*bytes);
  if (r.Read(kFormatBits) != kFormat) {
    error = "The share code was made by a different version of Metaforce.";
    return std::nullopt;
  }
  const Schema schema = MakeSchema(db, pickups);
  if (r.Read(16) != schema.hash) {
    error = "The share code was made with different randomizer data, maybe with a different "
            "version of Metaforce.";
    return std::nullopt;
  }
  const uint32_t checksum = r.Read(16);

  DecodedShareCode result;
  Settings& s = result.settings;
  const size_t seedLength = std::min< size_t >(r.Read(BitsFor(kMaxSeedString)), kMaxSeedString);
  for (size_t i = 0; i < seedLength; ++i) {
    s.seedString += static_cast< char >(r.Read(8));
  }

  s.trickLevel = std::min(r.ReadInt(BitsFor(std::size(kTrickLevelNames) - 1)),
                          static_cast< int >(std::size(kTrickLevelNames)) - 1);
  for (const auto& trick : schema.tricks) {
    if (r.ReadBool()) {
      s.SetTrickLevel(trick, r.ReadInt(BitsFor(std::size(kTrickLevelNames) - 1)));
    }
  }
  s.damageStrictness = std::min(r.ReadInt(BitsFor(std::size(kDamageStrictnessNames) - 1)),
                                static_cast< int >(std::size(kDamageStrictnessNames)) - 1);
  s.artifactTarget = std::min(r.ReadInt(BitsFor(12)), 12);
  s.artifactHints = std::min(r.ReadInt(BitsFor(std::size(kArtifactHintNames) - 1)),
                             static_cast< int >(std::size(kArtifactHintNames)) - 1);
  s.phazonSuitHints = std::min(r.ReadInt(BitsFor(std::size(kArtifactHintNames) - 1)),
                               static_cast< int >(std::size(kArtifactHintNames)) - 1);

  const uint32_t start = r.Read(BitsFor(schema.starts.size() + 1));
  if (start == 1) {
    s.startingLocation = kRandomStartingLocation;
  } else if (start >= 2 && start - 2 < schema.starts.size()) {
    s.startingLocation = schema.starts[start - 2];
  }

  s.roomRando = std::min(r.ReadInt(BitsFor(std::size(kRoomRandoNames) - 1)),
                         static_cast< int >(std::size(kRoomRandoNames)) - 1);
  s.roomRandoExcludedRegions =
      ReadSet(r, schema.regions, Settings::DefaultExcludedRegions());
  s.roomRandoMorphBallDoors = r.ReadBool();

  s.doorLockMode = std::min(r.ReadInt(BitsFor(std::size(kDoorLockModeNames) - 1)),
                            static_cast< int >(std::size(kDoorLockModeNames)) - 1);
  s.doorLockChangeFrom = ReadSet(r, schema.changeFrom, Settings::DefaultDoorLockChangeFrom());
  s.doorLockChangeTo = ReadSet(r, schema.changeTo, Settings::DefaultDoorLockChangeTo());
  s.unlockSaveStationDoors = r.ReadBool();
  s.blastShieldLockOn = r.ReadBool();
  s.logicDisabled = r.ReadBool();
  s.mainPlazaDoor = r.ReadBool();
  s.biggerThermalConduits = r.ReadBool();

  for (const auto& def : pickups.standard) {
    StandardPickupState state{def.defaultShuffled, def.defaultStarting, def.defaultAmmo};
    if (r.ReadBool()) {
      state.shuffled = r.ReadInt(kShuffledBits);
      state.starting = r.ReadInt(kStartingBits);
      state.ammo = r.ReadInt(kStandardAmmoBits);
    }
    s.standard[def.name] = state;
  }
  for (const auto& def : pickups.ammo) {
    AmmoPickupState state{def.defaultCount, def.defaultAmmo};
    if (r.ReadBool()) {
      state.count = r.ReadInt(kAmmoCountBits);
      state.ammo = r.ReadInt(kAmmoAmountBits);
    }
    s.ammo[def.name] = state;
  }

  if (r.Overrun()) {
    error = "The share code is too short. It was likely miscopied.";
    return std::nullopt;
  }
  if (!r.AtEnd()) {
    error = "The share code is too long. It was likely miscopied.";
    return std::nullopt;
  }
  result.exact = SettingsChecksum(s) == checksum;
  return result;
}

bool LooksLikeShareCode(std::string_view text) {
  text = Trim(text);
  return text.size() > kShareCodePrefix.size() && StartsWithPrefix(text);
}

} // namespace metaforce::randomizer

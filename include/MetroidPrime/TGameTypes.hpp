#ifndef _TGAMETYPES
#define _TGAMETYPES

#include "types.h"

class CInputStream;
class COutputStream;

struct TAreaId;
struct TEditorId;
struct TUniqueId;

extern const TAreaId kInvalidAreaId;
extern const TEditorId kInvalidEditorId;
extern const TUniqueId kInvalidUniqueId;

struct TAreaId {
  int value;

  TAreaId() : value(-1) {}
  TAreaId(int value) : value(value) {}
  int Value() const { return value; }

  bool operator==(const TAreaId& other) const { return value == other.value; }
  bool operator!=(const TAreaId& other) const { return value != other.value; }
};
CHECK_SIZEOF(TAreaId, 0x4)

struct TEditorId {
  uint value;

  TEditorId(uint value) : value(value) {}
  TEditorId(CInputStream& in);
  // TODO
  uint Value() const { return value & 0x3FFFFFF; }
  uint Id() const { return value & 0xffff; }
  int AreaNum() const { return (value >> 16) & 0x3ff; }

  void PutTo(COutputStream&) const;

  bool operator==(const TEditorId& other) const { return Value() == other.Value(); }
  bool operator!=(const TEditorId& other) const { return Value() != other.Value(); }
  bool operator<(const TEditorId& other) const { return Value() < other.Value(); }
};
CHECK_SIZEOF(TEditorId, 0x4)

#if defined(TARGET_PC)
// Randomized doors can put two large rooms next to each other, and both stay loaded while the
// player walks between them. The port doubles the object limit, trading a bit of the version
// counter that catches stale ids.
#define kUniqueIdIndexBits 11
#else
#define kUniqueIdIndexBits 10
#endif
#define kMaxObjects (1 << kUniqueIdIndexBits)
#define kUniqueIdVersionMask ((1 << (16 - kUniqueIdIndexBits)) - 1)

struct TUniqueId {
  ushort value;
  TUniqueId() {}
  TUniqueId(const ushort version, const ushort id)
  : value(id | (version << kUniqueIdIndexBits)) {}

  ushort Value() const { return value & (kMaxObjects - 1); }
  ushort Version() const { return (value >> kUniqueIdIndexBits) & kUniqueIdVersionMask; }

  bool operator==(const TUniqueId& other) const { return value == other.value; }
  bool operator!=(const TUniqueId& other) const { return value != other.value; }
  bool operator<(const TUniqueId& other) const { return value < other.value; }
  operator bool() const { return *this != kInvalidUniqueId; }

private:
};
CHECK_SIZEOF(TUniqueId, 0x2)

// struct TGameScriptId {
//   TEditorId editorId;
//   bool b;
// };
// CHECK_SIZEOF(TGameScriptId, 0x8)

typedef ushort TSfxId;
static TSfxId InvalidSfxId = 0xFFFFu;

#define ALIGN_UP(x, a) (((x) + (a - 1)) & ~(a - 1))

#endif // _TGAMETYPES

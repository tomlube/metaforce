#include "Metaforce/MapLayout.hpp"

#include "Metaforce/DockPortals.hpp"
#include "Metaforce/MergedWorld.hpp"
#include "Metaforce/Randomizer/Logic.hpp"
#include "Metaforce/Randomizer/Randomizer.hpp"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CResLoader.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapArea.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CWorld.hpp"

#include <borealis/log.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace metaforce::maplayout {
namespace {

constexpr borealis::Log Log{"MapLayout"};

constexpr float kPi = 3.14159265f;

// Set METAFORCE_NO_MAP_LAYOUT=1 to keep the vanilla map with the room randomizer.
const bool sDisabled = std::getenv("METAFORCE_NO_MAP_LAYOUT") != nullptr;
// Every room is joined to the layout by one of its doors, even where that lays it over other rooms:
// the door with the least overlap is taken. Set METAFORCE_MAP_LAYOUT_ISLANDS=1 to put rooms that
// don't fit by any door into free space instead, as islands.
const bool sForceDoors = std::getenv("METAFORCE_MAP_LAYOUT_ISLANDS") == nullptr;
// Set METAFORCE_MAP_LAYOUT_LOG=1 to log how every room and moved door was laid out.
const bool sLogEnabled = std::getenv("METAFORCE_MAP_LAYOUT_LOG") != nullptr;

// Size of the grid cells rooms are rasterized into, in X and Y.
constexpr float kCellSize = 2.5f;
// How far two rooms may overlap in Z and still not count as overlapping.
constexpr float kZTolerance = 1.f;
// How many of a group's cells may land on other rooms before it no longer fits: at least
// kMinOverlaps, and one in kOverlapsPerCell for big groups. A little overlap at the edges of two
// rooms is a better map than a door whose two sides are drawn apart.
constexpr int kMinOverlaps = 3;
constexpr int kOverlapsPerCell = 20;
// Layouts tried, each grown from a different first group. The one with the fewest islands is kept.
constexpr int kAttempts = 12;
// Islands go into the nearest free space, searched for in rings this far apart around where the
// group would have gone.
constexpr float kIslandRingStep = 10.f;
constexpr int kIslandRings = 60;
// How strongly overlaps between rooms are shaded on the map, faces and edges.
constexpr float kOverlapAlpha = 0.3f;
constexpr float kOverlapEdgeAlpha = 0.85f;
// How close a door's two sides have to be on the map to count as meeting.
constexpr float kMeetDistance = 0.5f;
// Rooms placed through a door touch along it, and their map geometry often reaches a little past
// the door's plane. Overlaps this close to the door they're joined by don't count.
constexpr float kDoorClearance = 8.f;
// Repair: passes over the islands, how many groups in the way of an island may be moved for it,
// and how far a moved group may go looking for free space, in island rings.
constexpr int kRepairPasses = 3;
constexpr int kMaxBlockers = 3;
constexpr int kRepairRings = 30;
// Grid entries of groups being tried somewhere new, as opposed to where they are.
constexpr int kTempStep = 0x7fffffff;
// The yaw of a pose not worked out yet.
constexpr float kNoPose = 1e30f;

// A turn about Z, then a move.
struct Pose {
  float yaw;
  float c;
  float s;
  CVector3f t;

  Pose() : yaw(0.f), c(1.f), s(0.f), t(CVector3f::Zero()) {}
  Pose(float yaw_, const CVector3f& t_) : yaw(yaw_), c(std::cos(yaw_)), s(std::sin(yaw_)), t(t_) {}

  float X(float x, float y) const { return c * x - s * y + t.GetX(); }
  float Y(float x, float y) const { return s * x + c * y + t.GetY(); }
  CVector3f Rotate(const CVector3f& p) const {
    return CVector3f(c * p.GetX() - s * p.GetY(), s * p.GetX() + c * p.GetY(), p.GetZ());
  }
  CVector3f Apply(const CVector3f& p) const { return Rotate(p) + t; }
};

// `b`, then `a`.
Pose Compose(const Pose& a, const Pose& b) { return Pose(a.yaw + b.yaw, a.Apply(b.t)); }

Pose Inverse(const Pose& p) {
  const Pose back(-p.yaw, CVector3f::Zero());
  return Pose(-p.yaw, back.Rotate(p.t) * -1.f);
}

bool SamePose(const Pose& a, const Pose& b) {
  return (a.t - b.t).Magnitude() < kMeetDistance && std::fabs(a.c - b.c) < 0.01f &&
         std::fabs(a.s - b.s) < 0.01f;
}

CAABox TransformBounds(const CAABox& box, const Pose& pose) {
  CAABox result = CAABox::MakeMaxInvertedBox();
  for (int i = 0; i < 8; ++i) {
    result.AccumulateBounds(pose.Apply(box.GetPoint(i)));
  }
  return result;
}

float BoundsArea(const CAABox& box) {
  const CVector3f size = box.GetMaxPoint() - box.GetMinPoint();
  return size.GetX() * size.GetY();
}

// A cell of a room's map geometry: its center in X and Y, and the range of Z it covers.
struct Column {
  float x;
  float y;
  float z0;
  float z1;
};

using CellKey = std::pair< int, int >;

int CellOf(float v) { return static_cast< int >(std::floor(v / kCellSize)); }

std::int64_t PackKey(int ix, int iy) {
  return (static_cast< std::int64_t >(ix) << 32) ^ static_cast< std::uint32_t >(iy);
}

void AddToCell(std::map< CellKey, std::pair< float, float > >& cells, int ix, int iy, float z0,
               float z1) {
  auto [it, added] = cells.emplace(CellKey(ix, iy), std::make_pair(z0, z1));
  if (!added) {
    it->second.first = std::min(it->second.first, z0);
    it->second.second = std::max(it->second.second, z1);
  }
}

// Marks every cell the triangle covers in X and Y, with the part of its Z range over that cell.
void RasterizeTriangle(const CVector3f& a, const CVector3f& b, const CVector3f& c,
                       std::map< CellKey, std::pair< float, float > >& cells) {
  const CVector3f p[3] = {a, b, c};
  float minX = a.GetX(), maxX = a.GetX(), minY = a.GetY(), maxY = a.GetY();
  float minZ = a.GetZ(), maxZ = a.GetZ();
  for (int i = 1; i < 3; ++i) {
    minX = std::min(minX, p[i].GetX());
    maxX = std::max(maxX, p[i].GetX());
    minY = std::min(minY, p[i].GetY());
    maxY = std::max(maxY, p[i].GetY());
    minZ = std::min(minZ, p[i].GetZ());
    maxZ = std::max(maxZ, p[i].GetZ());
  }
  const CVector3f normal = CVector3f::Cross(b - a, c - a);
  const float normalLength = normal.Magnitude();
  // Floors, ceilings and slopes cover only their height over each cell; walls their whole height.
  const bool flat = normalLength > 0.f && std::fabs(normal.GetZ()) > 0.2f * normalLength;

  for (int ix = CellOf(minX); ix <= CellOf(maxX); ++ix) {
    for (int iy = CellOf(minY); iy <= CellOf(maxY); ++iy) {
      const float x0 = ix * kCellSize;
      const float y0 = iy * kCellSize;
      const float cornersX[4] = {x0, x0 + kCellSize, x0, x0 + kCellSize};
      const float cornersY[4] = {y0, y0, y0 + kCellSize, y0 + kCellSize};
      // Separating axes: the cell's own axes are covered by the bounds, so only the edges' normals.
      bool separated = false;
      for (int e = 0; e < 3 && !separated; ++e) {
        const CVector3f& from = p[e];
        const CVector3f& to = p[(e + 1) % 3];
        const float nx = from.GetY() - to.GetY();
        const float ny = to.GetX() - from.GetX();
        if (std::fabs(nx) + std::fabs(ny) < 1e-5f) {
          continue;
        }
        float triMin = FLT_MAX, triMax = -FLT_MAX;
        for (int i = 0; i < 3; ++i) {
          const float d = nx * p[i].GetX() + ny * p[i].GetY();
          triMin = std::min(triMin, d);
          triMax = std::max(triMax, d);
        }
        float cellMin = FLT_MAX, cellMax = -FLT_MAX;
        for (int i = 0; i < 4; ++i) {
          const float d = nx * cornersX[i] + ny * cornersY[i];
          cellMin = std::min(cellMin, d);
          cellMax = std::max(cellMax, d);
        }
        separated = triMax < cellMin || cellMax < triMin;
      }
      if (separated) {
        continue;
      }
      float z0 = minZ, z1 = maxZ;
      if (flat) {
        z0 = FLT_MAX;
        z1 = -FLT_MAX;
        for (int i = 0; i < 4; ++i) {
          const float z = a.GetZ() - (normal.GetX() * (cornersX[i] - a.GetX()) +
                                      normal.GetY() * (cornersY[i] - a.GetY())) /
                                         normal.GetZ();
          z0 = std::min(z0, z);
          z1 = std::max(z1, z);
        }
        z0 = std::clamp(z0, minZ, maxZ);
        z1 = std::clamp(z1, minZ, maxZ);
      }
      AddToCell(cells, ix, iy, z0, z1);
    }
  }
}

// The triangles of a map area, in world space. False when the room has no map.
bool ReadMapTriangles(CAssetId mapa, const CTransform4f& xf, std::vector< CVector3f >& verts,
                      std::vector< int >& tris) {
  if (mapa == kInvalidAssetId) {
    return false;
  }
  const SObjectTag tag('MAPA', mapa);
  CResLoader& loader = gpResourceFactory->GetResLoader();
  if (!loader.ResourceExists(tag)) {
    return false;
  }
  std::unique_ptr< CInputStream > in(loader.LoadNewResourceSync(tag, nullptr));
  if (!in) {
    return false;
  }
  const CMapArea area(*in, loader.ResourceSize(tag));
  const TMapVertices local = area.GetVertices();
  verts.clear();
  verts.reserve(local.size());
  for (const CVector3f& v : local) {
    verts.push_back(xf * v);
  }
  tris.clear();
  area.GetTriangles(tris);
  return !tris.empty();
}

struct Room {
  TAreaId id;
  CAssetId world;
  int sourceIndex;
  int group;
  std::vector< Column > cells; // world space
  std::vector< Column > tests; // the cells checked for overlaps: those away from the room's edges
  // The room's map triangles seen from above, in world space: x0 y0 x1 y1 x2 y2 for each.
  std::vector< float > footprint;
  CAABox bounds; // world space
};

// A dock and where it leads. The pose of the room it leads to is the pose of the room it's in,
// then `rel`.
struct Link {
  int from;
  int to;
  int dock;
  CVector3f center; // of the dock, in world space
  Pose rel;
  bool moved;
};

// Rooms joined by vanilla doors, placed as one.
struct Group {
  std::vector< int > rooms;
  std::vector< int > links; // moved links to other groups
  CAABox bounds;            // world space
  int tests;
};

struct Candidate {
  int group;
  int parent; // the group it's joined to
  Pose pose;
  CVector3f door; // the door it's joined by, in map space
  int depth;
  std::uint32_t order;
  int hits;
  int checkedStep;
  CAABox bounds;
};

// How a group was placed.
enum EPlacement { kPL_Root, kPL_Door, kPL_Island, kPL_Separate };

struct Result {
  std::vector< Pose > poses;       // by group
  std::vector< EPlacement > kinds; // by group
  std::vector< int > steps;        // by group: when it was placed
  std::vector< int > parents;      // by group: the group it's joined to by a door, or -1
  int islands = 0;
  int forcedOverlaps = 0; // cells of rooms joined by doors with no free space
  float area = 0.f;
};

// The range of Z two rooms cover over part of their overlap, for each of the two.
struct ZRanges {
  float z0[2] = {FLT_MAX, FLT_MAX};
  float z1[2] = {-FLT_MAX, -FLT_MAX};
};

// Where two rooms overlap on the map: quads in map space, drawn when both rooms are. Each patch
// fits one GX index array, as triangles, drawn without culling so they show from above and
// below.
struct OverlapPatch {
  int areaA;
  int areaB;
  bool lines; // edges drawn as lines, rather than faces as triangles
  std::vector< CVector3f > verts;
};

// Vertices in one GX index array, as whole triangles or lines.
constexpr size_t kMaxPatchVerts = 252;
// Cell size of the fine grid overlaps between rooms are worked out on.
constexpr float kFineCell = 0.5f;

class Builder {
public:
  explicit Builder(const CWorld& world) : mWorld(world) {}

  bool Run(std::vector< CTransform4f >& transforms, std::vector< float >& yaws,
           std::vector< OverlapPatch >& overlaps) {
    const auto start = std::chrono::steady_clock::now();
    ReadRooms();
    if (!ReadLinks()) {
      return false;
    }
    MakeGroups();
    ReadElevators();

    std::mt19937 rng(0x6d617073);
    Result best;
    int bestAttempt = 0;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
      // The first two layouts grow from the first room, as sorted; the others from random ones.
      // Every other layout places the biggest groups that fit first, before small ones fill the
      // space around their doors; the others go breadth first.
      const int root = attempt < 2 ? mRooms[0].group : static_cast< int >(rng() % mGroups.size());
      mBigFirst = attempt % 2 == 1;
      Result result = Attempt(root, rng);
      const bool better = result.islands != best.islands ? result.islands < best.islands
                          : result.forcedOverlaps != best.forcedOverlaps
                              ? result.forcedOverlaps < best.forcedOverlaps
                              : result.area < best.area;
      if (attempt == 0 || better) {
        best = std::move(result);
        bestAttempt = attempt;
      }
      if (best.islands == 0 && best.forcedOverlaps == 0) {
        break;
      }
    }

    const int greedyIslands = best.islands;
    Repair(best);

    transforms.assign(mWorld.GetNumAreas(), CTransform4f::Identity());
    yaws.assign(mWorld.GetNumAreas(), 0.f);
    for (const Room& room : mRooms) {
      const Pose& pose = best.poses[room.group];
      transforms[room.id.Value()] =
          CTransform4f::Translate(pose.t) * CTransform4f::RotateZ(CRelAngle::FromRadians(pose.yaw));
      yaws[room.id.Value()] = pose.yaw;
    }

    int moved = 0;
    int met = 0;
    for (const Link& link : mLinks) {
      if (!link.moved) {
        continue;
      }
      ++moved;
      const Pose& from = best.poses[mRooms[link.from].group];
      const Pose& to = best.poses[mRooms[link.to].group];
      met += SamePose(Compose(from, link.rel), to) ? 1 : 0;
    }
    const auto ms = std::chrono::duration_cast< std::chrono::milliseconds >(
                        std::chrono::steady_clock::now() - start)
                        .count();
    Log.info(
        "Laid out {} rooms in {} groups: {} of {} moved doors meet, {} islands ({} before "
        "repair), {} cells overlapping where doors were forced (layout {} of {}, {}), in {} ms",
        mRooms.size(), mGroups.size(), met / 2, moved / 2, best.islands, greedyIslands,
        best.forcedOverlaps, bestAttempt + 1, kAttempts,
        bestAttempt % 2 == 1 ? "biggest first" : "breadth first", ms);
    FindOverlaps(best, overlaps);
    if (sLogEnabled) {
      LogLayout(best);
    }
    return true;
  }

private:
  std::string RoomName(int r) const {
    const CGameArea& area = mWorld.GetAreaAlways(mRooms[r].id);
    if (const randomizer::Database* db = randomizer::GetDatabase()) {
      for (const randomizer::Area& dbArea : db->Areas()) {
        if (dbArea.assetId == area.GetAreaAssetId()) {
          return fmt::format("{} / {}", db->Regions()[dbArea.region].name, dbArea.name);
        }
      }
    }
    return fmt::format("0x{:08X} area {}", mRooms[r].world, mRooms[r].sourceIndex);
  }

  void LogLayout(const Result& result) const {
    static const char* const kKinds[] = {"root", "door", "island", "separate"};
    for (int r = 0; r < static_cast< int >(mRooms.size()); ++r) {
      const Room& room = mRooms[r];
      const CTransform4f& tm = mWorld.GetAreaAlways(room.id).GetTM();
      const Pose& pose = result.poses[room.group];
      Log.info("[layout] room {} ({}): group {} placed {} by {}, yaw {:.1f} at ({:.1f}, {:.1f}, "
               "{:.1f}); {} cells, {} tested; area up ({:.3f}, {:.3f}, {:.3f}) right ({:.3f}, "
               "{:.3f}, {:.3f})",
               room.id.Value(), RoomName(r), room.group, result.steps[room.group],
               kKinds[result.kinds[room.group]], pose.yaw * 180.f / kPi, pose.t.GetX(),
               pose.t.GetY(), pose.t.GetZ(), room.cells.size(), room.tests.size(), tm.Get02(),
               tm.Get12(), tm.Get22(), tm.Get00(), tm.Get10(), tm.Get20());
    }
    // What kept each island from its doors.
    for (const Link& link : mLinks) {
      const int island = mRooms[link.to].group;
      const int from = mRooms[link.from].group;
      if (!link.moved || result.kinds[island] != kPL_Island || from == island ||
          result.steps[from] > result.steps[island]) {
        continue;
      }
      const Pose pose = Compose(result.poses[from], link.rel);
      const CVector3f door = result.poses[from].Apply(link.center);
      std::map< int, int > counts;
      for (const int r : mGroups[island].rooms) {
        for (const Column& column : mRooms[r].tests) {
          if (NearDoor(&door, pose.X(column.x, column.y), pose.Y(column.x, column.y))) {
            continue;
          }
          const auto it = mGrid.find(
              PackKey(CellOf(pose.X(column.x, column.y)), CellOf(pose.Y(column.x, column.y))));
          if (it == mGrid.end()) {
            continue;
          }
          std::vector< int > seen;
          for (const Entry& entry : it->second) {
            if (entry.step < result.steps[island] && entry.group != island &&
                column.z0 + pose.t.GetZ() < entry.z1 - kZTolerance &&
                entry.z0 < column.z1 + pose.t.GetZ() - kZTolerance &&
                std::find(seen.begin(), seen.end(), entry.group) == seen.end()) {
              seen.push_back(entry.group);
              ++counts[entry.group];
            }
          }
        }
      }
      std::vector< std::pair< int, int > > sorted(counts.begin(), counts.end());
      std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      std::string blockers;
      for (size_t i = 0; i < sorted.size() && i < 4; ++i) {
        blockers += fmt::format("{}{} ({} cells)", i == 0 ? "" : ", ",
                                RoomName(mGroups[sorted[i].first].rooms.front()), sorted[i].second);
      }
      Log.info("[layout] island {} by door from {}: limit {}, blocked by {}", RoomName(link.to),
               RoomName(link.from), OverlapLimit(island), blockers.empty() ? "nothing" : blockers);
    }
    for (const Link& link : mLinks) {
      if (!link.moved) {
        continue;
      }
      const Pose& from = result.poses[mRooms[link.from].group];
      const Pose& to = result.poses[mRooms[link.to].group];
      const Pose expected = Compose(from, link.rel);
      const CGameArea& area = mWorld.GetAreaAlways(mRooms[link.from].id);
      const rstl::reserved_vector< CVector3f, 4 >& verts =
          area.GetDock(link.dock).GetPlaneVertices();
      CVector3f normal = CVector3f::Zero();
      if (verts.size() >= 3) {
        normal = CVector3f::Cross(verts[1] - verts[0], verts[2] - verts[0]);
        if (normal.CanBeNormalized()) {
          normal = normal.AsNormalized();
        }
      }
      Log.info("[layout] door {} dock {} -> {}: {} (off by {:.1f} units, {:.1f} deg); normal "
               "({:.2f}, {:.2f}, {:.2f})",
               RoomName(link.from), link.dock, RoomName(link.to),
               SamePose(expected, to) ? "meets" : "apart", (expected.t - to.t).Magnitude(),
               (expected.yaw - to.yaw) * 180.f / kPi, normal.GetX(), normal.GetY(), normal.GetZ());
    }
  }

  void ReadRooms() {
    const CMapWorld* map = mWorld.GetMapWorld();
    std::vector< CVector3f > verts;
    std::vector< int > tris;
    for (int i = 0; i < mWorld.GetNumAreas(); ++i) {
      const CGameArea& area = mWorld.GetAreaAlways(TAreaId(i));
      Room room;
      room.id = TAreaId(i);
      room.world = merged::GetSourceWorld(mWorld.GetWorldAssetId(), room.id);
      room.sourceIndex = merged::GetSourceAreaIndex(room.id);
      room.group = -1;

      std::map< CellKey, std::pair< float, float > > cells;
      const CAssetId mapa = map != nullptr && i < static_cast< int >(map->GetNumAreas())
                                ? map->GetMapAreaRes(i)
                                : kInvalidAssetId;
      if (ReadMapTriangles(mapa, area.GetTM(), verts, tris)) {
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
          const CVector3f& a = verts[tris[t]];
          const CVector3f& b = verts[tris[t + 1]];
          const CVector3f& c = verts[tris[t + 2]];
          RasterizeTriangle(a, b, c, cells);
          // Walls are lines seen from above.
          const float area2 = (b.GetX() - a.GetX()) * (c.GetY() - a.GetY()) -
                              (c.GetX() - a.GetX()) * (b.GetY() - a.GetY());
          if (std::fabs(area2) > 0.02f) {
            room.footprint.insert(room.footprint.end(),
                                  {a.GetX(), a.GetY(), b.GetX(), b.GetY(), c.GetX(), c.GetY()});
          }
        }
      } else {
        const CAABox& box = area.GetAABB();
        const float x0 = box.GetMinPoint().GetX(), y0 = box.GetMinPoint().GetY();
        const float x1 = box.GetMaxPoint().GetX(), y1 = box.GetMaxPoint().GetY();
        room.footprint = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
        for (int ix = CellOf(box.GetMinPoint().GetX()); ix <= CellOf(box.GetMaxPoint().GetX());
             ++ix) {
          for (int iy = CellOf(box.GetMinPoint().GetY()); iy <= CellOf(box.GetMaxPoint().GetY());
               ++iy) {
            AddToCell(cells, ix, iy, box.GetMinPoint().GetZ(), box.GetMaxPoint().GetZ());
          }
        }
      }

      room.bounds = CAABox::MakeMaxInvertedBox();
      for (const auto& [key, z] : cells) {
        const Column column = {(key.first + 0.5f) * kCellSize, (key.second + 0.5f) * kCellSize,
                               z.first, z.second};
        room.cells.push_back(column);
        room.bounds.AccumulateBounds(
            CVector3f(key.first * kCellSize, key.second * kCellSize, z.first));
        room.bounds.AccumulateBounds(
            CVector3f((key.first + 1) * kCellSize, (key.second + 1) * kCellSize, z.second));
        const bool inner = cells.contains(CellKey(key.first - 1, key.second)) &&
                           cells.contains(CellKey(key.first + 1, key.second)) &&
                           cells.contains(CellKey(key.first, key.second - 1)) &&
                           cells.contains(CellKey(key.first, key.second + 1));
        if (inner) {
          room.tests.push_back(column);
        }
      }
      // Narrow rooms are nearly all edge.
      if (room.tests.size() * 4 < room.cells.size()) {
        room.tests = room.cells;
      }
      if (room.cells.empty()) {
        room.bounds = area.GetAABB();
      }
      mRooms.push_back(std::move(room));
    }
    // The same order whichever region is the host, so the layout comes out the same.
    std::sort(mRooms.begin(), mRooms.end(), [](const Room& a, const Room& b) {
      return a.world != b.world ? a.world < b.world : a.sourceIndex < b.sourceIndex;
    });
    mRoomOf.assign(mWorld.GetNumAreas(), -1);
    for (int i = 0; i < static_cast< int >(mRooms.size()); ++i) {
      mRoomOf[mRooms[i].id.Value()] = i;
    }
  }

  // Rooms joined by elevators, from the logic database. Elevators aren't randomized.
  void ReadElevators() {
    const randomizer::Database* db = randomizer::GetDatabase();
    if (db == nullptr) {
      return;
    }
    std::unordered_map< std::uint32_t, int > roomOfAsset;
    for (int r = 0; r < static_cast< int >(mRooms.size()); ++r) {
      roomOfAsset[mWorld.GetAreaAlways(mRooms[r].id).GetAreaAssetId()] = r;
    }
    for (const randomizer::Node& node : db->Nodes()) {
      if (!node.dock || node.dock->type != "teleporter" || node.dock->target < 0) {
        continue;
      }
      const auto from = roomOfAsset.find(db->Areas()[node.area].assetId);
      const auto to = roomOfAsset.find(db->Areas()[db->GetNode(node.dock->target).area].assetId);
      if (from != roomOfAsset.end() && to != roomOfAsset.end() && from->second < to->second) {
        mElevators.emplace_back(from->second, to->second);
      }
    }
  }

  // False when no door was moved.
  bool ReadLinks() {
    bool anyMoved = false;
    for (int r = 0; r < static_cast< int >(mRooms.size()); ++r) {
      const CGameArea& area = mWorld.GetAreaAlways(mRooms[r].id);
      for (int dock = 0; dock < area.GetDockCount(); ++dock) {
        const IGameArea::Dock& gameDock = area.GetDock(dock);
        if (gameDock.GetDockRefs().empty()) {
          continue;
        }
        const TAreaId target = gameDock.GetConnectedAreaId(gameDock.GetReferenceCount());
        if (!mWorld.DoesAreaExist(target) || target == mRooms[r].id) {
          continue;
        }
        Link link;
        link.from = r;
        link.to = mRoomOf[target.Value()];
        link.dock = dock;
        link.center = CVector3f::Zero();
        const rstl::reserved_vector< CVector3f, 4 >& verts = gameDock.GetPlaneVertices();
        for (int v = 0; v < verts.size(); ++v) {
          link.center += verts[v] * (1.f / static_cast< float >(verts.size()));
        }
        CTransform4f xf = CTransform4f::Identity();
        link.moved = portals::GetDockTransform(mWorld, mRooms[r].id, dock, xf);
        if (link.moved) {
          // The dock transform takes points by this door to the same places by the door it leads
          // to. Drawing the far room through its inverse puts those places together on the map.
          link.rel = Inverse(Pose(std::atan2(xf.Get10(), xf.Get00()), xf.GetTranslation()));
          anyMoved = true;
        }
        mLinks.push_back(link);
      }
    }
    return anyMoved;
  }

  int FindGroupRoot(std::vector< int >& parent, int i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  }

  void MakeGroups() {
    std::vector< int > parent(mRooms.size());
    for (size_t i = 0; i < parent.size(); ++i) {
      parent[i] = static_cast< int >(i);
    }
    for (const Link& link : mLinks) {
      if (!link.moved) {
        const int a = FindGroupRoot(parent, link.from);
        const int b = FindGroupRoot(parent, link.to);
        parent[std::max(a, b)] = std::min(a, b);
      }
    }
    std::vector< int > groupOfRoot(mRooms.size(), -1);
    for (int r = 0; r < static_cast< int >(mRooms.size()); ++r) {
      const int root = FindGroupRoot(parent, r);
      if (groupOfRoot[root] < 0) {
        groupOfRoot[root] = static_cast< int >(mGroups.size());
        Group group;
        group.bounds = CAABox::MakeMaxInvertedBox();
        group.tests = 0;
        mGroups.push_back(group);
      }
      Group& group = mGroups[groupOfRoot[root]];
      mRooms[r].group = groupOfRoot[root];
      group.rooms.push_back(r);
      group.bounds.Include(mRooms[r].bounds);
      group.tests += static_cast< int >(mRooms[r].tests.size());
    }
    for (int l = 0; l < static_cast< int >(mLinks.size()); ++l) {
      const Link& link = mLinks[l];
      if (link.moved && mRooms[link.from].group != mRooms[link.to].group) {
        mGroups[mRooms[link.from].group].links.push_back(l);
      }
    }
  }

  int OverlapLimit(int group) const {
    return std::max(kMinOverlaps, mGroups[group].tests / kOverlapsPerCell);
  }

  static bool NearDoor(const CVector3f* door, float x, float y) {
    if (door == nullptr) {
      return false;
    }
    const float dx = x - door->GetX();
    const float dy = y - door->GetY();
    return dx * dx + dy * dy < kDoorClearance * kDoorClearance;
  }

  // How many of the group's cells, posed there, land on rooms placed at `sinceStep` or later,
  // leaving out those by `door`, the door it's joined by if any. Stops counting past `limit`.
  int CountOverlaps(int group, const Pose& pose, int sinceStep, int limit,
                    const CVector3f* door = nullptr) const {
    int hits = 0;
    for (const int r : mGroups[group].rooms) {
      for (const Column& column : mRooms[r].tests) {
        const float x = pose.X(column.x, column.y);
        const float y = pose.Y(column.x, column.y);
        if (NearDoor(door, x, y)) {
          continue;
        }
        const auto it = mGrid.find(PackKey(CellOf(x), CellOf(y)));
        if (it == mGrid.end()) {
          continue;
        }
        const float z0 = column.z0 + pose.t.GetZ();
        const float z1 = column.z1 + pose.t.GetZ();
        for (const Entry& entry : it->second) {
          if (entry.step >= sinceStep && entry.group != group && z0 < entry.z1 - kZTolerance &&
              entry.z0 < z1 - kZTolerance) {
            ++hits;
            break;
          }
        }
        if (hits > limit) {
          return hits;
        }
      }
    }
    return hits;
  }

  // Overlaps of `group` posed there with the grid, leaving out where the groups flagged in
  // `moving` were (but not where they're being tried), and the cells by `door`. Adds up the cells
  // each other group overlaps in `byGroup` if given. Stops counting past `limit`.
  int CountOverlapsMoving(int group, const Pose& pose, const std::vector< char >& moving,
                          const CVector3f* door, int limit, std::map< int, int >* byGroup) const {
    int hits = 0;
    std::vector< int > seen;
    for (const int r : mGroups[group].rooms) {
      for (const Column& column : mRooms[r].tests) {
        const float x = pose.X(column.x, column.y);
        const float y = pose.Y(column.x, column.y);
        if (NearDoor(door, x, y)) {
          continue;
        }
        const auto it = mGrid.find(PackKey(CellOf(x), CellOf(y)));
        if (it == mGrid.end()) {
          continue;
        }
        const float z0 = column.z0 + pose.t.GetZ();
        const float z1 = column.z1 + pose.t.GetZ();
        seen.clear();
        for (const Entry& entry : it->second) {
          if (entry.group == group || (moving[entry.group] && entry.step != kTempStep) ||
              !(z0 < entry.z1 - kZTolerance && entry.z0 < z1 - kZTolerance) ||
              std::find(seen.begin(), seen.end(), entry.group) != seen.end()) {
            continue;
          }
          seen.push_back(entry.group);
          if (byGroup != nullptr) {
            ++(*byGroup)[entry.group];
          }
        }
        hits += seen.empty() ? 0 : 1;
        if (hits > limit && byGroup == nullptr) {
          return hits;
        }
      }
    }
    return hits;
  }

  bool OverlapsPlaced(const CAABox& bounds, int sinceStep) const {
    for (int s = sinceStep; s < static_cast< int >(mPlacedBounds.size()); ++s) {
      if (mPlacedBounds[s].DoBoundsOverlap(bounds)) {
        return true;
      }
    }
    return false;
  }

  bool Fits(int group, const Pose& pose) const {
    if (!OverlapsPlaced(TransformBounds(mGroups[group].bounds, pose), 0)) {
      return true;
    }
    const int limit = OverlapLimit(group);
    return CountOverlaps(group, pose, 0, limit) <= limit;
  }

  // The free place nearest `start`, turned the same way, no further than `maxRing` rings out.
  bool FindFreePose(int group, const Pose& start, int maxRing, Pose& out, int& ring) const {
    for (ring = 0; ring <= maxRing; ++ring) {
      const int count = ring == 0 ? 1 : std::max(8, static_cast< int >(2.f * kPi * ring));
      for (int k = 0; k < count; ++k) {
        const float angle = 2.f * kPi * k / count;
        const float distance = ring * kIslandRingStep;
        const Pose pose(start.yaw, start.t + CVector3f(distance * std::cos(angle),
                                                       distance * std::sin(angle), 0.f));
        if (Fits(group, pose)) {
          out = pose;
          return true;
        }
      }
    }
    return false;
  }

  // Past everything placed so far.
  Pose FarPose(int group, const Pose& start) const {
    CAABox all = CAABox::MakeMaxInvertedBox();
    for (const CAABox& box : mPlacedBounds) {
      all.Include(box);
    }
    const CAABox posed = TransformBounds(mGroups[group].bounds, start);
    return Pose(start.yaw, start.t + CVector3f(all.GetMaxPoint().GetX() -
                                                   posed.GetMinPoint().GetX() + kIslandRingStep,
                                               0.f, 0.f));
  }

  // Puts the group's cells in the grid, recording the cells it went into in `inserted` if given.
  void InsertGroup(int group, const Pose& pose, int step, std::vector< std::int64_t >* inserted) {
    // Spread each cell over the cells its corners land in, so that turned rooms leave no gaps.
    const float spread = 0.35f * kCellSize;
    for (const int r : mGroups[group].rooms) {
      for (const Column& column : mRooms[r].cells) {
        const float x = pose.X(column.x, column.y);
        const float y = pose.Y(column.x, column.y);
        const Entry entry = {group, step, column.z0 + pose.t.GetZ(), column.z1 + pose.t.GetZ()};
        std::int64_t keys[4];
        int keyCount = 0;
        for (int corner = 0; corner < 4; ++corner) {
          const std::int64_t key = PackKey(CellOf(x + ((corner & 1) ? spread : -spread)),
                                           CellOf(y + ((corner & 2) ? spread : -spread)));
          if (std::find(keys, keys + keyCount, key) == keys + keyCount) {
            keys[keyCount++] = key;
            mGrid[key].push_back(entry);
            if (inserted != nullptr) {
              inserted->push_back(key);
            }
          }
        }
      }
    }
  }

  // Takes out what InsertGroup put in, the last insertions first.
  void RemoveInserted(const std::vector< std::int64_t >& inserted) {
    for (auto it = inserted.rbegin(); it != inserted.rend(); ++it) {
      mGrid[*it].pop_back();
    }
  }

  void RebuildGrid(const Result& result) {
    mGrid.clear();
    mPlacedBounds.clear();
    for (int g = 0; g < static_cast< int >(mGroups.size()); ++g) {
      mPlacedBounds.push_back(TransformBounds(mGroups[g].bounds, result.poses[g]));
      InsertGroup(g, result.poses[g], result.steps[g], nullptr);
    }
  }

  void Place(int group, const Pose& pose, int depth, Result& result, std::vector< int >& depths,
             std::vector< Candidate >& frontier, std::mt19937& rng) {
    const int step = static_cast< int >(mPlacedBounds.size());
    depths[group] = depth;
    result.steps[group] = step;
    result.poses[group] = pose;
    mPlacedBounds.push_back(TransformBounds(mGroups[group].bounds, pose));
    InsertGroup(group, pose, step, nullptr);

    for (const int l : mGroups[group].links) {
      const Link& link = mLinks[l];
      const int to = mRooms[link.to].group;
      if (depths[to] >= 0) {
        continue;
      }
      Candidate candidate;
      candidate.group = to;
      candidate.parent = group;
      candidate.pose = Compose(pose, link.rel);
      candidate.door = pose.Apply(link.center);
      candidate.depth = depth + 1;
      candidate.order = rng();
      candidate.hits = 0;
      candidate.checkedStep = 0;
      candidate.bounds = TransformBounds(mGroups[to].bounds, candidate.pose);
      frontier.push_back(candidate);
    }
  }

  // The group and every group joined to it by a door, directly or not, that was placed after it.
  std::vector< int > Subtree(const Result& result, int root) const {
    std::vector< int > members{root};
    for (size_t i = 0; i < members.size(); ++i) {
      for (int g = 0; g < static_cast< int >(mGroups.size()); ++g) {
        if (result.parents[g] == members[i]) {
          members.push_back(g);
        }
      }
    }
    return members;
  }

  // Whether `members` fit with each of them moved by `delta`, `joined` by the door at `door`.
  bool FitsMoved(const Result& result, const std::vector< int >& members, const Pose& delta,
                 const std::vector< char >& moving, int joined, const CVector3f* door,
                 std::map< int, int >* byGroup) const {
    bool fits = true;
    for (const int g : members) {
      const int limit = OverlapLimit(g);
      const int hits = CountOverlapsMoving(g, Compose(delta, result.poses[g]), moving,
                                           g == joined ? door : nullptr, limit, byGroup);
      if (hits > limit) {
        fits = false;
        if (byGroup == nullptr) {
          return false;
        }
      }
    }
    return fits;
  }

  void InsertMoved(const Result& result, const std::vector< int >& members, const Pose& delta,
                   std::vector< std::int64_t >& inserted) {
    for (const int g : members) {
      InsertGroup(g, Compose(delta, result.poses[g]), kTempStep, &inserted);
    }
  }

  struct Move {
    std::vector< int > members;
    Pose delta;
    EPlacement kind;
    int parent;
  };

  // Somewhere for `members`, the group `head` and what hangs off it, to go out of the way: by
  // another of the head's doors if one fits, otherwise in the nearest free space. Costs one door
  // that met if the head was joined by a door and becomes an island.
  bool PlanMove(const Result& result, const std::vector< int >& members,
                const std::vector< Pose >& newPoses, const std::vector< char >& moving, Move& out,
                int& cost) const {
    const int head = members.front();
    out.members = members;
    for (const Link& link : mLinks) {
      const int to = mRooms[link.to].group;
      const int from = mRooms[link.from].group;
      if (!link.moved || to != head || (moving[from] && newPoses[from].yaw == kNoPose)) {
        continue;
      }
      const Pose& fromPose = moving[from] ? newPoses[from] : result.poses[from];
      const Pose delta = Compose(Compose(fromPose, link.rel), Inverse(result.poses[head]));
      const CVector3f door = fromPose.Apply(link.center);
      if (FitsMoved(result, members, delta, moving, head, &door, nullptr)) {
        out.delta = delta;
        out.kind = kPL_Door;
        out.parent = from;
        cost = 0;
        return true;
      }
    }
    for (int ring = 1; ring <= kRepairRings; ++ring) {
      const int count = std::max(8, static_cast< int >(2.f * kPi * ring));
      for (int k = 0; k < count; ++k) {
        const float angle = 2.f * kPi * k / count;
        const float distance = ring * kIslandRingStep;
        const Pose delta(0.f,
                         CVector3f(distance * std::cos(angle), distance * std::sin(angle), 0.f));
        if (FitsMoved(result, members, delta, moving, -1, nullptr, nullptr)) {
          out.delta = delta;
          out.kind = result.kinds[head] == kPL_Door ? kPL_Island : result.kinds[head];
          out.parent = -1;
          cost = result.kinds[head] == kPL_Door ? 1 : 0;
          return true;
        }
      }
    }
    return false;
  }

  void Apply(Result& result, const Move& move) {
    for (const int g : move.members) {
      result.poses[g] = Compose(move.delta, result.poses[g]);
    }
    result.kinds[move.members.front()] = move.kind;
    result.parents[move.members.front()] = move.parent;
  }

  // Tries to join island `island` to the layout by one of its doors, moving what's in the way.
  // Only takes a change that makes more doors meet.
  bool TryJoin(Result& result, int island) {
    const std::vector< int > members = Subtree(result, island);
    std::vector< char > inMembers(mGroups.size(), 0);
    for (const int g : members) {
      inMembers[g] = 1;
    }
    for (const Link& link : mLinks) {
      const int from = mRooms[link.from].group;
      if (!link.moved || mRooms[link.to].group != island || inMembers[from]) {
        continue;
      }
      Move join;
      join.members = members;
      join.delta = Compose(Compose(result.poses[from], link.rel), Inverse(result.poses[island]));
      join.kind = kPL_Door;
      join.parent = from;
      const CVector3f door = result.poses[from].Apply(link.center);
      std::map< int, int > blockers;
      if (FitsMoved(result, members, join.delta, inMembers, island, &door, &blockers)) {
        Apply(result, join);
        RebuildGrid(result);
        return true;
      }
      if (blockers.size() > kMaxBlockers) {
        continue;
      }

      // Move each group in the way, with what hangs off it, out of the way.
      std::vector< char > moving = inMembers;
      std::vector< std::vector< int > > blockerMembers;
      bool valid = true;
      for (const auto& [blocker, hits] : blockers) {
        if (moving[blocker]) {
          continue; // already moving with another group in the way
        }
        std::vector< int > sub = Subtree(result, blocker);
        for (const int g : sub) {
          valid = valid && !inMembers[g] && g != from;
          moving[g] = 1;
        }
        blockerMembers.push_back(std::move(sub));
      }
      if (!valid) {
        continue;
      }
      // Where everything on the move goes, kNoPose until planned.
      std::vector< Pose > newPoses(mGroups.size(), Pose(kNoPose, CVector3f::Zero()));
      for (const int g : members) {
        newPoses[g] = Compose(join.delta, result.poses[g]);
      }
      std::vector< std::int64_t > inserted;
      InsertMoved(result, members, join.delta, inserted);
      std::vector< Move > moves;
      int cost = 0;
      for (const std::vector< int >& sub : blockerMembers) {
        Move move;
        int moveCost = 0;
        if (!PlanMove(result, sub, newPoses, moving, move, moveCost)) {
          valid = false;
          break;
        }
        cost += moveCost;
        for (const int g : sub) {
          newPoses[g] = Compose(move.delta, result.poses[g]);
        }
        InsertMoved(result, sub, move.delta, inserted);
        moves.push_back(std::move(move));
      }
      RemoveInserted(inserted);
      // The island joining is one more door that meets.
      if (!valid || cost >= 1) {
        continue;
      }
      Apply(result, join);
      for (const Move& move : moves) {
        Apply(result, move);
      }
      RebuildGrid(result);
      return true;
    }
    return false;
  }

  // Cells where the inner parts of two rooms in different groups overlap as seen from above, at
  // any height, merged into quads along rows and laid on top of the higher room, where the map
  // shows them over each other. The rooms' edges are left out, so rooms that only meet don't show.
  void FindOverlaps(const Result& result, std::vector< OverlapPatch >& overlaps) const {
    struct Inner {
      int room;
      float z0;
      float z1;
    };
    std::unordered_map< std::int64_t, std::vector< Inner > > inner;
    // Half a cell each way, so that turned rooms leave no gaps between cells.
    const float spread = 0.5f * kCellSize;
    for (int r = 0; r < static_cast< int >(mRooms.size()); ++r) {
      const Pose& pose = result.poses[mRooms[r].group];
      for (const Column& column : mRooms[r].tests) {
        const float x = pose.X(column.x, column.y);
        const float y = pose.Y(column.x, column.y);
        const Inner cell = {r, column.z0 + pose.t.GetZ(), column.z1 + pose.t.GetZ()};
        std::int64_t keys[4];
        int keyCount = 0;
        for (int corner = 0; corner < 4; ++corner) {
          const std::int64_t key = PackKey(CellOf(x + ((corner & 1) ? spread : -spread)),
                                           CellOf(y + ((corner & 2) ? spread : -spread)));
          if (std::find(keys, keys + keyCount, key) == keys + keyCount) {
            keys[keyCount++] = key;
            inner[key].push_back(cell);
          }
        }
      }
    }

    // By pair of rooms, by row: the cells and the range of Z the two rooms cover there.
    std::map< std::pair< int, int >, std::map< std::pair< int, int >, ZRanges > > pairs;
    for (const auto& [key, cells] : inner) {
      const int ix = static_cast< int >(key >> 32);
      const int iy = static_cast< int >(static_cast< std::int32_t >(key & 0xffffffff));
      for (size_t i = 0; i < cells.size(); ++i) {
        for (size_t j = i + 1; j < cells.size(); ++j) {
          const Inner& a = cells[i];
          const Inner& b = cells[j];
          if (mRooms[a.room].group == mRooms[b.room].group) {
            continue;
          }
          const std::pair< int, int > rooms(std::min(a.room, b.room), std::max(a.room, b.room));
          ZRanges& ranges = pairs[rooms][std::make_pair(iy, ix)];
          for (const Inner* cell : {&a, &b}) {
            const int which = cell->room == rooms.first ? 0 : 1;
            ranges.z0[which] = std::min(ranges.z0[which], cell->z0);
            ranges.z1[which] = std::max(ranges.z1[which], cell->z1);
          }
        }
      }
    }

    overlaps.clear();
    for (const auto& [rooms, cells] : pairs) {
      AddSmoothOverlap(result, rooms.first, rooms.second, cells, overlaps);
    }
  }

  // Marks the fine cells whose centers the room covers, seen from above, posed on the map.
  void RasterizeFootprint(int room, const Pose& pose, float originX, float originY, int width,
                          int height, std::vector< char >& covered) const {
    const std::vector< float >& tris = mRooms[room].footprint;
    for (size_t t = 0; t + 5 < tris.size(); t += 6) {
      float x[3], y[3];
      for (int v = 0; v < 3; ++v) {
        x[v] = pose.X(tris[t + 2 * v], tris[t + 2 * v + 1]);
        y[v] = pose.Y(tris[t + 2 * v], tris[t + 2 * v + 1]);
      }
      const float area2 = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
      const float sign = area2 < 0.f ? -1.f : 1.f;
      const int i0 = std::max(
          0, static_cast< int >(std::floor((std::min({x[0], x[1], x[2]}) - originX) / kFineCell)));
      const int i1 = std::min(
          width - 1,
          static_cast< int >(std::floor((std::max({x[0], x[1], x[2]}) - originX) / kFineCell)));
      const int j0 = std::max(
          0, static_cast< int >(std::floor((std::min({y[0], y[1], y[2]}) - originY) / kFineCell)));
      const int j1 = std::min(
          height - 1,
          static_cast< int >(std::floor((std::max({y[0], y[1], y[2]}) - originY) / kFineCell)));
      for (int j = j0; j <= j1; ++j) {
        const float py = originY + (j + 0.5f) * kFineCell;
        for (int i = i0; i <= i1; ++i) {
          const float px = originX + (i + 0.5f) * kFineCell;
          bool inside = true;
          for (int e = 0; e < 3 && inside; ++e) {
            const int n = (e + 1) % 3;
            const float edge = (x[n] - x[e]) * (py - y[e]) - (y[n] - y[e]) * (px - x[e]);
            inside = edge * sign >= 0.f;
          }
          if (inside) {
            covered[j * width + i] = 1;
          }
        }
      }
    }
  }

  // The overlap of two rooms as seen from above, from their map triangles on a fine grid, with
  // smooth edges by marching squares, as one flat sheet a little above the higher ceiling.
  void AddSmoothOverlap(const Result& result, int roomA, int roomB,
                        const std::map< std::pair< int, int >, ZRanges >& cells,
                        std::vector< OverlapPatch >& overlaps) const {
    // The range of Z each room covers over the overlap.
    ZRanges whole;
    int minX = INT_MAX, maxX = INT_MIN, minY = INT_MAX, maxY = INT_MIN;
    for (const auto& [cell, range] : cells) {
      for (int k = 0; k < 2; ++k) {
        whole.z0[k] = std::min(whole.z0[k], range.z0[k]);
        whole.z1[k] = std::max(whole.z1[k], range.z1[k]);
      }
      minY = std::min(minY, cell.first);
      maxY = std::max(maxY, cell.first);
      minX = std::min(minX, cell.second);
      maxX = std::max(maxX, cell.second);
    }
    // The coarse cells are the rooms' inner parts; the overlap reaches past them.
    const float originX = (minX - 2) * kCellSize;
    const float originY = (minY - 2) * kCellSize;
    const int width = static_cast< int >((maxX - minX + 5) * kCellSize / kFineCell);
    const int height = static_cast< int >((maxY - minY + 5) * kCellSize / kFineCell);
    std::vector< char > coveredA(width * height, 0);
    std::vector< char > coveredB(width * height, 0);
    RasterizeFootprint(roomA, result.poses[mRooms[roomA].group], originX, originY, width, height,
                       coveredA);
    RasterizeFootprint(roomB, result.poses[mRooms[roomB].group], originX, originY, width, height,
                       coveredB);
    // Covered by both, one fine cell in from the edges: rooms that meet along a wall or at a door
    // touch, and slivers where their geometry pokes past it aren't worth shading.
    std::vector< char > both(width * height, 0);
    for (int j = 1; j + 1 < height; ++j) {
      for (int i = 1; i + 1 < width; ++i) {
        bool all = true;
        for (int dj = -1; dj <= 1 && all; ++dj) {
          for (int di = -1; di <= 1 && all; ++di) {
            const int k = (j + dj) * width + (i + di);
            all = coveredA[k] && coveredB[k];
          }
        }
        both[j * width + i] = all ? 1 : 0;
      }
    }

    // A shell around where the two rooms take up the same space: the overlap's outline as a floor
    // a little below the higher of their floors and a ceiling a little above the lower of their
    // ceilings, walls between them along the outline, and the outline drawn as lines at both
    // heights. Rooms that only lie above one another get none.
    const float shellBottom = std::max(whole.z0[0], whole.z0[1]);
    const float shellTop = std::min(whole.z1[0], whole.z1[1]);
    if (shellTop - shellBottom < kZTolerance) {
      return;
    }
    const std::pair< float, float > shells[] = {{shellBottom, shellTop}};
    float zTop = 0.f;
    float zBottom = 0.f;
    OverlapPatch faces;
    faces.areaA = mRooms[roomA].id.Value();
    faces.areaB = mRooms[roomB].id.Value();
    faces.lines = false;
    OverlapPatch edges = faces;
    edges.lines = true;
    const auto add = [&](OverlapPatch& patch, const CVector3f* verts, size_t count) {
      if (patch.verts.size() + count > kMaxPatchVerts) {
        overlaps.push_back(patch);
        patch.verts.clear();
      }
      patch.verts.insert(patch.verts.end(), verts, verts + count);
    };
    const auto toMap = [&](float x, float y, float z) {
      return CVector3f(originX + (x + 0.5f) * kFineCell, originY + (y + 0.5f) * kFineCell, z);
    };
    // A convex polygon in sample coordinates, as a fan of triangles at both heights.
    const auto addPolygon = [&](const float (*points)[2], int count) {
      for (const float z : {zTop, zBottom}) {
        for (int k = 1; k + 1 < count; ++k) {
          const CVector3f tri[3] = {toMap(points[0][0], points[0][1], z),
                                    toMap(points[k][0], points[k][1], z),
                                    toMap(points[k + 1][0], points[k + 1][1], z)};
          add(faces, tri, 3);
        }
      }
    };
    // A piece of the outline, in sample coordinates: a wall and its two edges.
    const auto addSegment = [&](float x0, float y0, float x1, float y1) {
      const CVector3f a0 = toMap(x0, y0, zBottom), a1 = toMap(x0, y0, zTop);
      const CVector3f b0 = toMap(x1, y1, zBottom), b1 = toMap(x1, y1, zTop);
      const CVector3f wall[6] = {a0, b0, b1, a0, b1, a1};
      add(faces, wall, 6);
      const CVector3f lines[4] = {a0, b0, a1, b1};
      add(edges, lines, 4);
    };
    // Corners 0 (0,0) 1 (1,0) 2 (1,1) 3 (0,1), then edge midpoints 4 bottom 5 right 6 top 7 left.
    static const float kPoints[8][2] = {{0, 0},    {1, 0},    {1, 1},    {0, 1},
                                        {0.5f, 0}, {1, 0.5f}, {0.5f, 1}, {0, 0.5f}};
    // The covered part of a square for each case (bit k set when corner k is covered).
    static const int kCases[16][7] = {
        {0},
        {3, 0, 4, 7},
        {3, 4, 1, 5},
        {4, 0, 1, 5, 7},
        {3, 5, 2, 6},
        {6, 0, 4, 5, 2, 6, 7},
        {4, 4, 1, 2, 6},
        {5, 0, 1, 2, 6, 7},
        {3, 7, 6, 3},
        {4, 0, 4, 6, 3},
        {6, 4, 1, 5, 6, 3, 7},
        {5, 0, 1, 5, 6, 3},
        {4, 7, 5, 2, 3},
        {5, 0, 4, 5, 2, 3},
        {5, 4, 1, 2, 3, 7},
        {0},
    };
    // The outline through each case, as pairs of edge midpoints.
    static const int kSegments[16][5] = {
        {0},       {1, 4, 7}, {1, 4, 5}, {1, 5, 7}, {1, 5, 6},       {2, 4, 5, 6, 7},
        {1, 4, 6}, {1, 6, 7}, {1, 6, 7}, {1, 4, 6}, {2, 5, 6, 7, 4}, {1, 5, 6},
        {1, 7, 5}, {1, 4, 5}, {1, 7, 4}, {0},
    };
    for (const auto& [low, high] : shells) {
      zBottom = low - 0.5f;
      zTop = high + 0.5f;
      for (int j = 0; j + 1 < height; ++j) {
        int fullStart = -1;
        for (int i = 0; i + 1 <= width; ++i) {
          int which = 0;
          if (i + 1 < width) {
            which = (both[j * width + i] ? 1 : 0) | (both[j * width + i + 1] ? 2 : 0) |
                    (both[(j + 1) * width + i + 1] ? 4 : 0) | (both[(j + 1) * width + i] ? 8 : 0);
          }
          // Full squares next to each other in a row become one rectangle.
          if (which == 15) {
            if (fullStart < 0) {
              fullStart = i;
            }
            continue;
          }
          if (fullStart >= 0) {
            const float rect[4][2] = {
                {static_cast< float >(fullStart), static_cast< float >(j)},
                {static_cast< float >(i), static_cast< float >(j)},
                {static_cast< float >(i), static_cast< float >(j + 1)},
                {static_cast< float >(fullStart), static_cast< float >(j + 1)}};
            addPolygon(rect, 4);
            fullStart = -1;
          }
          const int count = kCases[which][0];
          if (count >= 3) {
            float points[6][2];
            for (int k = 0; k < count; ++k) {
              points[k][0] = i + kPoints[kCases[which][k + 1]][0];
              points[k][1] = j + kPoints[kCases[which][k + 1]][1];
            }
            addPolygon(points, count);
          }
          for (int k = 0; k < kSegments[which][0]; ++k) {
            const float* from = kPoints[kSegments[which][1 + 2 * k]];
            const float* to = kPoints[kSegments[which][2 + 2 * k]];
            addSegment(i + from[0], j + from[1], i + to[0], j + to[1]);
          }
        }
      }
    }
    for (OverlapPatch* patch : {&faces, &edges}) {
      if (!patch->verts.empty()) {
        overlaps.push_back(std::move(*patch));
      }
    }
  }

  void Repair(Result& result) {
    RebuildGrid(result);
    for (int pass = 0; pass < kRepairPasses; ++pass) {
      bool improved = false;
      for (int g = 0; g < static_cast< int >(mGroups.size()); ++g) {
        if (result.kinds[g] == kPL_Island && TryJoin(result, g)) {
          improved = true;
        }
      }
      if (!improved) {
        break;
      }
    }
    result.islands = 0;
    for (const EPlacement kind : result.kinds) {
      result.islands += kind == kPL_Island || kind == kPL_Separate ? 1 : 0;
    }
  }

  bool Before(const Candidate& a, const Candidate& b) const {
    if (mBigFirst && mGroups[a.group].tests != mGroups[b.group].tests) {
      return mGroups[a.group].tests > mGroups[b.group].tests;
    }
    return a.depth != b.depth ? a.depth < b.depth : a.order < b.order;
  }

  Result Attempt(int root, std::mt19937& rng) {
    mGrid.clear();
    mPlacedBounds.clear();
    Result result;
    result.poses.assign(mGroups.size(), Pose());
    result.kinds.assign(mGroups.size(), kPL_Root);
    result.steps.assign(mGroups.size(), 0);
    result.parents.assign(mGroups.size(), -1);
    std::vector< int > depths(mGroups.size(), -1);
    std::vector< Candidate > frontier;
    Place(root, Pose(), 0, result, depths, frontier, rng);

    for (size_t placed = 1; placed < mGroups.size(); ++placed) {
      std::erase_if(frontier, [&](const Candidate& c) { return depths[c.group] >= 0; });
      const int step = static_cast< int >(mPlacedBounds.size());
      int best = -1;
      int first = -1;
      for (int i = 0; i < static_cast< int >(frontier.size()); ++i) {
        Candidate& c = frontier[i];
        if (first < 0 || Before(c, frontier[first])) {
          first = i;
        }
        const int limit = OverlapLimit(c.group);
        // Hits add up over the groups placed since the candidate was last checked.
        if (c.hits <= limit && c.checkedStep < step && OverlapsPlaced(c.bounds, c.checkedStep)) {
          c.hits += CountOverlaps(c.group, c.pose, c.checkedStep, limit - c.hits, &c.door);
        }
        c.checkedStep = step;
        if (c.hits <= limit && (best < 0 || Before(c, frontier[best]))) {
          best = i;
        }
      }

      if (best >= 0) {
        const Candidate c = frontier[best];
        Place(c.group, c.pose, c.depth, result, depths, frontier, rng);
        result.kinds[c.group] = kPL_Door;
        result.parents[c.group] = c.parent;
      } else if (first >= 0 && sForceDoors) {
        // Every door into what's placed is blocked: take the one with the least overlap.
        int chosen = -1;
        int chosenHits = INT_MAX;
        for (int i = 0; i < static_cast< int >(frontier.size()); ++i) {
          const Candidate& c = frontier[i];
          const int hits = CountOverlaps(c.group, c.pose, 0, chosenHits, &c.door);
          if (hits < chosenHits || (hits == chosenHits && Before(c, frontier[chosen]))) {
            chosen = i;
            chosenHits = hits;
          }
        }
        const Candidate c = frontier[chosen];
        Place(c.group, c.pose, c.depth, result, depths, frontier, rng);
        result.kinds[c.group] = kPL_Door;
        result.parents[c.group] = c.parent;
        result.forcedOverlaps += chosenHits;
      } else if (first >= 0) {
        // Every door into what's placed is blocked. Of all those doors, the island goes by the
        // one with free space nearest it, so at least that door's two sides end up close.
        std::vector< int > order(frontier.size());
        for (size_t i = 0; i < order.size(); ++i) {
          order[i] = static_cast< int >(i);
        }
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return Before(frontier[a], frontier[b]); });
        int chosen = first;
        Pose pose = FarPose(frontier[first].group, frontier[first].pose);
        int bestRing = kIslandRings + 1;
        for (const int i : order) {
          Pose free;
          int ring = 0;
          if (FindFreePose(frontier[i].group, frontier[i].pose, bestRing - 1, free, ring)) {
            chosen = i;
            pose = free;
            bestRing = ring;
            if (ring <= 1) {
              break;
            }
          }
        }
        const Candidate c = frontier[chosen];
        Place(c.group, pose, c.depth, result, depths, frontier, rng);
        result.kinds[c.group] = kPL_Island;
        ++result.islands;
      } else {
        // No door leads from anything placed to what's left. Start again by an elevator from
        // something placed if there is one, so the elevator's two ends are drawn together;
        // otherwise next to everything.
        int group = -1;
        Pose start;
        for (const auto& [a, b] : mElevators) {
          for (int side = 0; side < 2 && group < 0; ++side) {
            const int here = side == 0 ? a : b;
            const int there = side == 0 ? b : a;
            if (depths[mRooms[here].group] < 0 && depths[mRooms[there].group] >= 0) {
              group = mRooms[here].group;
              const CVector3f target =
                  result.poses[mRooms[there].group].Apply(mRooms[there].bounds.GetCenterPoint());
              start = Pose(0.f, target - mRooms[here].bounds.GetCenterPoint());
            }
          }
          if (group >= 0) {
            break;
          }
        }
        if (group < 0) {
          for (const Room& room : mRooms) {
            if (depths[room.group] < 0) {
              group = room.group;
              break;
            }
          }
          CAABox all = CAABox::MakeMaxInvertedBox();
          for (const CAABox& box : mPlacedBounds) {
            all.Include(box);
          }
          start = Pose(0.f, all.GetCenterPoint() - mGroups[group].bounds.GetCenterPoint());
        }
        Pose pose = FarPose(group, start);
        int ring = 0;
        FindFreePose(group, start, kIslandRings, pose, ring);
        Place(group, pose, 0, result, depths, frontier, rng);
        result.kinds[group] = kPL_Separate;
        ++result.islands;
      }
    }

    CAABox all = CAABox::MakeMaxInvertedBox();
    for (const CAABox& box : mPlacedBounds) {
      all.Include(box);
    }
    result.area = BoundsArea(all);
    return result;
  }

  struct Entry {
    int group;
    int step;
    float z0;
    float z1;
  };

  const CWorld& mWorld;
  std::vector< Room > mRooms; // sorted by region and index there
  std::vector< int > mRoomOf; // by area index
  std::vector< Link > mLinks;
  std::vector< Group > mGroups;
  // Cells of placed rooms in map space.
  std::unordered_map< std::int64_t, std::vector< Entry > > mGrid;
  std::vector< CAABox > mPlacedBounds; // by placement step
  bool mBigFirst = false;
  std::vector< std::pair< int, int > > mElevators; // rooms
};

const CWorld* sOwner = nullptr;
std::vector< CTransform4f > sTransforms; // by area index
std::vector< float > sYaws;
std::vector< OverlapPatch > sOverlaps;

} // namespace

void Build(const CWorld& world) {
  sOwner = nullptr;
  sTransforms.clear();
  sYaws.clear();
  sOverlaps.clear();
  if (sDisabled || world.GetNumAreas() == 0) {
    return;
  }
  Builder builder(world);
  if (builder.Run(sTransforms, sYaws, sOverlaps)) {
    sOwner = &world;
  } else {
    sTransforms.clear();
    sYaws.clear();
    sOverlaps.clear();
  }
}

void Release(const CWorld* world) {
  if (world == sOwner) {
    sOwner = nullptr;
    sTransforms.clear();
    sYaws.clear();
    sOverlaps.clear();
  }
}

bool IsActive(const IWorld& world) {
  return sOwner != nullptr && static_cast< const IWorld* >(sOwner) == &world;
}

CTransform4f GetAreaTransform(const IWorld& world, int area) {
  if (!IsActive(world) || area < 0 || area >= static_cast< int >(sTransforms.size())) {
    return CTransform4f::Identity();
  }
  return sTransforms[area];
}

float GetAreaYaw(const IWorld& world, int area) {
  if (!IsActive(world) || area < 0 || area >= static_cast< int >(sYaws.size())) {
    return 0.f;
  }
  return sYaws[area];
}

void DrawOverlaps(const IWorld& world, const std::vector< bool >& drawn,
                  const CTransform4f& modelXf, float alpha) {
  if (!IsActive(world) || sOverlaps.empty() || alpha <= 0.f) {
    return;
  }
  bool setUp = false;
  // Faces first, then the edges over them.
  for (int pass = 0; pass < 2; ++pass) {
    const bool lines = pass == 1;
    bool colorSet = false;
    for (const OverlapPatch& patch : sOverlaps) {
      if (patch.lines != lines || patch.areaA >= static_cast< int >(drawn.size()) ||
          patch.areaB >= static_cast< int >(drawn.size()) || !drawn[patch.areaA] ||
          !drawn[patch.areaB]) {
        continue;
      }
      if (!setUp) {
        // The map's own surface material, in colours of its own.
        CMapArea::CMapAreaSurface::SetupGXMaterial();
        gpRender->SetModelMatrix(modelXf);
        CGraphics::SetCullMode(kCM_None);
        CGraphics::SetLineWidth(1.f, kTO_One);
        setUp = true;
      }
      if (!colorSet) {
        CGX::SetTevKColor(GX_KCOLOR0,
                          CColor(1.f, 1.f, 1.f, (lines ? kOverlapEdgeAlpha : kOverlapAlpha) * alpha)
                              .GetGXColor());
        colorSet = true;
      }
      CGX::SetArray(GX_VA_POS, patch.verts.data(), sizeof(CVector3f),
                    patch.verts.size() * sizeof(CVector3f), TARGET_LITTLE_ENDIAN);
      CGX::Begin(lines ? GX_LINES : GX_TRIANGLES, GX_VTXFMT0,
                 static_cast< ushort >(patch.verts.size()));
      for (size_t v = 0; v < patch.verts.size(); ++v) {
        GXPosition1x8(static_cast< uchar >(v));
      }
      CGX::End();
    }
  }
  if (setUp) {
    // As CAutoMapper::Draw left it.
    CGraphics::SetCullMode(kCM_Front);
  }
}

} // namespace metaforce::maplayout

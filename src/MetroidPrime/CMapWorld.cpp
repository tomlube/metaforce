#include "MetroidPrime/CMapWorld.hpp"

#include "MetroidPrime/CMemoryDrawEnum.hpp"

#include "Kyoto/CFactoryFnReturn.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CVector2f.hpp"
#include "Kyoto/Streams/CInputStream.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorldInfo.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Tweaks/CTweakAutoMapper.hpp"
#include "rstl/algorithm.hpp"
#include "rstl/math.hpp"
#include <float.h>
#include <math.h>
#include <stdlib.h>

#if defined(TARGET_PC)
#include "Metaforce/MapLayout.hpp"
#include "Metaforce/MergedWorld.hpp"
#include "Metaforce/Randomizer/Hooks.hpp"

// The map objects of a room appended from another region keep that region's editor ids, but its
// doors and their visited state go by ids in the loaded world. The map area itself is left alone:
// the same MAPA can be loaded again as a host map. Also marks the doors the randomizer saw the
// player go through.
static CMappableObject GetLoadedMappableObject(const CMapWorld::CMapWorldDrawParms& parms,
                                               const CMapArea& area, int areaIdx, int idx) {
  CMappableObject object = area.GetMappableObject(idx);
  const uint sourceId = object.GetObjId().value;
  CAssetId world = parms.GetWorld().IGetWorldAssetId();
  // Only the loaded world is merged; a world viewed from the map universe is not.
  const CWorld* loaded = parms.GetStateManager().GetWorld();
  if (loaded != nullptr && loaded->GetWorldAssetId() == world) {
    object.SetObjId(TEditorId(metaforce::merged::ToLoadedEditorId(TAreaId(areaIdx), sourceId)));
    world = metaforce::merged::GetSourceWorld(world, TAreaId(areaIdx));
  }
  object.SetTraversed(CMappableObject::IsDoorType(object.GetType()) &&
                      metaforce::randomizer::IsDoorTraversed(world, sourceId));
  // Door lock randomizer: the door's new color. Green for a door gone through still wins.
  int doorType = object.GetType();
  if (CMappableObject::IsDoorType(object.GetType()) &&
      metaforce::randomizer::GetDoorMapType(world, sourceId, doorType)) {
    object.SetType(static_cast< CMappableObject::EMappableObjectType >(doorType));
  }
  return object;
}
#endif

struct CMapObjectSortInfoGreaterThan {
  CMapObjectSortInfoGreaterThan() {}
  bool operator()(const CMapWorld::CMapObjectSortInfo& a,
                  const CMapWorld::CMapObjectSortInfo& b) const {
    if (a.GetObjectCode() == b.GetObjectCode()) {
      return a.GetZDistance() > b.GetZDistance();
    }
    return a.GetObjectCode() > b.GetObjectCode();
  }
};

class Circle2 {
  CVector2f mCenter;
  float mRadius;

public:
  Circle2() : mCenter(CVector2f::Zero()), mRadius(0.f) {}
  CVector2f& Center() { return mCenter; }
  float& Radius() { return mRadius; }
  const CVector2f& GetCenter() const { return mCenter; }
  float GetRadius() const { return mRadius; }
};
CHECK_SIZEOF(Circle2, 0xc)

struct Support {
  int mCount;
  int mIndices[3];

  bool Contains(int idx, CVector2f** points) {
    for (int i = 0; i < mCount; ++i) {
      const CVector2f delta = *points[idx] - *points[mIndices[i]];
      if (delta.MagSquared() < 0.01f) {
        return true;
      }
    }
    return false;
  }
};
CHECK_SIZEOF(Support, 0x10)

static Circle2 MinCircle(int count, const CVector2f* points);

CMapWorld::CMapObjectSortInfo::CMapObjectSortInfo(float zDist, int areaIdx, EObjectCode type,
                                                  int idx, CColor surfColor, CColor outlineColor)
: mZDist(zDist)
, mAreaIdx(areaIdx)
, mTypeAndIdx(static_cast< int >(type) | idx)
, mSurfColor(surfColor)
, mOutlineColor(outlineColor) {}

CMapWorld::CMapAreaData::CMapAreaData(CAssetId areaRes, EMapAreaList list, CMapAreaData* next)
: mAreaRes(areaRes)
, mArea(gpSimplePool->GetObj(SObjectTag('MAPA', areaRes)))
, mList(list)
, mNext(next) {}

void CMapWorld::CMapAreaData::Lock() { mArea.Lock(); }

void CMapWorld::CMapAreaData::Unlock() { mArea.Unlock(); }

bool CMapWorld::CMapAreaData::IsLoaded() const {
  if (!mArea.GetToken().IsLocked()) {
    return false;
  }
  return mArea.TryCache();
}

CMapArea* CMapWorld::CMapAreaData::GetMapArea() const {
  mArea.TryCache();
  return mArea.GetObject();
}

CMapWorld::CMapWorldDrawParms::CMapWorldDrawParms(
    float alphaSurfVisited, float alphaOlVisited, float alphaSurfUnvisited, float alphaOlUnvisited,
    float alpha, const CStateManager& mgr, const CTransform4f& modelXf, const CTransform4f& viewXf,
    const IWorld& wld, const CMapWorldInfo& mwInfo, float outlineWidthScale, bool sortDoorSurfs,
    float playerFlash, float hintFlash, float objectScale)
: mAlphaSurfVisited(alphaSurfVisited)
, mAlphaOlVisited(alphaOlVisited)
, mAlphaSurfUnvisited(alphaSurfUnvisited)
, mAlphaOlUnvisited(alphaOlUnvisited)
, mAlpha(alpha)
, mOutlineWidthScale(outlineWidthScale)
, mMgr(mgr)
, mModelXf(modelXf)
, mViewXf(viewXf)
, mWld(wld)
, mMwInfo(mwInfo)
, mPlayerFlashIntensity(playerFlash)
, mHintFlashIntensity(hintFlash)
, mObjectScale(objectScale)
, mSortDoorSurfs(sortDoorSurfs) {}

CMapWorld::CMapAreaBFSInfo::CMapAreaBFSInfo(int areaIdx, int depth, float surfDepth,
                                            float outlineDepth)
: mAreaIdx(areaIdx)
, mDepth(depth)
, mSurfDrawDepth(surfDepth)
, mOutlineDrawDepth(outlineDepth) {}

CMapWorld::CMapWorld(CInputStream& in)
: mListHeads(3, nullptr)
, mWorldSpherePoint(CVector3f::Zero())
, mWorldSphereRadius(0.f)
, mWorldSphereHalfDepth(0.f) {
  in.ReadLong();
  in.ReadLong();
  int areaCount = in.Get< int >();
  mAreas.reserve(areaCount);
  mTraversed = rstl::vector< bool >(areaCount, false);
  for (int i = 0; i < areaCount; ++i) {
    CAssetId areaRes = in.ReadLong();
    mAreas.push_back(CMapAreaData(areaRes, kMAL_Unloaded, i == 0 ? nullptr : &mAreas[i - 1]));
  }
  mListHeads[kMAL_Unloaded] = &mAreas.back();
  CMemoryDrawEnum::AddWorldMemory(mAreas.capacity() * sizeof(CMapAreaData) + sizeof(*this));
}

CMapWorld::~CMapWorld() {
  CMemoryDrawEnum::SubtractWorldMemory(mAreas.capacity() * sizeof(CMapAreaData) + sizeof(*this));
}

CMapArea* CMapWorld::GetMapArea(int aid) const { return mAreas[aid].GetMapArea(); }

#if defined(TARGET_PC)
void CMapWorld::SetAppendedMapAreas(int ownCount, const rstl::vector< CAssetId >& appended) {
  if (ownCount > mAreas.size()) {
    ownCount = mAreas.size();
  }
  rstl::vector< CAssetId > ids;
  ids.reserve(ownCount + appended.size());
  for (int i = 0; i < ownCount; ++i) {
    ids.push_back(mAreas[i].GetAreaRes());
  }
  for (int i = 0; i < appended.size(); ++i) {
    ids.push_back(appended[i]);
  }
  bool same = ids.size() == mAreas.size();
  for (int i = 0; same && i < ids.size(); ++i) {
    same = ids[i] == mAreas[i].GetAreaRes();
  }
  if (same || mListHeads[kMAL_Loaded] != nullptr || mListHeads[kMAL_Loading] != nullptr) {
    return;
  }
  CMemoryDrawEnum::SubtractWorldMemory(mAreas.capacity() * sizeof(CMapAreaData) + sizeof(*this));
  mAreas.clear();
  mAreas.reserve(ids.size());
  mTraversed = rstl::vector< bool >(ids.size(), false);
  for (int i = 0; i < ids.size(); ++i) {
    mAreas.push_back(CMapAreaData(ids[i], kMAL_Unloaded, i == 0 ? nullptr : &mAreas[i - 1]));
  }
  mListHeads[kMAL_Unloaded] = mAreas.empty() ? nullptr : &mAreas.back();
  CMemoryDrawEnum::AddWorldMemory(mAreas.capacity() * sizeof(CMapAreaData) + sizeof(*this));
}
#endif

bool CMapWorld::IsMapAreaInBFSInfoVector(const CMapAreaData* area,
                                         const rstl::vector< CMapAreaBFSInfo >& vec) const {
  for (AUTO(it, vec.begin()); it != vec.end(); ++it) {
    if (area == &mAreas[it->GetAreaIndex()]) {
      return true;
    }
  }
  return false;
}

void CMapWorld::SetWhichMapAreasLoaded(const IWorld& wld, int start, int count) {
  ClearTraversedFlags();
  rstl::vector< CMapAreaBFSInfo > bfsInfos;
  bfsInfos.reserve(mAreas.size());
  DoBFS(wld, start, count, 9999.f, 9999.f, false, bfsInfos);
  for (int i = 0; i < 2; ++i) {
    CMapAreaData* data = mListHeads[i == 0 ? kMAL_Loaded : kMAL_Loading];
    while (data != nullptr) {
      CMapAreaData* next = data->NextMapAreaData();
      if (!IsMapAreaInBFSInfoVector(data, bfsInfos)) {
        data->Unlock();
        MoveMapAreaToList(data, kMAL_Unloaded);
      }
      data = next;
    }
  }
  for (AUTO(it, bfsInfos.begin()); it != bfsInfos.end(); ++it) {
    CMapAreaData& data = mAreas[it->GetAreaIndex()];
    data.Lock();
    if (data.GetContainingList() == kMAL_Unloaded) {
      MoveMapAreaToList(&data, kMAL_Loading);
    }
  }
}

bool CMapWorld::IsMapAreasStreaming() const {
  bool streaming = false;
  CMapAreaData* data = mListHeads[kMAL_Loading];
  while (data != nullptr) {
    CMapAreaData* next = data->NextMapAreaData();
    if (data->IsLoaded()) {
      const_cast< CMapWorld* >(this)->MoveMapAreaToList(data, kMAL_Loaded);
    } else {
      streaming = true;
    }
    data = next;
  }
  const bool result = streaming;
  return result;
}

void CMapWorld::MoveMapAreaToList(CMapAreaData* data, EMapAreaList list) {
  CMapAreaData* previous = nullptr;
  for (CMapAreaData* current = mListHeads[data->GetContainingList()];;
       previous = current, current = current->NextMapAreaData()) {
    if (current == data) {
      CMapAreaData* next = current->NextMapAreaData();
      if (previous == nullptr) {
        mListHeads[data->GetContainingList()] = next;
      } else {
        previous->SetNextMapArea(next);
      }
      break;
    }
  }
  data->SetNextMapArea(mListHeads[list]);
  data->SetContainingList(list);
  mListHeads[list] = data;
}

int CMapWorld::GetCurrentMapAreaDepth(const IWorld& wld, int aid) const {
  ClearTraversedFlags();
  rstl::vector< CMapAreaBFSInfo > bfsInfos;
  bfsInfos.reserve(mAreas.size());
  DoBFS(wld, aid, 9999, 9999.f, 9999.f, false, bfsInfos);
  if (bfsInfos.empty()) {
    return 0;
  }
  return bfsInfos.back().GetDepth();
}

rstl::vector< int > CMapWorld::GetVisibleAreas(const IWorld& wld,
                                               const CMapWorldInfo& mwInfo) const {
  rstl::vector< int > areas;
  areas.reserve(mAreas.size());
  for (int i = 0; i < mAreas.size(); ++i) {
    if (!IsMapAreaValid(wld, i, true)) {
      continue;
    }
    if (GetMapArea(i)->GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(i),
                                                mwInfo.IsAreaVisible(i))) {
      areas.push_back(i);
    }
  }
  return areas;
}

void CMapWorld::Draw(const CMapWorldDrawParms& parms, int curArea, int otherArea, float depth1,
                     float depth2, bool inMapScreen) const {
  if (depth1 == 0.f && depth2 == 0.f) {
    return;
  }
  ClearTraversedFlags();
  const IWorld& wld = parms.GetWorld();
  int areaDepth = CMath::CeilingF(rstl::max_val(depth1, depth2));
  rstl::vector< CMapAreaBFSInfo > bfsInfos;
  bfsInfos.reserve(mAreas.size());
  if (curArea != otherArea) {
    mTraversed[otherArea] = true;
    DoBFS(wld, curArea, areaDepth, depth1, depth2, true, bfsInfos);
    float lowDepth1 = CMath::CeilingF(depth1 - 1.f);
    float newDepth1 =
        lowDepth1 +
        (depth1 == CMath::FloorF(depth1) ? 0.f : 1.f - static_cast< float >(fmod(depth1, 1.0)));
    float lowDepth2 = CMath::CeilingF(depth2 - 1.f);
    float newDepth2 =
        lowDepth2 +
        (depth2 == CMath::FloorF(depth2) ? 0.f : 1.f - static_cast< float >(fmod(depth2, 1.0)));
    int otherDepth = CMath::CeilingF(rstl::max_val(newDepth1, newDepth2));
    if (wld.IGetAreaAlways(otherArea)->IIsActive()) {
      mTraversed[otherArea] = false;
      DoBFS(wld, otherArea, otherDepth, newDepth1, newDepth2, true, bfsInfos);
    }
  } else {
    DoBFS(wld, curArea, areaDepth, depth1, depth2, true, bfsInfos);
  }
  DrawAreas(parms, curArea, bfsInfos, inMapScreen);
}

void CMapWorld::DoBFS(const IWorld& wld, int startArea, int areaCount, float surfDepth,
                      float outlineDepth, bool checkLoad,
                      rstl::vector< CMapAreaBFSInfo >& bfsInfos) const {
  if (areaCount > 0 && IsMapAreaValid(wld, startArea, checkLoad)) {
    int idx = bfsInfos.size();
    bfsInfos.push_back(CMapAreaBFSInfo(startArea, 1, surfDepth, outlineDepth));
    mTraversed[startArea] = true;
    for (;; ++idx) {
      if (idx == bfsInfos.size()) {
        break;
      }
      int areaIdx = bfsInfos[idx].GetAreaIndex();
      int depth = bfsInfos[idx].GetDepth();
      float surfaceDepth = bfsInfos[idx].GetSurfaceDrawDepth() - 1.f;
      float outlineDepth = bfsInfos[idx].GetOutlineDrawDepth() - 1.f;
      if (depth == areaCount) {
        continue;
      }
      const IGameArea* area = wld.IGetAreaAlways(areaIdx);
      for (int i = 0; i < static_cast< int >(area->IGetNumAttachedAreas()); ++i) {
        int attached = area->IGetAttachedAreaId(i).Value();
        if (IsMapAreaValid(wld, attached, checkLoad) && !mTraversed[attached]) {
          bfsInfos.push_back(CMapAreaBFSInfo(attached, depth + 1, surfaceDepth, outlineDepth));
          mTraversed[attached] = true;
        }
      }
    }
  }
}

bool CMapWorld::IsMapAreaValid(const IWorld& wld, const int areaIdx, bool checkLoad) const {
  if (!wld.IGetAreaAlways(areaIdx)->IIsActive()) {
    return false;
  }
  const CMapArea* area = GetMapArea(areaIdx);
  if (checkLoad && area == nullptr) {
    return false;
  }
  return true;
}

void CMapWorld::DrawAreas(const CMapWorldDrawParms& parms, int selArea,
                          const rstl::vector< CMapAreaBFSInfo >& bfsInfos, bool inMapScreen) const {
  gpRender->SetBlendMode_AlphaBlended();
  CGraphics::SetLineWidth(1.f, kTO_One);
  rstl::vector< CMapObjectSortInfo > sortInfos;
  int surfaceCount = 0;
  int objectCount = 0;
  for (int i = 0; i < bfsInfos.size(); ++i) {
    const CMapArea* area = GetMapArea(bfsInfos[i].GetAreaIndex());
    surfaceCount += area->GetNumSurfaces();
    objectCount += area->GetNumMappableObjects();
  }
  sortInfos.reserve(surfaceCount + objectCount +
                    (parms.GetIsSortDoorSurfaces() ? objectCount * 6 : 0));
  const CStateManager& mgr = parms.GetStateManager();
  const CMapWorldInfo& mwInfo = parms.GetMapWorldInfo();
  int playerArea = mgr.GetNextAreaId().Value();
#if defined(TARGET_PC)
  std::vector< bool > drawn(mAreas.size(), false);
#endif
  for (int i = 0; i < bfsInfos.size(); ++i) {
    const CMapAreaBFSInfo& bfsInfo = bfsInfos[i];
    int areaIdx = bfsInfo.GetAreaIndex();
    CMapArea* area = GetMapArea(areaIdx);
    if (!area->GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(areaIdx),
                                        mwInfo.IsAreaVisible(areaIdx))) {
      continue;
    }
#if defined(TARGET_PC)
    drawn[areaIdx] = true;
#endif
    const float surfaceDepth = bfsInfo.GetSurfaceDrawDepth();
    const float outlineDepth = bfsInfo.GetOutlineDrawDepth();
    float alphaSurfVisited = parms.GetAlphaSurfaceVisited();
    float alphaOutlineVisited = parms.GetAlphaOutlineVisited();
    float alphaSurfUnvisited = parms.GetAlphaSurfaceUnvisited();
    float alphaOutlineUnvisited = parms.GetAlphaOutlineUnvisited();
    const CTransform4f& cameraXf = parms.GetCameraTransform();
    float surfaceFrac = surfaceDepth - CMath::FloorF(surfaceDepth);
    float surfaceAlpha = surfaceDepth >= 1.f ? 1.f : surfaceDepth >= 0.f ? surfaceFrac : 0.f;
    float outlineFrac = outlineDepth - CMath::FloorF(outlineDepth);
    float outlineAlpha = outlineDepth >= 1.f ? 1.f : outlineDepth >= 0.f ? outlineFrac : 0.f;
    bool visited = mwInfo.IsAreaVisited(areaIdx);
    float alphaSurf = visited ? alphaSurfVisited : alphaSurfUnvisited;
    float alphaOutline = visited ? alphaOutlineVisited : alphaOutlineUnvisited;
    CColor surfaceColor = visited ? gpTweakAutoMapper->mSurfColorVisited
                                  : gpTweakAutoMapper->mSurfColorUnvisited;
    CColor outlineColor = visited ? gpTweakAutoMapper->mOutlineColorVisited
                                  : gpTweakAutoMapper->mOutlineColorUnvisited;
    CColor surfaceSelect = visited ? gpTweakAutoMapper->mSurfaceSelectColorVisited
                                   : gpTweakAutoMapper->mSurfaceSelectColorUnvisited;
    CColor outlineSelect = visited ? gpTweakAutoMapper->mOutlineSelectColorVisited
                                   : gpTweakAutoMapper->mOutlineSelectColorUnvisited;
    CColor hintFlash =
        CColor::Lerp(CColor(0u), CColor(uchar(255), uchar(255), uchar(255), uchar(0)),
                     parms.GetHintAreaFlashIntensity());
    CColor finalSurface = areaIdx == selArea && inMapScreen
                              ? CColor::Add(surfaceSelect, hintFlash)
                              : surfaceColor.WithAlphaOf(surfaceAlpha * alphaSurf);
    CColor finalOutline = areaIdx == selArea && inMapScreen
                              ? CColor::Add(outlineSelect, hintFlash)
                              : outlineColor.WithAlphaOf(outlineAlpha * alphaOutline);
    if ((selArea != playerArea || parms.GetHintAreaFlashIntensity() == 0.f) &&
        playerArea == areaIdx && this == mgr.GetWorld()->GetMapWorld()) {
      float pulse = parms.GetPlayerAreaFlashIntensity();
      const CColor& flashColor = gpTweakAutoMapper->mAreaFlashPulseColor;
      finalSurface = CColor::Lerp(finalSurface, flashColor, pulse);
      finalOutline = CColor::Lerp(finalOutline, flashColor, pulse);
    }
    const CTransform4f modelView =
        cameraXf.GetQuickInverse() * area->GetAreaPostTransform(parms.GetWorld(), areaIdx);
#if defined(TARGET_PC)
    // Map objects are in world space, not the area's own.
    const CTransform4f objectView =
        cameraXf.GetQuickInverse() * CMapArea::GetAreaMapTransform(parms.GetWorld(), areaIdx);
#endif
    int j;
    int count = area->GetNumSurfaces();
    for (j = 0; j < count; ++j) {
      const CMapArea::CMapAreaSurface& surface = area->GetSurface(j);
      const CVector3f center = surface.GetCenterPosition();
      const CVector3f pos = modelView * center;
      sortInfos.push_back(CMapObjectSortInfo(pos.GetY(), areaIdx, CMapObjectSortInfo::kOC_Surface,
                                             j, finalSurface, finalOutline));
    }
    int surfaceBase = 0;
    for (j = 0; j < area->GetNumMappableObjects(); surfaceBase += 6, ++j) {
#if defined(TARGET_PC)
      const CMappableObject object = GetLoadedMappableObject(parms, *area, areaIdx, j);
#else
      const CMappableObject& object = area->GetMappableObject(j);
#endif
      if (!object.GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(areaIdx), mwInfo)) {
        continue;
      }
      bool door = CMappableObject::IsDoorType(object.GetType());
      if (door) {
        if (!mwInfo.IsAreaVisible(areaIdx)) {
          continue;
        }
        if (parms.GetIsSortDoorSurfaces()) {
          int objectFace, face;
          for (face = 0, objectFace = surfaceBase; face < 6; ++face, ++objectFace) {
            const CVector3f& center = object.BuildSurfaceCenterPoint(face);
#if defined(TARGET_PC)
            const CVector3f pos = objectView * center;
#else
            const CVector3f translated =
                CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx) + center;
            const CVector3f pos = modelView * translated;
#endif
            sortInfos.push_back(CMapObjectSortInfo(pos.GetY(), areaIdx,
                                                   CMapObjectSortInfo::kOC_DoorSurface, objectFace,
                                                   CColor(), CColor()));
          }
          continue;
        }
      }
      const CVector3f origin = object.GetTransform().GetTranslation();
#if defined(TARGET_PC)
      const CVector3f pos = objectView * origin;
#else
      const CVector3f translated =
          CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx) + origin;
      const CVector3f pos = modelView * translated;
#endif
      CMapObjectSortInfo::EObjectCode code =
          door ? CMapObjectSortInfo::kOC_Door : CMapObjectSortInfo::kOC_Object;
      sortInfos.push_back(CMapObjectSortInfo(pos.GetY(), areaIdx, code, j, CColor(), CColor()));
    }
  }

  const CTransform4f& modelXf = parms.GetPlaneProjectionTransform();
  const CTransform4f& cameraXf = parms.GetCameraTransform();
  float outlineWidth = parms.GetOutlineWidthScale();
  if (sortInfos.size() > 0) {
    rstl::sort(sortInfos.begin(), sortInfos.end(), CMapObjectSortInfoGreaterThan());
    enum EDrawMode { kDM_Invalid = -1, kDM_Surface, kDM_Door, kDM_DoorSurface, kDM_Object };
    EDrawMode lastMode = kDM_Invalid;
    int lastArea = -1;
    CMapArea::CMapAreaSurface::SetupGXMaterial();
    for (int i = 0; i < sortInfos.size(); ++i) {
      const CMapObjectSortInfo& info = sortInfos[i];
      CMapObjectSortInfo::EObjectCode type = info.GetObjectCode();
      int areaIdx = info.GetAreaIndex();
      int idx = info.GetLocalObjectIndex();
      const CColor& surfaceColor = info.GetSurfaceColor();
      const CColor& outlineColor = info.GetOutlineColor();
      CMapArea* area = GetMapArea(areaIdx);
      const CTransform4f& areaXf = area->GetAreaPostTransform(parms.GetWorld(), areaIdx);
      if (type == CMapObjectSortInfo::kOC_Surface) {
        const CMapArea::CMapAreaSurface& surface = area->GetSurface(idx);
        float linear = gpTweakAutoMapper->mMapSurfaceNormColorLinear;
        float constant = gpTweakAutoMapper->mMapSurfaceNormColorConstant;
        float shade =
            linear * rstl::max_val(0.f, CVector3f::Dot(-1.f * cameraXf.GetForward(),
                                                       areaXf.Rotate(surface.GetNormal())));
        shade = constant + shade;
        const CColor normalColor(shade, shade, shade, 1.f);
        const CColor color = CColor::Modulate(surfaceColor, normalColor);
        bool needsVertices = lastArea != areaIdx || lastMode != kDM_Surface;
        if (needsVertices) {
          gpRender->SetModelMatrix(modelXf * areaXf);
        }
        TMapVertices vertices = needsVertices ? area->GetVertices() : TMapVertices();
        surface.Draw(vertices, color, outlineColor, outlineWidth);
        lastMode = kDM_Surface;
      } else if (type == CMapObjectSortInfo::kOC_Door || type == CMapObjectSortInfo::kOC_Object) {
        EDrawMode mode = type == CMapObjectSortInfo::kOC_Door ? kDM_Door : kDM_Object;
#if defined(TARGET_PC)
        const CMappableObject object = GetLoadedMappableObject(parms, *area, areaIdx, idx);
#else
        const CMappableObject& object = area->GetMappableObject(idx);
#endif
        bool needsVertices = lastMode != mode;
#if defined(TARGET_PC)
        // Doors turn with their room; icons only move with it, and face the camera.
        const CTransform4f mapXf = CMapArea::GetAreaMapTransform(parms.GetWorld(), areaIdx);
        const CTransform4f objXf =
            type == CMapObjectSortInfo::kOC_Door
                ? mapXf * object.GetTransform()
                : CTransform4f::Translate(mapXf * object.GetTransform().GetTranslation()) *
                      object.GetTransform().GetRotation();
#else
        const CTransform4f objXf =
            CTransform4f::Translate(CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx)) *
            object.GetTransform();
#endif
        gpRender->SetModelMatrix(
            type == CMapObjectSortInfo::kOC_Door
                ? modelXf * objXf
                : modelXf * objXf *
                      CTransform4f(cameraXf.BuildMatrix3f() *
                                       CMatrix3f::Scale(parms.GetObjectScale()),
                                   CVector3f::Zero()));
        object.Draw(selArea, mwInfo, parms.GetAlpha(), needsVertices);
        lastMode = mode;
      } else if (type == CMapObjectSortInfo::kOC_DoorSurface) {
#if defined(TARGET_PC)
        const CMappableObject object = GetLoadedMappableObject(parms, *area, areaIdx, idx / 6);
#else
        const CMappableObject& object = area->GetMappableObject(idx / 6);
#endif
#if defined(TARGET_PC)
        gpRender->SetModelMatrix(modelXf *
                                 CMapArea::GetAreaMapTransform(parms.GetWorld(), areaIdx) *
                                 object.GetTransform());
#else
        gpRender->SetModelMatrix(
            modelXf *
            CTransform4f::Translate(CMapArea::GetAreaPostTranslate(parms.GetWorld(), areaIdx)) *
            object.GetTransform());
#endif
        object.DrawDoorSurface(selArea, mwInfo, parms.GetAlpha(), idx % 6,
                               lastMode != kDM_DoorSurface);
        lastMode = kDM_DoorSurface;
      }
      lastArea = areaIdx;
    }
  }
#if defined(TARGET_PC)
  // Room randomizer: where rooms the map had to lay over each other overlap.
  metaforce::maplayout::DrawOverlaps(parms.GetWorld(), drawn, modelXf, parms.GetAlpha());
#endif
}

void CMapWorld::RecalculateWorldSphere(const CMapWorldInfo& mwInfo, const IWorld& wld) const {
  rstl::vector< CVector2f > coords;
  coords.reserve(mAreas.size() * 8);
  float zMin = FLT_MAX;
  float zMax = -FLT_MAX;
  for (int i = 0; i < mAreas.size(); ++i) {
    if (IsMapAreaValid(wld, i, true)) {
      CMapArea* area = GetMapArea(i);
      if (area->GetIsVisibleToAutoMapper(mwInfo.IsWorldVisible(i), mwInfo.IsAreaVisible(i))) {
        CAABox box = area->GetBoundingBox().GetTransformedAABox(area->GetAreaPostTransform(wld, i));
        for (int j = 0; j < 8; ++j) {
          const CVector3f point = box.GetPoint(j);
          coords.push_back(CVector2f(point.GetX(), point.GetY()));
          zMin = rstl::min_val(zMin, point.GetZ());
          zMax = rstl::max_val(zMax, point.GetZ());
        }
      }
    }
  }
  const Circle2 circle = MinCircle(coords.size(), coords.data());
  mWorldSphereRadius = circle.GetRadius();
  mWorldSpherePoint =
      CVector3f(circle.GetCenter().GetX(), circle.GetCenter().GetY(), 0.5f * (zMax + zMin));
  mWorldSphereHalfDepth = 0.5f * (zMax - zMin);
}

CVector3f CMapWorld::ConstrainToWorldVolume(const CVector3f& point,
                                            const CVector3f& lookVec) const {
  CVector3f result = point;
  if (CMath::AbsF(lookVec.GetZ()) > FLT_EPSILON) {
    float above = point.GetZ() - (mWorldSphereHalfDepth + mWorldSpherePoint.GetZ());
    float below = point.GetZ() - (mWorldSpherePoint.GetZ() - mWorldSphereHalfDepth);
    if (above > 0.f) {
      result = point + (-above / lookVec.GetZ()) * lookVec;
    } else if (below < 0.f) {
      result = point + (-below / lookVec.GetZ()) * lookVec;
    }
  } else {
    result.SetZ(CMath::Clamp(mWorldSpherePoint.GetZ() - mWorldSphereHalfDepth, result.GetZ(),
                             mWorldSphereHalfDepth + mWorldSpherePoint.GetZ()));
  }
  const CVector2f delta = CVector2f(point.GetX(), point.GetY()) -
                          CVector2f(mWorldSpherePoint.GetX(), mWorldSpherePoint.GetY());
  if (delta.Magnitude() > mWorldSphereRadius) {
    const CVector2f offset = delta.AsNormalized() * mWorldSphereRadius;
    result.SetX(mWorldSpherePoint.GetX() + offset.GetX());
    result.SetY(mWorldSpherePoint.GetY() + offset.GetY());
  }
  return result;
}

void CMapWorld::ClearTraversedFlags() const {
  for (int i = 0; i < mTraversed.size(); ++i) {
    mTraversed[i] = false;
  }
}

const CFactoryFnReturn FMapWorldFactory(const SObjectTag& tag, CInputStream& in,
                                  const CVParamTransfer& param) {
  return CFactoryFnReturn(rs_new CMapWorld(in));
}

static bool PointInsideCircle(const CVector2f& point, const Circle2& circle, float& distance) {
  const CVector2f delta = point - circle.GetCenter();
  float distanceSquared = delta.MagSquared();
  distance = distanceSquared - circle.GetRadius();
  return distance <= 0.f;
}

static Circle2 ExactCircle1(const CVector2f& point) {
  Circle2 result;
  result.Center() = point;
  result.Radius() = 0.f;
  return result;
}

static Circle2 ExactCircle2(const CVector2f& a, const CVector2f& b) {
  Circle2 result;
  result.Center() = 0.5f * (a + b);
  const CVector2f delta = b - a;
  result.Radius() = 0.25f * delta.MagSquared();
  return result;
}

static Circle2 ExactCircle3(const CVector2f& a, const CVector2f& b, const CVector2f& c) {
  const CVector2f d1 = b - a;
  const CVector2f d2 = c - a;
  const float x1 = d1.GetX();
  const float y1 = d1.GetY();
  const float x2 = d2.GetX();
  const float y2 = d2.GetY();
  const float halfMag1 = 0.5f * d1.MagSquared();
  const float halfMag2 = 0.5f * d2.MagSquared();
  const float determinant = x1 * y2 - y1 * x2;
  Circle2 result;
  if (CMath::AbsF(determinant) > 0.01f) {
    const float inverse = 1.f / determinant;
    CVector2f offset = CVector2f::Zero();
    offset[0] = inverse * (y2 * halfMag1 - y1 * halfMag2);
    offset[1] = inverse * (x1 * halfMag2 - x2 * halfMag1);
    result.Center() = a + offset;
    result.Radius() = offset.MagSquared();
  } else {
    result.Center() = CVector2f::Zero();
    result.Radius() = FLT_MAX;
  }
  return result;
}

static Circle2 UpdateSupport1(int idx, CVector2f** points, Support& support) {
  Circle2 result = ExactCircle2(*points[support.mIndices[0]], *points[idx]);
  support.mCount = 2;
  support.mIndices[1] = idx;
  return result;
}

static Circle2 UpdateSupport2(int idx, CVector2f** points, Support& support) {
  const CVector2f& a = *points[support.mIndices[0]];
  const CVector2f& b = *points[support.mIndices[1]];
  const CVector2f& point = *points[idx];
  Circle2 circles[3];
  float distance;
  int best = -1;
  float minRadius = FLT_MAX;
  circles[0] = ExactCircle2(a, point);
  if (PointInsideCircle(b, circles[0], distance)) {
    minRadius = circles[0].GetRadius();
    best = 0;
  }
  circles[1] = ExactCircle2(b, point);
  if (circles[1].GetRadius() < minRadius && PointInsideCircle(a, circles[1], distance)) {
    best = 1;
  }
  Circle2 result;
  if (best != -1) {
    result = circles[best];
    support.mIndices[1 - best] = idx;
  } else {
    result = ExactCircle3(a, b, point);
    support.mCount = 3;
    support.mIndices[2] = idx;
  }
  return result;
}

static Circle2 UpdateSupport3(int idx, CVector2f** points, Support& support) {
  const CVector2f& a = *points[support.mIndices[0]];
  const CVector2f& b = *points[support.mIndices[1]];
  const CVector2f& c = *points[support.mIndices[2]];
  const CVector2f& point = *points[idx];
  Circle2 circles[6];
  float distance;
  int best = -1;
  int fallback = -1;
  float minRadius = FLT_MAX;
  float minDistance = FLT_MAX;

  circles[0] = ExactCircle2(a, point);
  if (PointInsideCircle(b, circles[0], distance)) {
    if (PointInsideCircle(c, circles[0], distance)) {
      minRadius = circles[0].GetRadius();
      best = 0;
    } else {
      minDistance = distance;
      fallback = 0;
    }
  } else {
    minDistance = distance;
    fallback = 0;
  }

  circles[1] = ExactCircle2(b, point);
  if (circles[1].GetRadius() < minRadius) {
    if (PointInsideCircle(a, circles[1], distance)) {
      if (PointInsideCircle(c, circles[1], distance)) {
        minRadius = circles[1].GetRadius();
        best = 1;
      } else if (distance < minDistance) {
        minDistance = distance;
        fallback = 1;
      }
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 1;
    }
  }

  circles[2] = ExactCircle2(c, point);
  if (circles[2].GetRadius() < minRadius) {
    if (PointInsideCircle(a, circles[2], distance)) {
      if (PointInsideCircle(b, circles[2], distance)) {
        minRadius = circles[2].GetRadius();
        best = 2;
      } else if (distance < minDistance) {
        minDistance = distance;
        fallback = 2;
      }
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 2;
    }
  }

  circles[3] = ExactCircle3(a, b, point);
  if (circles[3].GetRadius() < minRadius) {
    if (PointInsideCircle(c, circles[3], distance)) {
      minRadius = circles[3].GetRadius();
      best = 3;
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 3;
    }
  }

  circles[4] = ExactCircle3(a, c, point);
  if (circles[4].GetRadius() < minRadius) {
    if (PointInsideCircle(b, circles[4], distance)) {
      minRadius = circles[4].GetRadius();
      best = 4;
    } else if (distance < minDistance) {
      minDistance = distance;
      fallback = 4;
    }
  }

  circles[5] = ExactCircle3(b, c, point);
  if (circles[5].GetRadius() < minRadius) {
    if (PointInsideCircle(a, circles[5], distance)) {
      best = 5;
    } else if (distance < minDistance) {
      fallback = 5;
    }
  }

  if (best == -1) {
    best = fallback;
  }
  Circle2 result = circles[best];
  switch (best) {
  case 0:
    support.mCount = 2;
    support.mIndices[1] = idx;
    break;
  case 1:
    support.mCount = 2;
    support.mIndices[0] = idx;
    break;
  case 2:
    support.mCount = 2;
    support.mIndices[0] = support.mIndices[2];
    support.mIndices[1] = idx;
    break;
  case 3:
    support.mIndices[2] = idx;
    break;
  case 4:
    support.mIndices[1] = idx;
    break;
  case 5:
    support.mIndices[0] = idx;
    break;
  }
  return result;
}

static Circle2 (*gs_aoUpdate[])(int, CVector2f**, Support&) = {nullptr, UpdateSupport1,
                                                               UpdateSupport2, UpdateSupport3};

static Circle2 MinCircle(int count, const CVector2f* points) {
  Circle2 result;
  if (count >= 1) {
    CVector2f** shuffled = rs_new CVector2f* [count];
    int i;
    for (i = 0; i < count; ++i) {
      shuffled[i] = const_cast< CVector2f* >(&points[i]);
    }
    for (i = count - 1; i > 0; --i) {
      int j = rand() % (i + 1);
      if (j != i) {
        rstl::swap(shuffled[i], shuffled[j]);
      }
    }
    result = ExactCircle1(*shuffled[0]);
    Support support;
    support.mCount = 1;
    support.mIndices[0] = 0;
    i = 1;
    while (i < count) {
      if (!support.Contains(i, shuffled)) {
        float distance;
        if (!PointInsideCircle(*shuffled[i], result, distance)) {
          Circle2 next = gs_aoUpdate[support.mCount](i, shuffled, support);
          if (next.GetRadius() > result.GetRadius()) {
            result = next;
            i = 0;
            continue;
          }
        }
      }
      ++i;
    }
    if (shuffled != nullptr) {
      delete[] shuffled;
    }
  }
  result.Radius() = CMath::SqrtF(result.GetRadius());
  return result;
}

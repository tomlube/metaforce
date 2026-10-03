#include "Metaforce/MenuPointer.hpp"

#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiModel.hpp"
#include "GuiSys/CGuiPane.hpp"
#include "GuiSys/CGuiTableGroup.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Math/CAABox.hpp"

#include <aurora/aurora.h>
#include <aurora/input.hpp>
#include <dolphin/gx/GXAurora.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace metaforce::menu_pointer {
namespace {

using aurora::input::InputEvent;
using aurora::input::InputSource;

// A frame's camera as last drawn, mapping world space to logical framebuffer pixels.
struct FrameView {
  const CGuiFrame* frame;
  CTransform4f worldToView;
  bool perspective;
  float left;
  float right;
  float top;
  float bottom;
  float znear;
  float vpLeft;
  float vpTop;
  float vpWidth;
  float vpHeight;
  unsigned int drawnFrame;
};

struct Rect {
  float minX;
  float minY;
  float maxX;
  float maxY;
  bool valid;

  void Add(float x, float y) {
    if (!valid) {
      minX = maxX = x;
      minY = maxY = y;
      valid = true;
      return;
    }
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
  }
  bool Contains(float x, float y) const {
    return valid && x >= minX && x <= maxX && y >= minY && y <= maxY;
  }
  float Area() const { return (maxX - minX) * (maxY - minY); }
};

// A frame drawn this many game frames ago still takes clicks.
constexpr unsigned int kMaxViewAge = 2;

std::vector< FrameView > sViews;
unsigned int sFrameIndex = 0;

// Live state, written by input events.
bool sHasPosition = false;
float sWindowX = 0.f;
float sWindowY = 0.f;
bool sPendingMove = false;
bool sPendingLeft = false;
bool sPendingRight = false;
bool sLeftDown = false;
float sPendingScroll = 0.f;

// Latched for the current game frame.
bool sCursorValid = false;
float sCursorX = 0.f; // Logical framebuffer pixels
float sCursorY = 0.f;
bool sMoved = false;
bool sLeftPress = false;
bool sRightPress = false;
bool sLeftHeld = false;
int sScrollSteps = 0;
const CGuiWidget* sDragOwner = nullptr;

// Window coordinates to the logical framebuffer the game draws into. Aurora draws the content
// framebuffer centered in the window at the largest size that keeps its aspect.
bool WindowToLogical(float x, float y, float& outX, float& outY) {
  SDL_Window* window = aurora_get_window();
  int windowWidth = 0;
  int windowHeight = 0;
  if (window == nullptr || !SDL_GetWindowSize(window, &windowWidth, &windowHeight) ||
      windowWidth <= 0 || windowHeight <= 0) {
    return false;
  }
  u32 renderWidth = 0;
  u32 renderHeight = 0;
  AuroraGetRenderSize(&renderWidth, &renderHeight);
  if (renderWidth == 0 || renderHeight == 0) {
    return false;
  }
  const float scale =
      std::min(static_cast< float >(windowWidth) / static_cast< float >(renderWidth),
               static_cast< float >(windowHeight) / static_cast< float >(renderHeight));
  const float width = static_cast< float >(renderWidth) * scale;
  const float height = static_cast< float >(renderHeight) * scale;
  const float left = (static_cast< float >(windowWidth) - width) * 0.5f;
  const float top = (static_cast< float >(windowHeight) - height) * 0.5f;

  const GXRenderModeObj& mode = CGraphics::GetRenderMode();
  const float logicalWidth = mode.fbWidth ? static_cast< float >(mode.fbWidth) : 640.f;
  const float logicalHeight = mode.efbHeight ? static_cast< float >(mode.efbHeight) : 448.f;
  outX = (x - left) / width * logicalWidth;
  outY = (y - top) / height * logicalHeight;
  return true;
}

const FrameView* FindView(const CGuiFrame* frame) {
  for (const FrameView& view : sViews) {
    if (view.frame == frame) {
      return sFrameIndex - view.drawnFrame <= kMaxViewAge ? &view : nullptr;
    }
  }
  return nullptr;
}

bool Project(const FrameView& view, const CVector3f& world, float& x, float& y) {
  const CVector3f local = view.worldToView * world;
  float px = local.GetX();
  float pz = local.GetZ();
  if (view.perspective) {
    // The camera looks down +Y.
    if (local.GetY() <= 0.001f) {
      return false;
    }
    const float scale = view.znear / local.GetY();
    px *= scale;
    pz *= scale;
  }
  const float ndcX = (2.f * px - view.right - view.left) / (view.right - view.left);
  const float ndcY = (2.f * pz - view.top - view.bottom) / (view.top - view.bottom);
  x = view.vpLeft + (ndcX + 1.f) * 0.5f * view.vpWidth;
  y = view.vpTop + (1.f - ndcY) * 0.5f * view.vpHeight;
  return true;
}

void AddPoint(const FrameView& view, const CTransform4f& xf, const CVector3f& point, Rect& rect) {
  float x;
  float y;
  if (Project(view, xf * point, x, y)) {
    rect.Add(x, y);
  }
}

// The screen rect of what the widget itself draws: a model's bounds or a pane's quad.
Rect WidgetRect(const FrameView& view, const CGuiWidget& widget) {
  Rect rect = {};
  if (!widget.GetIsVisible() || widget.GetModifiedColor().GetAlphau8() == 0) {
    return rect;
  }
  const FourCC type = widget.GetWidgetTypeID();
  if (type == 'MODL') {
    const auto& token = static_cast< const CGuiModel& >(widget).GetModel();
    const CModel* model = token.valid() ? token->GetObject() : nullptr;
    if (model == nullptr) {
      return rect;
    }
    const CAABox& box = model->GetBoundingBox();
    const CVector3f& lo = box.GetMinPoint();
    const CVector3f& hi = box.GetMaxPoint();
    const CTransform4f& xf = widget.GetWorldTransform();
    for (int i = 0; i < 8; ++i) {
      AddPoint(view, xf,
               CVector3f(i & 1 ? hi.GetX() : lo.GetX(), i & 2 ? hi.GetY() : lo.GetY(),
                         i & 4 ? hi.GetZ() : lo.GetZ()),
               rect);
    }
  } else if (type == 'PANE' || type == 'TXPN') {
    const CGuiPane& pane = static_cast< const CGuiPane& >(widget);
    const float* points = pane.GetVtxBuf();
    if (points == nullptr) {
      return rect;
    }
    // As CGuiPane::Draw places it.
    const CTransform4f xf =
        widget.GetWorldTransform() * CTransform4f::Translate(pane.GetScaleCenter());
    for (int i = 0; i < pane.GetPointCount(); ++i) {
      AddPoint(view, xf, CVector3f(points[i * 3], points[i * 3 + 1], points[i * 3 + 2]), rect);
    }
  }
  return rect;
}

template < typename Fn >
void ForEachRect(const FrameView& view, const CGuiWidget& widget, Fn&& fn) {
  const Rect rect = WidgetRect(view, widget);
  if (rect.valid) {
    fn(rect);
  }
  for (const CGuiObject* child = widget.GetChildObject(); child != nullptr;
       child = child->GetNextSibling()) {
    ForEachRect(view, *static_cast< const CGuiWidget* >(child), fn);
  }
}

// The area of the smallest piece of the widget under the cursor, or a negative value.
float HitArea(const FrameView& view, const CGuiWidget& widget) {
  float best = -1.f;
  ForEachRect(view, widget, [&](const Rect& rect) {
    if (rect.Contains(sCursorX, sCursorY) && (best < 0.f || rect.Area() < best)) {
      best = rect.Area();
    }
  });
  return best;
}

Rect Bounds(const FrameView& view, const CGuiWidget& widget) {
  Rect bounds = {};
  ForEachRect(view, widget, [&](const Rect& rect) {
    bounds.Add(rect.minX, rect.minY);
    bounds.Add(rect.maxX, rect.maxY);
  });
  return bounds;
}

} // namespace

void HandleInputEvent(const InputEvent& event) {
  if (const auto* pointer = event.payload.get_if< InputEvent::PointerChanged >()) {
    const bool touch = event.source.kind == InputSource::Kind::Touch;
    if (!touch && event.source.kind != InputSource::Kind::Mouse) {
      return;
    }
    switch (pointer->phase) {
    case InputEvent::PointerChanged::Phase::Move:
      sPendingMove = true;
      break;
    case InputEvent::PointerChanged::Phase::Down:
      if (touch || pointer->button == SDL_BUTTON_LEFT) {
        sPendingMove = true;
        sPendingLeft = true;
        sLeftDown = true;
      } else if (pointer->button == SDL_BUTTON_RIGHT) {
        sPendingRight = true;
      }
      break;
    case InputEvent::PointerChanged::Phase::Up:
      if (touch || pointer->button == SDL_BUTTON_LEFT) {
        sLeftDown = false;
      }
      break;
    case InputEvent::PointerChanged::Phase::Cancel:
      sLeftDown = false;
      break;
    }
    sHasPosition = true;
    sWindowX = pointer->position.x;
    sWindowY = pointer->position.y;
  } else if (const auto* scroll = event.payload.get_if< InputEvent::Scroll >()) {
    sPendingScroll += scroll->delta.y;
  } else if (event.payload.is< InputEvent::Cancelled >()) {
    sLeftDown = false;
  }
}

void Reset() {
  sPendingMove = false;
  sPendingLeft = false;
  sPendingRight = false;
  sLeftDown = false;
  sPendingScroll = 0.f;
}

void BeginFrame() {
  ++sFrameIndex;
  sCursorValid = sHasPosition && WindowToLogical(sWindowX, sWindowY, sCursorX, sCursorY);
  sMoved = sPendingMove;
  sLeftPress = sPendingLeft;
  sRightPress = sPendingRight;
  sLeftHeld = sLeftDown;
  // Whole steps only; the rest carries over so smooth wheels and trackpads still step.
  sScrollSteps = static_cast< int >(sPendingScroll);
  sPendingScroll -= static_cast< float >(sScrollSteps);
  sPendingMove = false;
  sPendingLeft = false;
  sPendingRight = false;
  if (!sLeftHeld && !sLeftPress) {
    sDragOwner = nullptr;
  }
}

void RecordFrameView(const CGuiFrame* frame, float alpha) {
  if (alpha <= 0.f) {
    // Faded out: leave the last view to age out.
    return;
  }
  const CGraphics::CProjectionState& proj = CGraphics::GetProjectionState();
  const CViewport& viewport = CGraphics::GetViewport();
  const FrameView view = {
      frame,
      CGraphics::GetViewMatrix().GetInverse(),
      proj.IsPerspective(),
      proj.GetLeft(),
      proj.GetRight(),
      proj.GetTop(),
      proj.GetBottom(),
      proj.GetNear(),
      static_cast< float >(viewport.mLeft),
      static_cast< float >(viewport.mTop),
      static_cast< float >(viewport.mWidth),
      static_cast< float >(viewport.mHeight),
      sFrameIndex,
  };
  const auto it = std::find_if(sViews.begin(), sViews.end(),
                               [frame](const FrameView& other) { return other.frame == frame; });
  if (it == sViews.end()) {
    sViews.push_back(view);
  } else {
    *it = view;
  }
}

void ForgetFrame(const CGuiFrame* frame) {
  sViews.erase(std::remove_if(sViews.begin(), sViews.end(),
                              [frame](const FrameView& view) { return view.frame == frame; }),
               sViews.end());
}

bool Moved() { return sMoved && sCursorValid; }

bool LeftPressed() { return sLeftPress && sCursorValid; }

bool ConsumeLeftPress() {
  const bool pressed = LeftPressed();
  sLeftPress = false;
  return pressed;
}

bool ConsumeRightPress() {
  const bool pressed = sRightPress;
  sRightPress = false;
  return pressed;
}

int ConsumeScrollSteps() {
  const int steps = sScrollSteps;
  sScrollSteps = 0;
  return steps;
}

int HitTableWorker(CGuiTableGroup& table) {
  if (!sCursorValid) {
    return -1;
  }
  const FrameView* view = FindView(table.GetParentFrame());
  if (view == nullptr) {
    return -1;
  }
  int hit = -1;
  float hitArea = 0.f;
  for (int i = 0; i < table.GetElementCount(); ++i) {
    const CGuiWidget* worker = table.GetWorkerWidget(i);
    if (worker == nullptr || !table.IsWorkerSelectable(i)) {
      continue;
    }
    // Overlapping rows go to the one with the smaller piece under the cursor.
    const float area = HitArea(*view, *worker);
    if (area >= 0.f && (hit < 0 || area < hitArea)) {
      hit = i;
      hitArea = area;
    }
  }
  return hit;
}

bool DragAlong(const CGuiWidget& owner, const CGuiWidget& handle, const CVector3f& from,
               const CVector3f& to, float& t) {
  const FrameView* view = FindView(owner.GetParentFrame());
  float ax;
  float ay;
  float bx;
  float by;
  if (!sCursorValid || view == nullptr || !Project(*view, from, ax, ay) ||
      !Project(*view, to, bx, by)) {
    if (sDragOwner == &owner) {
      sDragOwner = nullptr;
    }
    return false;
  }
  const float dx = bx - ax;
  const float dy = by - ay;
  const float length2 = dx * dx + dy * dy;
  if (length2 < 1.f) {
    return false;
  }
  const float along = ((sCursorX - ax) * dx + (sCursorY - ay) * dy) / length2;

  if (sDragOwner != &owner) {
    if (!sLeftPress) {
      return false;
    }
    const Rect handleRect = Bounds(*view, handle);
    bool grabbed = handleRect.Contains(sCursorX, sCursorY);
    if (!grabbed && handleRect.valid && along >= 0.f && along <= 1.f) {
      // Anywhere on the track, within the handle's height of it.
      const float across =
          std::fabs((sCursorX - ax) * dy - (sCursorY - ay) * dx) / std::sqrt(length2);
      grabbed = across <= (handleRect.maxY - handleRect.minY) * 0.5f;
    }
    if (!grabbed) {
      return false;
    }
    sLeftPress = false;
    sDragOwner = &owner;
  }
  t = std::clamp(along, 0.f, 1.f);
  return true;
}

} // namespace metaforce::menu_pointer

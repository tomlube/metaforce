#include "Metaforce/SaveIndicator.hpp"

#include "Metaforce/Display.hpp"

#include "GuiSys/CGuiTextSupport.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector2f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/Tweaks/CTweakGuiColors.hpp"

#include <algorithm>
#include <memory>

namespace metaforce::save_indicator {
namespace {

// Seconds the note stays up at full strength, then fading out.
constexpr float kShowTime = 2.5f;
constexpr float kFadeTime = 0.5f;
// Seconds to wait for the font before giving up on a note.
constexpr float kLoadTimeout = 5.f;
// Distance from the left and bottom edges, and the text block's size, in 640x448 UI units.
constexpr float kMargin = 28.f;
constexpr int kBlockWidth = 256;
constexpr int kBlockHeight = 32;
constexpr float kUiHeight = 448.f;

// Made by Show and dropped once the note is gone, so the font is only held while it's up.
std::unique_ptr< CGuiTextSupport > sText;
// Seconds since the font finished loading, or since Show while it hasn't.
float sTime = 0.f;
bool sLoaded = false;

CAssetId FontId() {
  const SObjectTag* tag = gpResourceFactory->GetResourceIdByName("FONT_Deface14B");
  return tag != nullptr ? tag->GetId() : kInvalidAssetId;
}

} // namespace

void Show() {
  const CAssetId font = FontId();
  if (font == kInvalidAssetId || gpSimplePool == nullptr || gpTweakGuiColors == nullptr) {
    return;
  }
  sText = std::make_unique< CGuiTextSupport >(
      font, CGuiTextProperties(false, true, kJustification_Left, kVerticalJustification_Bottom),
      gpTweakGuiColors->GetHudMessageFill(), gpTweakGuiColors->GetHudMessageOutline(),
      CColor::White(), kBlockWidth, kBlockHeight, gpSimplePool);
  sText->SetText(rstl::string("Saved"));
  sTime = 0.f;
  sLoaded = false;
}

void Update(float dt) {
  if (!sText) {
    return;
  }
  if (!sLoaded && sText->GetIsTextSupportFinishedLoading()) {
    sLoaded = true;
    sTime = 0.f;
  }
  sTime += dt;
  if (sLoaded ? sTime >= kShowTime + kFadeTime : sTime >= kLoadTimeout) {
    sText.reset();
    return;
  }
  sText->Update(dt);
}

void Draw() {
  if (!sText || !sLoaded) {
    return;
  }
  const float alpha = std::clamp((kShowTime + kFadeTime - sTime) / kFadeTime, 0.f, 1.f);
  sText->SetGeometryColor(CColor::White().WithAlphaOf(alpha));

  const CGraphics::CProjectionState projection = CGraphics::GetProjectionState();
  const CTransform4f model = CGraphics::GetModelMatrix();
  CGraphics::SetCullMode(kCM_None);
  // The UI's ortho runs bottom to top: z is 0 at the bottom of the screen.
  const rstl::pair< CVector2f, CVector2f > region =
      gpRender->SetViewportOrtho(false, -4096.f, 4096.f);
  AdjustUiProjection();
  // The left edge of the screen, past the 4:3 area on a wider one.
  const float left = CGraphics::GetProjectionState().GetLeft();
  const float scale = (region.second.GetY() - region.first.GetY()) / kUiHeight;
  CTransform4f xf = CTransform4f::Scale(scale, 1.f, scale);
  // The block hangs down from its top left corner, with the text at its bottom.
  xf.SetTranslation(CVector3f(left + kMargin * scale, 0.f,
                              region.first.GetY() + (kMargin + kBlockHeight) * scale));
  gpRender->SetModelMatrix(xf);
  gpRender->SetDepthReadWrite(false, false);
  gpRender->SetBlendMode_AlphaBlended();
  sText->Render();

  CGraphics::SetProjectionState(projection);
  CGraphics::SetModelMatrix(model);
  CGraphics::SetCullMode(kCM_Front);
}

} // namespace metaforce::save_indicator

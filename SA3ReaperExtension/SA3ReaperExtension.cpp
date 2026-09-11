#include "SA3ReaperExtension.h"
#include "ReaperExt_include_in_plug_src.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <utility>

#include "IControls.h"
#include "ITextEntryControl.h"
#include "SA3UITheme.h"
#include "roboto.hpp"

namespace
{
class SA3BackgroundControl final : public IControl
{
public:
  explicit SA3BackgroundControl(const IRECT& bounds)
  : IControl(bounds)
  {
    mIgnoreMouse = true;
  }

  void Draw(IGraphics& g) override
  {
    g.FillRect(gary::ui::Background(), mRECT);
    const IRECT shell = mRECT.GetPadded(-8.f);
    g.FillRoundRect(gary::ui::Panel(), shell, 7.f);
    g.DrawRoundRect(gary::ui::Frame(), shell, 7.f);
  }
};

class SA3TabControl final : public IControl
{
public:
  SA3TabControl(const IRECT& bounds, const char* label, bool active, std::function<void()> action)
  : IControl(bounds)
  , mLabel(label)
  , mActive(active)
  , mAction(std::move(action))
  {
  }

  void Draw(IGraphics& g) override
  {
    gary::ui::DrawTab(g, mRECT, mLabel.c_str(), gary::ui::FontName, mActive);
  }

  void OnMouseDown(float, float, const IMouseMod&) override
  {
    if (mAction)
      mAction();
  }

  void SetActive(bool active)
  {
    if (mActive != active)
    {
      mActive = active;
      SetDirty(false);
    }
  }

private:
  std::string mLabel;
  bool mActive = false;
  std::function<void()> mAction;
};

class SA3ActionButton final : public IControl
{
public:
  SA3ActionButton(const IRECT& bounds, const char* label, std::function<void()> action)
  : IControl(bounds)
  , mLabel(label)
  , mAction(std::move(action))
  {
  }

  void Draw(IGraphics& g) override
  {
    gary::ui::DrawButton(g, mRECT, mLabel.c_str(), gary::ui::FontName, mHovered, mEnabled);
  }

  void OnMouseDown(float, float, const IMouseMod&) override
  {
    if (mEnabled && mAction)
      mAction();
  }

  void OnMouseOver(float, float, const IMouseMod&) override
  {
    mHovered = true;
    SetDirty(false);
  }

  void OnMouseOut() override
  {
    mHovered = false;
    SetDirty(false);
  }

  void SetLabel(const char* label)
  {
    if (mLabel != label)
    {
      mLabel = label;
      SetDirty(false);
    }
  }

  void SetEnabled(bool enabled)
  {
    if (mEnabled != enabled)
    {
      mEnabled = enabled;
      SetDirty(false);
    }
  }

private:
  std::string mLabel;
  std::function<void()> mAction;
  bool mHovered = false;
  bool mEnabled = true;
};

class SA3PromptControl final : public IEditableTextControl
{
public:
  SA3PromptControl(const IRECT& bounds, const char* prompt, const IText& text)
  : IEditableTextControl(bounds, prompt, text, gary::ui::PanelDark())
  {
    SetTextEntryLength(2048);
  }

  void Draw(IGraphics& g) override
  {
    g.FillRoundRect(gary::ui::PanelDark(), mRECT, gary::ui::CornerRadius);
    g.DrawRoundRect(gary::ui::FrameSoft(), mRECT, gary::ui::CornerRadius);
    const IRECT textBounds = mRECT.GetPadded(-10.f);
    if (GetStr()[0] != '\0')
      g.DrawText(mText, GetStr(), textBounds);
    else
      g.DrawText(mText.WithFGColor(gary::ui::TextFaint()), "describe the audio to generate…", textBounds);
  }
};

std::string HexEncode(const std::string& text)
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(text.size() * 2);
  for (const unsigned char c : text)
  {
    encoded.push_back(kHex[c >> 4]);
    encoded.push_back(kHex[c & 0x0f]);
  }
  return encoded;
}

bool HexDecode(const std::string& encoded, std::string& text)
{
  if ((encoded.size() & 1u) != 0u)
    return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  text.clear();
  text.reserve(encoded.size() / 2);
  for (size_t i = 0; i < encoded.size(); i += 2)
  {
    const int high = nibble(encoded[i]);
    const int low = nibble(encoded[i + 1]);
    if (high < 0 || low < 0)
      return false;
    text.push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

bool NearlyEqual(double lhs, double rhs)
{
  return std::abs(lhs - rhs) < 0.0001;
}

bool ParseBoolSetting(const std::string& text, bool fallback)
{
  if (text.empty()) return fallback;
  if (text == "1" || text == "true" || text == "on") return true;
  if (text == "0" || text == "false" || text == "off") return false;
  return fallback;
}

float ParseFloatSetting(const std::string& text, float fallback, float minimum, float maximum)
{
  if (text.empty()) return fallback;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || (end && *end != '\0') || !std::isfinite(value))
    return fallback;
  return std::clamp(value, minimum, maximum);
}

std::string SettingFloat(float value)
{
  char text[32] = {};
  std::snprintf(text, sizeof(text), "%.3f", value);
  return text;
}

std::string CompactText(const std::string& text, size_t maxChars)
{
  if (text.size() <= maxChars)
    return text;
  if (maxChars <= 3)
    return text.substr(0, maxChars);
  return text.substr(0, maxChars - 3) + "...";
}

std::string FileNameOnly(const std::string& path)
{
  return path.empty() ? std::string() : std::filesystem::path(path).filename().string();
}

std::string HumanBytes(uint64_t bytes)
{
  char text[64] = {};
  if (bytes >= 1024ull * 1024ull)
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  else if (bytes >= 1024ull)
    std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  else
    std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
  return text;
}
}

class SA3SettingsControl final : public IControl
{
public:
  SA3SettingsControl(const IRECT& bounds, SA3ReaperExtension& owner)
  : IControl(bounds)
  , mOwner(owner)
  {
  }

  void Draw(IGraphics& g) override
  {
    using namespace gary::ui;
    ClearHitRects();

    g.DrawText(IText(18.f, COLOR_WHITE, FontName, EAlign::Near, EVAlign::Middle),
               "settings", IRECT(mRECT.L, mRECT.T, mRECT.L + 84.f, mRECT.T + 24.f));
    g.DrawText(IText(10.f, TextDim(), FontName, EAlign::Near, EVAlign::Middle),
               CompactText(mOwner.mSettingsNotice, 70).c_str(),
               IRECT(mRECT.L + 90.f, mRECT.T, mRECT.R, mRECT.T + 24.f));

    float y = mRECT.T + 30.f;
    const IRECT modelCard(mRECT.L, y, mRECT.R, y + 64.f);
    DrawCard(g, modelCard);
    DrawModels(g, modelCard);
    y = modelCard.B + 6.f;

    const IRECT decoderCard(mRECT.L, y, mRECT.R, y + 94.f);
    DrawCard(g, decoderCard);
    DrawDecoder(g, decoderCard);
    y = decoderCard.B + 6.f;

    const IRECT outputCard(mRECT.L, y, mRECT.R, mRECT.B);
    DrawCard(g, outputCard);
    DrawOutput(g, outputCard);
  }

  void OnMouseDown(float x, float y, const IMouseMod&) override
  {
    if (mModelMenuRect.Contains(x, y)) { OpenModelMenu(); return; }
    if (mModelsFolderRect.Contains(x, y)) { mOwner.ChooseModelsFolder(); SetDirty(false); return; }
    if (mDecoderToggleRect.Contains(x, y)) { mOwner.SetDecoderLoraEnabled(!mOwner.mDecoderLoraEnabled); SetDirty(false); return; }
    if (mDecoderDownloadRect.Contains(x, y)) { mOwner.StartOrCancelDecoderLoraDownload(); SetDirty(false); return; }
    if (mDecoderChooseRect.Contains(x, y)) { mOwner.ChooseDecoderLora(); SetDirty(false); return; }
    if (mDecoderClearRect.Contains(x, y)) { mOwner.ClearDecoderLora(); SetDirty(false); return; }
    if (mNormalizeToggleRect.Contains(x, y)) { mOwner.SetPeakNormalizeEnabled(!mOwner.mPeakNormalizeEnabled); SetDirty(false); return; }
    if (mLimiterToggleRect.Contains(x, y)) { mOwner.SetLimiterEnabled(!mOwner.mLimiterEnabled); SetDirty(false); return; }
    if (mDefaultsRect.Contains(x, y)) { mOwner.ResetOutputProcessing(); SetDirty(false); return; }
    if (mRawRect.Contains(x, y)) { mOwner.SetRawOutputProcessing(); SetDirty(false); return; }
    if (mPeakRect.Contains(x, y)) { mSlider = Slider::Peak; UpdateSlider(x); return; }
    if (mCeilingRect.Contains(x, y)) { mSlider = Slider::Ceiling; UpdateSlider(x); return; }
    if (mKneeRect.Contains(x, y)) { mSlider = Slider::Knee; UpdateSlider(x); return; }
  }

  void OnMouseDrag(float x, float, float, float, const IMouseMod&) override
  {
    if (mSlider != Slider::None)
      UpdateSlider(x);
  }

  void OnMouseUp(float, float, const IMouseMod&) override
  {
    mSlider = Slider::None;
  }

  void OnPopupMenuSelection(IPopupMenu* menu, int) override
  {
    if (menu)
    {
      const int selected = menu->GetChosenItemIdx();
      if (selected == 0)
        mOwner.SelectModelVariant("medium");
      else if (selected == 1)
        mOwner.SelectModelVariant("small-music");
    }
    SetDirty(false);
  }

private:
  enum class Slider { None, Peak, Ceiling, Knee };

  static void DrawCard(IGraphics& g, const IRECT& bounds)
  {
    g.FillRoundRect(gary::ui::PanelDark(), bounds, 5.f);
    g.DrawRoundRect(gary::ui::FrameSoft(), bounds, 5.f);
  }

  static void DrawToggle(IGraphics& g, const IRECT& bounds, const char* label, bool on, bool interactive = true)
  {
    const IRECT box(bounds.L, bounds.MH() - 7.f, bounds.L + 14.f, bounds.MH() + 7.f);
    const IColor border = interactive ? (on ? gary::ui::Red() : gary::ui::Frame()) : gary::ui::FrameSoft();
    g.DrawRoundRect(border, box, 2.f);
    if (on)
      g.FillRoundRect(interactive ? gary::ui::Red() : gary::ui::TextDim(), box.GetPadded(-4.f), 1.f);
    g.DrawText(IText(11.f, interactive ? (on ? COLOR_WHITE : gary::ui::TextDim()) : gary::ui::TextFaint(),
                     gary::ui::FontName,
                     EAlign::Near, EVAlign::Middle), label, IRECT(box.R + 6.f, bounds.T, bounds.R, bounds.B));
  }

  static void DrawSlider(IGraphics& g, const IRECT& bounds, const char* label, const char* valueText,
                         float value, float minimum, float maximum, bool enabled)
  {
    g.DrawText(IText(10.f, enabled ? gary::ui::TextDim() : gary::ui::FrameSoft(), gary::ui::FontName,
                     EAlign::Near, EVAlign::Middle), label, IRECT(bounds.L, bounds.T, bounds.L + 54.f, bounds.B));
    g.DrawText(IText(10.f, enabled ? COLOR_WHITE : gary::ui::TextDim(), gary::ui::FontName,
                     EAlign::Far, EVAlign::Middle), valueText, IRECT(bounds.R - 54.f, bounds.T, bounds.R, bounds.B));
    const IRECT slider(bounds.L + 58.f, bounds.MH() - 7.f, bounds.R - 60.f, bounds.MH() + 7.f);
    const IRECT track(slider.L, slider.MH() - 2.f, slider.R, slider.MH() + 2.f);
    const float fraction = std::clamp((value - minimum) / std::max(0.001f, maximum - minimum), 0.f, 1.f);
    const float filled = slider.L + slider.W() * fraction;
    g.FillRoundRect(gary::ui::FrameSoft(), track, 2.f);
    g.FillRoundRect(enabled ? gary::ui::Red() : gary::ui::FrameSoft(),
                    IRECT(track.L, track.T, filled, track.B), 2.f);
    g.FillCircle(enabled ? COLOR_WHITE : gary::ui::TextDim(), filled, slider.MH(), enabled ? 5.f : 4.f);
  }

  void DrawModels(IGraphics& g, const IRECT& card)
  {
    g.DrawText(IText(13.f, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle),
               "generation model", IRECT(card.L + 10.f, card.T + 4.f, card.L + 126.f, card.T + 24.f));
    const bool ready = mOwner.mModelVariant == "medium" ? mOwner.mMediumModelsAvailable
                                                         : mOwner.mSmallModelsAvailable;
    const std::string folder = FileNameOnly(mOwner.mModelsDir);
    const std::string detail = (ready ? "ready" : "missing") + (folder.empty() ? "" : "  /  " + folder);
    g.DrawText(IText(10.f, ready ? gary::ui::Green() : gary::ui::TextDim(), gary::ui::FontName,
                     EAlign::Near, EVAlign::Middle), CompactText(detail, 54).c_str(),
               IRECT(card.L + 132.f, card.T + 4.f, card.R - 10.f, card.T + 24.f));

    const float left = card.L + 10.f;
    const float right = card.R - 10.f;
    const float gap = 6.f;
    const float width = (right - left - gap) * 0.5f;
    mModelMenuRect = IRECT(left, card.T + 31.f, left + width, card.B - 7.f);
    mModelsFolderRect = IRECT(mModelMenuRect.R + gap, mModelMenuRect.T, right, mModelMenuRect.B);
    const std::string modelLabel = mOwner.mModelVariant + "  v";
    gary::ui::DrawButton(g, mModelMenuRect, modelLabel.c_str(), gary::ui::FontName);
    gary::ui::DrawButton(g, mModelsFolderRect, "models folder", gary::ui::FontName);
  }

  void OpenModelMenu()
  {
    if (!GetUI())
      return;
    mModelMenu.Clear();
    IPopupMenu::Item* medium = mModelMenu.AddItem("medium");
    IPopupMenu::Item* smallMusic = mModelMenu.AddItem("small-music");
    medium->SetEnabled(mOwner.mMediumModelsAvailable);
    smallMusic->SetEnabled(mOwner.mSmallModelsAvailable);
    mModelMenu.CheckItem(mOwner.mModelVariant == "small-music" ? 1 : 0, true);
    GetUI()->CreatePopupMenu(*this, mModelMenu, mModelMenuRect);
  }

  void DrawDecoder(IGraphics& g, const IRECT& card)
  {
    g.DrawText(IText(13.f, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle),
               "decoder correction (medium only)", IRECT(card.L + 10.f, card.T + 3.f, card.R - 10.f, card.T + 23.f));
    const bool installed = !mOwner.mDecoderLoraPath.empty() && gary::FileSizeBytes(mOwner.mDecoderLoraPath) > 0;
    const bool active = installed && mOwner.mDecoderLoraEnabled && mOwner.mModelVariant == "medium";
    std::string state;
    if (mOwner.mDecoderDownloadBusy.load(std::memory_order_acquire))
      state = mOwner.DecoderDownloadStatus();
    else if (!installed)
      state = "not installed  /  published adapter: squeakfix_v3";
    else if (!mOwner.mDecoderLoraEnabled)
      state = "installed - disabled  /  " + FileNameOnly(mOwner.mDecoderLoraPath);
    else if (mOwner.mModelVariant != "medium")
      state = "available when generation model is medium  /  " + FileNameOnly(mOwner.mDecoderLoraPath);
    else
      state = "active for next render  /  " + FileNameOnly(mOwner.mDecoderLoraPath);
    g.DrawText(IText(10.f, active ? gary::ui::Green() : gary::ui::TextDim(), gary::ui::FontName,
                     EAlign::Near, EVAlign::Middle), CompactText(state, 64).c_str(),
               IRECT(card.L + 10.f, card.T + 22.f, card.R - 10.f, card.T + 40.f));

    mDecoderToggleRect = IRECT(card.L + 10.f, card.T + 41.f, card.L + 150.f, card.T + 62.f);
    const bool decoderCompatible = mOwner.mModelVariant == "medium";
    DrawToggle(g, mDecoderToggleRect, decoderCompatible ? "use correction" : "not used by small-music",
               decoderCompatible && mOwner.mDecoderLoraEnabled, decoderCompatible);
    if (!decoderCompatible)
      mDecoderToggleRect = {};

    const float left = card.L + 10.f;
    const float right = card.R - 10.f;
    const float gap = 6.f;
    const float width = (right - left - gap * 2.f) / 3.f;
    mDecoderDownloadRect = IRECT(left, card.T + 65.f, left + width, card.B - 6.f);
    mDecoderChooseRect = IRECT(mDecoderDownloadRect.R + gap, mDecoderDownloadRect.T,
                               mDecoderDownloadRect.R + gap + width, mDecoderDownloadRect.B);
    mDecoderClearRect = IRECT(mDecoderChooseRect.R + gap, mDecoderChooseRect.T, right, mDecoderChooseRect.B);
    const char* downloadLabel = mOwner.mDecoderDownloadBusy.load(std::memory_order_acquire) ? "cancel"
                                : installed ? "redownload" : "download 11 MB";
    gary::ui::DrawButton(g, mDecoderDownloadRect, downloadLabel, gary::ui::FontName);
    gary::ui::DrawButton(g, mDecoderChooseRect, "choose file", gary::ui::FontName);
    gary::ui::DrawButton(g, mDecoderClearRect, "clear", gary::ui::FontName, false, installed);
    if (!installed)
      mDecoderClearRect = {};
  }

  void DrawOutput(IGraphics& g, const IRECT& card)
  {
    if (card.H() < 80.f)
      return;
    g.DrawText(IText(13.f, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle),
               "output processing", IRECT(card.L + 10.f, card.T + 3.f, card.R - 150.f, card.T + 23.f));
    mDefaultsRect = IRECT(card.R - 142.f, card.T + 4.f, card.R - 57.f, card.T + 23.f);
    mRawRect = IRECT(card.R - 51.f, card.T + 4.f, card.R - 10.f, card.T + 23.f);
    gary::ui::DrawButton(g, mDefaultsRect, "defaults", gary::ui::FontName);
    gary::ui::DrawButton(g, mRawRect, "raw", gary::ui::FontName);

    mNormalizeToggleRect = IRECT(card.L + 10.f, card.T + 25.f, card.R - 10.f, card.T + 43.f);
    DrawToggle(g, mNormalizeToggleRect, "peak normalize", mOwner.mPeakNormalizeEnabled);
    char value[32] = {};
    std::snprintf(value, sizeof(value), "%+.1f dB", mOwner.mPeakNormalizeDb);
    mPeakRect = IRECT(card.L + 10.f, card.T + 44.f, card.R - 10.f, card.T + 65.f);
    DrawSlider(g, mPeakRect, "target", value, mOwner.mPeakNormalizeDb, -6.f, 6.f, mOwner.mPeakNormalizeEnabled);
    if (!mOwner.mPeakNormalizeEnabled) mPeakRect = {};

    mLimiterToggleRect = IRECT(card.L + 10.f, card.T + 68.f, card.R - 10.f, card.T + 86.f);
    DrawToggle(g, mLimiterToggleRect, "soft limiter", mOwner.mLimiterEnabled);
    std::snprintf(value, sizeof(value), "%.1f dB", mOwner.mLimiterCeilingDb);
    mCeilingRect = IRECT(card.L + 10.f, card.T + 87.f, card.R - 10.f, card.T + 108.f);
    DrawSlider(g, mCeilingRect, "ceiling", value, mOwner.mLimiterCeilingDb, -6.f, 0.f, mOwner.mLimiterEnabled);
    std::snprintf(value, sizeof(value), "%.2f", mOwner.mLimiterKnee);
    mKneeRect = IRECT(card.L + 10.f, card.T + 109.f, card.R - 10.f, std::min(card.B - 4.f, card.T + 136.f));
    DrawSlider(g, mKneeRect, "knee", value, mOwner.mLimiterKnee, 0.1f, 1.f, mOwner.mLimiterEnabled);
    if (!mOwner.mLimiterEnabled) mCeilingRect = mKneeRect = {};
  }

  void UpdateSlider(float x)
  {
    const IRECT* rect = nullptr;
    if (mSlider == Slider::Peak) rect = &mPeakRect;
    else if (mSlider == Slider::Ceiling) rect = &mCeilingRect;
    else if (mSlider == Slider::Knee) rect = &mKneeRect;
    if (!rect || rect->W() <= 0.f)
      return;
    const float sliderLeft = rect->L + 58.f;
    const float sliderRight = rect->R - 60.f;
    const float fraction = std::clamp((x - sliderLeft) / std::max(1.f, sliderRight - sliderLeft), 0.f, 1.f);
    if (mSlider == Slider::Peak) mOwner.SetPeakNormalizeDb(-6.f + fraction * 12.f);
    else if (mSlider == Slider::Ceiling) mOwner.SetLimiterCeilingDb(-6.f + fraction * 6.f);
    else if (mSlider == Slider::Knee) mOwner.SetLimiterKnee(0.1f + fraction * 0.9f);
    SetDirty(false);
  }

  void ClearHitRects()
  {
    mModelMenuRect = mModelsFolderRect = {};
    mDecoderToggleRect = mDecoderDownloadRect = mDecoderChooseRect = mDecoderClearRect = {};
    mNormalizeToggleRect = mLimiterToggleRect = {};
    mPeakRect = mCeilingRect = mKneeRect = {};
    mDefaultsRect = mRawRect = {};
  }

  SA3ReaperExtension& mOwner;
  Slider mSlider = Slider::None;
  IPopupMenu mModelMenu;
  IRECT mModelMenuRect, mModelsFolderRect;
  IRECT mDecoderToggleRect, mDecoderDownloadRect, mDecoderChooseRect, mDecoderClearRect;
  IRECT mNormalizeToggleRect, mLimiterToggleRect;
  IRECT mPeakRect, mCeilingRect, mKneeRect, mDefaultsRect, mRawRect;
};

class SA3CreativeLoraControl final : public IControl
{
public:
  SA3CreativeLoraControl(const IRECT& bounds, SA3ReaperExtension& owner)
  : IControl(bounds)
  , mOwner(owner)
  {
  }

  void Draw(IGraphics& g) override
  {
    using namespace gary::ui;
    mToggleRects.clear();
    mSliderRects.clear();
    mRemoveRects.clear();

    g.FillRoundRect(PanelDark(), mRECT, 4.f);
    g.DrawRoundRect(FrameSoft(), mRECT, 4.f);
    const float left = mRECT.L + 9.f;
    const float right = mRECT.R - 9.f;
    g.DrawText(IText(12.f, COLOR_WHITE, FontName, EAlign::Near, EVAlign::Middle),
               "creative LoRAs", IRECT(left, mRECT.T + 3.f, left + 100.f, mRECT.T + 23.f));
    const std::string registryLabel = mOwner.mModelVariant + " registry";
    g.DrawText(IText(9.f, TextDim(), FontName, EAlign::Near, EVAlign::Middle),
               registryLabel.c_str(), IRECT(left + 104.f, mRECT.T + 3.f, right - 84.f, mRECT.T + 23.f));
    mAddRect = IRECT(right - 76.f, mRECT.T + 3.f, right, mRECT.T + 23.f);
    DrawButton(g, mAddRect, "add LoRA", FontName);

    if (mOwner.mCreativeLoras.empty())
    {
      g.DrawText(IText(10.f, TextDim(), FontName, EAlign::Near, EVAlign::Middle),
                 "no creative LoRAs loaded", IRECT(left, mRECT.T + 27.f, right, mRECT.B - 3.f));
      return;
    }

    const size_t rows = std::min<size_t>(2, mOwner.mCreativeLoras.size());
    for (size_t i = 0; i < rows; ++i)
    {
      const auto& lora = mOwner.mCreativeLoras[i];
      const float top = mRECT.T + 27.f + static_cast<float>(i) * 17.f;
      const IRECT row(left, top, right, top + 15.f);
      const IRECT toggle(row.L, row.T + 1.f, row.L + 13.f, row.B - 1.f);
      const IRECT remove(row.R - 18.f, row.T, row.R, row.B);
      const IRECT value(remove.L - 34.f, row.T, remove.L - 2.f, row.B);
      const float sliderWidth = std::clamp(row.W() * 0.28f, 76.f, 150.f);
      const IRECT slider(value.L - sliderWidth - 5.f, row.T + 1.f, value.L - 5.f, row.B - 1.f);
      const IRECT label(toggle.R + 5.f, row.T, slider.L - 7.f, row.B);
      mToggleRects.push_back(toggle);
      mSliderRects.push_back(slider);
      mRemoveRects.push_back(remove);

      g.DrawRoundRect(lora.enabled ? Red() : Frame(), toggle, 2.f);
      if (lora.enabled)
        g.FillRoundRect(Red(), toggle.GetPadded(-3.5f), 1.f);
      g.DrawText(IText(10.f, COLOR_WHITE, FontName, EAlign::Near, EVAlign::Middle),
                 CompactText(lora.name, 30).c_str(), label);

      const IRECT track(slider.L, slider.MH() - 1.5f, slider.R, slider.MH() + 1.5f);
      const float fraction = std::clamp(lora.strength * 0.5f, 0.f, 1.f);
      const float filled = slider.L + slider.W() * fraction;
      g.FillRoundRect(FrameSoft(), track, 1.5f);
      g.FillRoundRect(lora.enabled ? Red() : Frame(), IRECT(track.L, track.T, filled, track.B), 1.5f);
      g.FillCircle(lora.enabled ? COLOR_WHITE : TextDim(), filled, slider.MH(), 4.f);
      char strength[16] = {};
      std::snprintf(strength, sizeof(strength), "%.2f", lora.strength);
      g.DrawText(IText(9.f, TextDim(), FontName, EAlign::Center, EVAlign::Middle), strength, value);
      DrawButton(g, remove, "x", FontName);
    }

    if (mOwner.mCreativeLoras.size() > rows)
    {
      const std::string more = "+" + std::to_string(mOwner.mCreativeLoras.size() - rows);
      g.DrawText(IText(9.f, TextDim(), FontName, EAlign::Far, EVAlign::Middle),
                 more.c_str(), IRECT(mAddRect.L - 34.f, mRECT.T + 4.f, mAddRect.L - 4.f, mRECT.T + 22.f));
    }
  }

  void OnMouseDown(float x, float y, const IMouseMod&) override
  {
    if (mAddRect.Contains(x, y))
    {
      mOwner.AddCreativeLora();
      SetDirty(false);
      return;
    }
    for (size_t i = 0; i < mToggleRects.size(); ++i)
    {
      if (mToggleRects[i].Contains(x, y))
      {
        mOwner.ToggleCreativeLora(i);
        SetDirty(false);
        return;
      }
      if (mRemoveRects[i].Contains(x, y))
      {
        mOwner.RemoveCreativeLora(i);
        SetDirty(false);
        return;
      }
      if (mSliderRects[i].Contains(x, y))
      {
        mActiveSlider = i;
        UpdateSlider(x);
        return;
      }
    }
  }

  void OnMouseDrag(float x, float, float, float, const IMouseMod&) override
  {
    if (mActiveSlider < mSliderRects.size())
      UpdateSlider(x);
  }

  void OnMouseUp(float, float, const IMouseMod&) override
  {
    mActiveSlider = static_cast<size_t>(-1);
  }

private:
  void UpdateSlider(float x)
  {
    if (mActiveSlider >= mSliderRects.size())
      return;
    const IRECT& slider = mSliderRects[mActiveSlider];
    const float fraction = std::clamp((x - slider.L) / std::max(1.f, slider.W()), 0.f, 1.f);
    mOwner.SetCreativeLoraStrength(mActiveSlider, fraction * 2.f);
    SetDirty(false);
  }

  SA3ReaperExtension& mOwner;
  IRECT mAddRect;
  std::vector<IRECT> mToggleRects;
  std::vector<IRECT> mSliderRects;
  std::vector<IRECT> mRemoveRects;
  size_t mActiveSlider = static_cast<size_t>(-1);
};

bool SA3ReaperExtension::SelectionSnapshot::operator==(const SelectionSnapshot& other) const
{
  return itemCount == other.itemCount
    && selectedTrackCount == other.selectedTrackCount
    && NearlyEqual(itemStart, other.itemStart)
    && NearlyEqual(itemLength, other.itemLength)
    && NearlyEqual(timeStart, other.timeStart)
    && NearlyEqual(timeEnd, other.timeEnd)
    && NearlyEqual(bpm, other.bpm)
    && NearlyEqual(beatsPerMeasure, other.beatsPerMeasure)
    && beatUnit == other.beatUnit
    && hasActiveTake == other.hasActiveTake
    && firstTakeIsAudio == other.firstTakeIsAudio
    && trackName == other.trackName
    && selectedTrackName == other.selectedTrackName
    && takeName == other.takeName
    && sourceType == other.sourceType;
}

SA3ReaperExtension::SA3ReaperExtension(reaper_plugin_info_t* pRec)
: ReaperExtBase(pRec)
{
  IMPAPI(CountSelectedMediaItems);
  IMPAPI(CountSelectedTracks);
  IMPAPI(GetSelectedMediaItem);
  IMPAPI(GetSelectedTrack);
  IMPAPI(GetActiveTake);
  IMPAPI(GetMediaItemInfo_Value);
  IMPAPI(GetMediaItemTrack);
  IMPAPI(GetTrackName);
  IMPAPI(GetTakeName);
  IMPAPI(GetMediaItemTake_Source);
  IMPAPI(GetMediaSourceType);
  IMPAPI(TakeIsMIDI);
  IMPAPI(GetSet_LoopTimeRange2);
  IMPAPI(TimeMap_GetTimeSigAtTime);
  IMPAPI(ValidatePtr2);
  IMPAPI(CountTrackMediaItems);
  IMPAPI(GetTrackMediaItem);
  IMPAPI(SplitMediaItem);
  IMPAPI(DeleteTrackMediaItem);
  IMPAPI(PCM_Source_BuildPeaks);
  IMPAPI(PCM_Source_CreateFromFile);
  IMPAPI(AddMediaItemToTrack);
  IMPAPI(AddTakeToMediaItem);
  IMPAPI(GetSetMediaItemTakeInfo);
  IMPAPI(GetSetMediaItemTakeInfo_String);
  IMPAPI(SetMediaItemInfo_Value);
  IMPAPI(Undo_BeginBlock2);
  IMPAPI(Undo_EndBlock2);
  IMPAPI(UpdateItemInProject);
  IMPAPI(UpdateArrange);
  IMPAPI(MarkProjectDirty);

  SetDockId("SA3ReaperExtension.MainWindow");
  SetMenuName("SA3");
  ReloadSharedSettings();

  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS);
  };

  RegisterAction("SA3: Transform selected audio", [this]() { SelectOperation(Operation::Transform); },
                 true, nullptr, "Media item context", "Transform selected audio");
  RegisterAction("SA3: Continue selected audio", [this]() { SelectOperation(Operation::Continue); },
                 true, nullptr, "Media item context", "Continue selected audio");
  RegisterAction("SA3: Generate at time selection", [this]() { SelectOperation(Operation::Generate); },
                 true, nullptr, nullptr, "Generate at time selection");
  RegisterAction("SA3: Show/hide panel", [this]() { ShowHideMainWindow(); },
                 true, GetWindowTogglePtr(), nullptr, "Show/hide panel");
  RegisterAction("SA3: Dock/undock panel", [this]() { ToggleDocking(); },
                 true, GetDockTogglePtr(), nullptr, "Dock/undock panel");

  mLayoutFunc = [&](IGraphics* graphics) {
    if (graphics->NControls())
    {
      ApplyResponsiveLayout(graphics);
      return;
    }

    graphics->SetLayoutOnResize(true);
    graphics->EnableMouseOver(true);
    graphics->AttachTextEntryControl();
    if (!graphics->LoadFont(gary::ui::FontName, (void*) ROBOTO_REGULAR, ROBOTO_REGULAR_length))
      graphics->LoadFont(gary::ui::FontName, "Arial", ETextStyle::Normal);

    const IRECT initial = graphics->GetBounds();
    graphics->AttachControl(new SA3BackgroundControl(initial), kCtrlTagBackground);
    graphics->AttachControl(new ITextControl(initial, "sa3 / REAPER",
      IText(gary::ui::TitleTextSize, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle)),
      kCtrlTagOperation);
    graphics->AttachControl(new SA3TabControl(initial, "generate", false,
      [this]() { SelectOperation(Operation::Generate); }), kCtrlTagGenerate);
    graphics->AttachControl(new SA3TabControl(initial, "transform", true,
      [this]() { SelectOperation(Operation::Transform); }), kCtrlTagTransform);
    graphics->AttachControl(new SA3TabControl(initial, "continue", false,
      [this]() { SelectOperation(Operation::Continue); }), kCtrlTagContinue);
    graphics->AttachControl(new IMultiLineTextControl(initial, "No media item selected",
      IText(17.f, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle)), kCtrlTagSelection);
    graphics->AttachControl(new ITextControl(initial, "Time selection: none",
      IText(gary::ui::BodyTextSize, gary::ui::TextDim(), gary::ui::FontName, EAlign::Near, EVAlign::Middle)),
      kCtrlTagTiming);
    graphics->AttachControl(new IMultiLineTextControl(initial,
      mPanelStatus.c_str(),
      IText(gary::ui::BodyTextSize, gary::ui::TextDim(), gary::ui::FontName, EAlign::Near, EVAlign::Top)),
      kCtrlTagHint);
    graphics->AttachControl(new SA3PromptControl(initial, mPrompt.c_str(),
      IText(gary::ui::BodyTextSize, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle)
        .WithTEColors(gary::ui::PanelDark(), COLOR_WHITE)), kCtrlTagPrompt);
    graphics->AttachControl(new SA3CreativeLoraControl(initial, *this), kCtrlTagCreativeLoras);
    graphics->AttachControl(new SA3ActionButton(initial, "generate into selection",
      [this]() { StartOrCancelGeneration(); }), kCtrlTagRun);
    graphics->AttachControl(new SA3ActionButton(initial, "settings",
      [this]() { ToggleSettingsPage(); }), kCtrlTagSettingsButton);
    graphics->AttachControl(new SA3SettingsControl(initial, *this), kCtrlTagSettings);

    ApplyResponsiveLayout(graphics);
    RefreshPanel(true);
  };
}

SA3ReaperExtension::~SA3ReaperExtension()
{
  mRenderService.Cancel();
  StopDecoderLoraDownload();
}

void SA3ReaperExtension::OnIdle()
{
  SyncPromptFromUI();
  gary::SA3RenderResult result;
  const bool completed = mRenderService.TakeCompletedResult(result);
  if (completed)
    FinishGeneration(std::move(result));
  ContinuePendingPeakBuilds();

  const uint64_t decoderRevision = mDecoderDownloadRevision.load(std::memory_order_acquire);
  const bool decoderFinished = decoderRevision != mSeenDecoderDownloadRevision;
  if (decoderFinished)
  {
    mSeenDecoderDownloadRevision = decoderRevision;
    mSettingsNotice = DecoderDownloadStatus();
    ReloadSharedSettings();
  }
  else if (++mSettingsRefreshTicks >= PLUG_FPS * 2)
  {
    mSettingsRefreshTicks = 0;
    ReloadSharedSettings(false);
  }

  RefreshPanel(completed || decoderFinished || mRenderService.Busy()
               || mDecoderDownloadBusy.load(std::memory_order_acquire));
}

void SA3ReaperExtension::OnActionRun(int, int)
{
  RefreshPanel();
}

void SA3ReaperExtension::ApplyResponsiveLayout(IGraphics* graphics)
{
  if (!graphics)
    return;

  const IRECT bounds = graphics->GetBounds();
  const IRECT content = bounds.GetPadded(-18.f);
  mWideLayout = bounds.W() >= 860.f;

  const float columnGap = 18.f;
  const IRECT main = mWideLayout
    ? IRECT(content.L, content.T, content.MW() - columnGap * 0.5f, content.B)
    : content;
  const IRECT settings = mWideLayout
    ? IRECT(content.MW() + columnGap * 0.5f, content.T, content.R, content.B)
    : IRECT(content.L, content.T + 42.f, content.R, content.B);
  const bool showMain = mWideLayout || !mSettingsOpen;
  const bool showSettings = mWideLayout || mSettingsOpen;

  auto place = [graphics](int tag, const IRECT& rect, bool visible = true) {
    if (IControl* control = graphics->GetControlWithTag(tag))
    {
      control->SetTargetAndDrawRECTs(rect);
      control->Hide(!visible);
    }
  };

  place(kCtrlTagBackground, bounds);
  const IRECT title(main.L, main.T, main.R - (mWideLayout ? 0.f : 92.f), main.T + 32.f);
  place(kCtrlTagOperation, title);
  place(kCtrlTagSettingsButton, IRECT(main.R - 82.f, main.T + 3.f, main.R, main.T + 29.f), !mWideLayout);

  const IRECT tabs(main.L, main.T + 46.f, main.R, main.T + 86.f);
  const float tabGap = 7.f;
  const float tabWidth = (tabs.W() - tabGap * 2.f) / 3.f;
  place(kCtrlTagGenerate, IRECT(tabs.L, tabs.T, tabs.L + tabWidth, tabs.B), showMain);
  place(kCtrlTagTransform, IRECT(tabs.L + tabWidth + tabGap, tabs.T,
                                 tabs.L + tabWidth * 2.f + tabGap, tabs.B), showMain);
  place(kCtrlTagContinue, IRECT(tabs.R - tabWidth, tabs.T, tabs.R, tabs.B), showMain);
  place(kCtrlTagSelection, IRECT(main.L, main.T + 96.f, main.R, main.T + 132.f), showMain);
  place(kCtrlTagTiming, IRECT(main.L, main.T + 138.f, main.R, main.T + 164.f), showMain);
  place(kCtrlTagPrompt, IRECT(main.L, main.T + 174.f, main.R, main.T + 222.f), showMain);
  place(kCtrlTagCreativeLoras, IRECT(main.L, main.T + 230.f, main.R, main.T + 294.f), showMain);
  place(kCtrlTagRun, IRECT(main.L, main.T + 302.f, main.R, main.T + 338.f), showMain);
  place(kCtrlTagHint, IRECT(main.L, std::max(main.T + 346.f, main.B - 48.f), main.R, main.B), showMain);
  place(kCtrlTagSettings, settings, showSettings);

  if (IControl* control = graphics->GetControlWithTag(kCtrlTagSettingsButton))
  {
    if (auto* button = control->As<SA3ActionButton>())
      button->SetLabel(mSettingsOpen ? "back" : "settings");
  }
  graphics->SetAllControlsDirty();
}

void SA3ReaperExtension::ToggleSettingsPage()
{
  if (mWideLayout)
    return;
  if (IGraphics* ui = GetUI())
  {
    if (auto* textEntry = ui->GetTextEntryControl(); textEntry && textEntry->EditInProgress())
      textEntry->CommitEdit();
  }
  mSettingsOpen = !mSettingsOpen;
  if (mSettingsOpen)
    ReloadSharedSettings();
  ApplyResponsiveLayout(GetUI());
  RefreshPanel(true);
}

void SA3ReaperExtension::ReloadSharedSettings(bool scanModels)
{
  const auto request = gary::LoadSharedTextGenerationRequest("", 1.0, 0.0);
  const bool modelLocationChanged = request.modelsDir != mModelsDir || request.variant != mModelVariant;
  mModelsDir = request.modelsDir;
  mModelVariant = request.variant;
  mDecoderLoraPath = gary::LoadSetting("decoder_lora_same_l_path");
  mDecoderLoraEnabled = ParseBoolSetting(gary::LoadSetting("decoder_lora_same_l_enabled"), true);
  mPeakNormalizeEnabled = request.peakNormalize;
  mPeakNormalizeDb = request.peakNormalizeDb;
  mLimiterEnabled = request.limiter;
  mLimiterCeilingDb = request.limiterCeilingDb;
  mLimiterKnee = request.limiterKnee;
  ReloadCreativeLoras();

  if (scanModels || modelLocationChanged)
  {
    std::vector<std::string> missing;
    mMediumModelsAvailable = gary::ModelSetComplete(mModelsDir, "medium", "f16", missing);
    missing.clear();
    mSmallModelsAvailable = gary::ModelSetComplete(mModelsDir, "small-music", "f16", missing);
  }

  if (IGraphics* ui = GetUI())
  {
    if (IControl* control = ui->GetControlWithTag(kCtrlTagSettings))
      control->SetDirty(false);
  }
}

void SA3ReaperExtension::ChooseModelsFolder()
{
  IGraphics* ui = GetUI();
  if (!ui)
    return;
  WDL_String directory;
  if (!mModelsDir.empty())
    directory.Set(mModelsDir.c_str());
  ui->PromptForDirectory(directory);
  if (directory.GetLength() == 0)
    return;

  const std::string selected = directory.Get();
  std::vector<std::string> missing;
  const bool medium = gary::ModelSetComplete(selected, "medium", "f16", missing);
  missing.clear();
  const bool smallAvailable = gary::ModelSetComplete(selected, "small-music", "f16", missing);
  if (!medium && !smallAvailable)
  {
    mSettingsNotice = "That folder does not contain a complete SAME-L or SAME-S model set.";
    RefreshPanel(true);
    return;
  }

  const std::string selectedVariant = mModelVariant == "small-music" && smallAvailable ? "small-music"
                                      : medium ? "medium" : "small-music";
  gary::SaveSetting("models_dir", selected);
  gary::SaveSetting("variant", selectedVariant);
  mSettingsNotice = "Models folder selected; " + selectedVariant + " is active.";
  ReloadSharedSettings();
  RefreshPanel(true);
}

void SA3ReaperExtension::SelectModelVariant(const char* variant)
{
  const std::string selected = variant && std::strcmp(variant, "small-music") == 0 ? "small-music" : "medium";
  const bool available = selected == "medium" ? mMediumModelsAvailable : mSmallModelsAvailable;
  if (!available)
  {
    mSettingsNotice = selected + " is not available in the selected models folder.";
    RefreshPanel(true);
    return;
  }
  gary::SaveSetting("variant", selected);
  mModelVariant = selected;
  ReloadCreativeLoras();
  mSettingsNotice = "Active model: " + selected + ". Its LoRA registry is loaded.";
  RefreshPanel(true);
}

void SA3ReaperExtension::SetDecoderLoraEnabled(bool enabled)
{
  mDecoderLoraEnabled = enabled;
  gary::SaveSetting("decoder_lora_same_l_enabled", enabled ? "1" : "0");
  mSettingsNotice = enabled ? (mModelVariant == "medium" ? "Decoder correction enabled."
                                                           : "Decoder correction saved for SAME-L only.")
                            : "Decoder correction disabled.";
  RefreshPanel(true);
}

void SA3ReaperExtension::ChooseDecoderLora()
{
  IGraphics* ui = GetUI();
  if (!ui)
    return;
  WDL_String fileName;
  WDL_String directory;
  ui->PromptForFile(fileName, directory, EFileAction::Open, "gguf safetensors");
  if (fileName.GetLength() == 0)
    return;

  const auto info = gary::ImportLoraFile(fileName.Get());
  if (!info.ok)
  {
    mSettingsNotice = "Decoder LoRA import failed: " + info.error;
    RefreshPanel(true);
    return;
  }
  gary::SaveSetting("decoder_lora_same_l_path", info.path);
  gary::SaveSetting("decoder_lora_same_l_enabled", "1");
  mSettingsNotice = "Decoder correction selected: " + FileNameOnly(info.path) + ".";
  ReloadSharedSettings(false);
  RefreshPanel(true);
}

void SA3ReaperExtension::ClearDecoderLora()
{
  gary::SaveSetting("decoder_lora_same_l_path", "");
  mDecoderLoraPath.clear();
  mSettingsNotice = "Decoder correction selection cleared; the file was kept.";
  RefreshPanel(true);
}

void SA3ReaperExtension::StartOrCancelDecoderLoraDownload()
{
  if (mDecoderDownloadBusy.load(std::memory_order_acquire))
  {
    mDecoderDownloadCancel.store(true, std::memory_order_release);
    SetDecoderDownloadStatus("cancelling decoder correction download");
    return;
  }
  if (mRenderService.Busy())
  {
    mSettingsNotice = "Finish or cancel the current generation before downloading.";
    RefreshPanel(true);
    return;
  }
  if (mDecoderDownloadWorker.joinable())
    mDecoderDownloadWorker.join();
  mDecoderDownloadCancel.store(false, std::memory_order_release);
  mDecoderDownloadProgress.store(0.f, std::memory_order_release);
  mDecoderDownloadBusy.store(true, std::memory_order_release);
  SetDecoderDownloadStatus("checking SAME-L decoder correction");
  mDecoderDownloadWorker = std::thread([this]() { DecoderLoraDownloadWorkerMain(); });
  RefreshPanel(true);
}

void SA3ReaperExtension::DecoderLoraDownloadWorkerMain()
{
  namespace fs = std::filesystem;
  auto finish = [this]() {
    mDecoderDownloadBusy.store(false, std::memory_order_release);
    mDecoderDownloadRevision.fetch_add(1, std::memory_order_acq_rel);
  };

  std::string directoryError;
  const std::string directory = gary::LoraDirectory(&directoryError);
  if (directory.empty())
  {
    SetDecoderDownloadStatus("decoder download failed: " + directoryError);
    finish();
    return;
  }

  constexpr const char* kRepository = "thepatch/same-l-decoder-lora";
  constexpr const char* kFileName = "squeakfix_v3.safetensors";
  const std::string url = gary::HuggingFaceResolveUrl(kRepository, kFileName);
  const std::string destination = (fs::path(directory) / kFileName).string();
  const long long expected = gary::HttpContentLength(url);
  long long local = static_cast<long long>(gary::FileSizeBytes(destination));
  if (expected > 0 && local > expected)
  {
    std::remove(destination.c_str());
    local = 0;
  }

  if (!(expected > 0 && local == expected))
  {
    std::string processError;
    gary::AsyncProcess process = gary::StartProcess(
      {"curl", "-fL", "--retry", "3", "-C", "-", "-o", destination, url}, processError);
    if (!process.valid)
    {
      SetDecoderDownloadStatus("decoder download failed to start: " + processError);
      finish();
      return;
    }

    int exitCode = 1;
    for (;;)
    {
      const bool done = gary::ProcessTryWait(process, &exitCode);
      const uint64_t currentBytes = gary::FileSizeBytes(destination);
      if (expected > 0)
        mDecoderDownloadProgress.store(static_cast<float>(std::clamp(
          static_cast<double>(currentBytes) / static_cast<double>(expected), 0.0, 1.0)),
          std::memory_order_release);
      SetDecoderDownloadStatus("downloading decoder correction - " + HumanBytes(currentBytes)
        + (expected > 0 ? " / " + HumanBytes(static_cast<uint64_t>(expected)) : ""));
      if (done)
        break;
      if (mDecoderDownloadCancel.load(std::memory_order_acquire))
      {
        gary::ProcessTerminate(process);
        gary::ProcessClose(process);
        SetDecoderDownloadStatus("decoder download cancelled; partial file kept");
        finish();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    gary::ProcessClose(process);
    if (exitCode != 0)
    {
      SetDecoderDownloadStatus("decoder download failed (curl exit " + std::to_string(exitCode) + ")");
      finish();
      return;
    }
  }

  if (expected > 0 && static_cast<long long>(gary::FileSizeBytes(destination)) != expected)
  {
    std::remove(destination.c_str());
    SetDecoderDownloadStatus("decoder download was incomplete; retry");
    finish();
    return;
  }
  if (mDecoderDownloadCancel.load(std::memory_order_acquire))
  {
    SetDecoderDownloadStatus("decoder download cancelled");
    finish();
    return;
  }

  mDecoderDownloadProgress.store(0.95f, std::memory_order_release);
  SetDecoderDownloadStatus("converting decoder correction to GGUF");
  const auto imported = gary::ImportLoraFile(destination);
  if (!imported.ok)
  {
    SetDecoderDownloadStatus("decoder conversion failed: " + imported.error);
    finish();
    return;
  }

  gary::SaveSetting("decoder_lora_same_l_path", imported.path);
  gary::SaveSetting("decoder_lora_same_l_enabled", "1");
  mDecoderDownloadProgress.store(1.f, std::memory_order_release);
  SetDecoderDownloadStatus("SAME-L decoder correction ready and enabled");
  finish();
}

void SA3ReaperExtension::StopDecoderLoraDownload()
{
  mDecoderDownloadCancel.store(true, std::memory_order_release);
  if (mDecoderDownloadWorker.joinable())
    mDecoderDownloadWorker.join();
  mDecoderDownloadBusy.store(false, std::memory_order_release);
}

void SA3ReaperExtension::SetDecoderDownloadStatus(const std::string& status)
{
  std::lock_guard<std::mutex> lock(mDecoderDownloadStatusMutex);
  mDecoderDownloadStatus = status;
}

std::string SA3ReaperExtension::DecoderDownloadStatus() const
{
  std::lock_guard<std::mutex> lock(mDecoderDownloadStatusMutex);
  return mDecoderDownloadStatus;
}

void SA3ReaperExtension::SetPeakNormalizeEnabled(bool enabled)
{
  mPeakNormalizeEnabled = enabled;
  gary::SaveSetting("peak_normalize_enabled", enabled ? "1" : "0");
  mSettingsNotice = enabled ? "Peak normalization enabled." : "Peak normalization disabled.";
}

void SA3ReaperExtension::SetPeakNormalizeDb(float db)
{
  mPeakNormalizeDb = std::clamp(db, -6.f, 6.f);
  gary::SaveSetting("peak_normalize_db", SettingFloat(mPeakNormalizeDb));
  mSettingsNotice = "Peak normalization target updated.";
}

void SA3ReaperExtension::SetLimiterEnabled(bool enabled)
{
  mLimiterEnabled = enabled;
  gary::SaveSetting("limiter_enabled", enabled ? "1" : "0");
  mSettingsNotice = enabled ? "Soft limiter enabled." : "Soft limiter disabled.";
}

void SA3ReaperExtension::SetLimiterCeilingDb(float db)
{
  mLimiterCeilingDb = std::clamp(db, -6.f, 0.f);
  gary::SaveSetting("limiter_ceiling_db", SettingFloat(mLimiterCeilingDb));
  mSettingsNotice = "Limiter ceiling updated.";
}

void SA3ReaperExtension::SetLimiterKnee(float knee)
{
  mLimiterKnee = std::clamp(knee, 0.1f, 1.f);
  gary::SaveSetting("limiter_knee", SettingFloat(mLimiterKnee));
  mSettingsNotice = "Limiter knee updated.";
}

void SA3ReaperExtension::ResetOutputProcessing()
{
  SetPeakNormalizeDb(2.f);
  SetLimiterCeilingDb(-0.3f);
  SetLimiterKnee(0.8f);
  SetPeakNormalizeEnabled(true);
  SetLimiterEnabled(true);
  mSettingsNotice = "Output processing reset to tuned defaults.";
  RefreshPanel(true);
}

void SA3ReaperExtension::SetRawOutputProcessing()
{
  SetPeakNormalizeEnabled(false);
  SetLimiterEnabled(false);
  mSettingsNotice = "Raw output selected; normalization and limiting are off.";
  RefreshPanel(true);
}

void SA3ReaperExtension::ReloadCreativeLoras()
{
  const auto savedLoras = gary::LoadCreativeLoraRegistry(mModelVariant);
  std::vector<CreativeLora> restored;
  restored.reserve(savedLoras.size());
  for (const auto& saved : savedLoras)
  {
    CreativeLora lora;
    lora.path = saved.path;
    lora.name = std::filesystem::path(saved.path).stem().string();
    if (lora.name.empty())
      lora.name = FileNameOnly(saved.path);
    if (gary::FileSizeBytes(saved.path) == 0)
      lora.name += " (missing)";
    lora.strength = saved.strength;
    lora.enabled = saved.enabled;
    restored.push_back(std::move(lora));
  }
  mCreativeLoras = std::move(restored);

  if (IGraphics* ui = GetUI())
  {
    if (IControl* control = ui->GetControlWithTag(kCtrlTagCreativeLoras))
      control->SetDirty(false);
  }
}

void SA3ReaperExtension::PersistCreativeLoras()
{
  std::vector<gary::PersistedCreativeLora> savedLoras;
  savedLoras.reserve(mCreativeLoras.size());
  for (const auto& lora : mCreativeLoras)
  {
    savedLoras.push_back({lora.path, lora.strength, lora.enabled});
  }
  if (!gary::SaveCreativeLoraRegistry(mModelVariant, savedLoras))
    mPanelStatus = "Could not persist the creative LoRA settings.";
}

void SA3ReaperExtension::AddCreativeLora()
{
  IGraphics* ui = GetUI();
  if (!ui)
    return;
  WDL_String fileName;
  WDL_String directory;
  ui->PromptForFile(fileName, directory, EFileAction::Open, "gguf safetensors ckpt");
  if (fileName.GetLength() == 0)
    return;

  const auto imported = gary::ImportLoraFile(fileName.Get());
  if (!imported.ok)
  {
    mPanelStatus = "LoRA import failed: " + imported.error;
    RefreshPanel(true);
    return;
  }

  CreativeLora lora;
  lora.path = imported.path;
  lora.name = imported.name.empty() ? std::filesystem::path(imported.path).stem().string() : imported.name;
  lora.strength = 1.f;
  lora.enabled = true;
  mCreativeLoras.push_back(std::move(lora));
  PersistCreativeLoras();
  mPanelStatus = "LoRA imported into the " + mModelVariant + " registry: " + mCreativeLoras.back().name + ".";
  RefreshPanel(true);
}

void SA3ReaperExtension::RemoveCreativeLora(size_t index)
{
  if (index >= mCreativeLoras.size())
    return;
  const std::string name = mCreativeLoras[index].name;
  mCreativeLoras.erase(mCreativeLoras.begin() + static_cast<ptrdiff_t>(index));
  PersistCreativeLoras();
  mPanelStatus = "Removed LoRA from the render list: " + name + ".";
  RefreshPanel(true);
}

void SA3ReaperExtension::ToggleCreativeLora(size_t index)
{
  if (index >= mCreativeLoras.size())
    return;
  mCreativeLoras[index].enabled = !mCreativeLoras[index].enabled;
  PersistCreativeLoras();
  mPanelStatus = mCreativeLoras[index].name + (mCreativeLoras[index].enabled ? " enabled." : " disabled.");
  RefreshPanel(true);
}

void SA3ReaperExtension::SetCreativeLoraStrength(size_t index, float strength)
{
  if (index >= mCreativeLoras.size())
    return;
  mCreativeLoras[index].strength = std::clamp(strength, 0.f, 2.f);
  PersistCreativeLoras();
}

void SA3ReaperExtension::SaveProjectState(ProjectStateContext* context)
{
  if (!context)
    return;
  SyncPromptFromUI();
  const std::string encoded = HexEncode(mPrompt);
  context->AddLine("SA3_REAPER_GENERATE_PROMPT %s", encoded.empty() ? "-" : encoded.c_str());
}

bool SA3ReaperExtension::LoadProjectStateLine(const char* line)
{
  if (!line)
    return false;
  while (*line == ' ' || *line == '\t')
    ++line;
  constexpr const char* prefix = "SA3_REAPER_GENERATE_PROMPT ";
  const size_t prefixLength = std::strlen(prefix);
  if (std::strncmp(line, prefix, prefixLength) != 0)
    return false;

  std::string decoded;
  if (std::strcmp(line + prefixLength, "-") == 0)
    decoded.clear();
  else if (!HexDecode(line + prefixLength, decoded))
    return true;
  mPrompt = std::move(decoded);
  if (IGraphics* ui = GetUI())
  {
    if (IControl* control = ui->GetControlWithTag(kCtrlTagPrompt))
    {
      if (auto* promptControl = control->As<ITextControl>())
      {
        promptControl->SetStr(mPrompt.c_str());
        promptControl->SetDirty(false);
      }
    }
  }
  return true;
}

void SA3ReaperExtension::OnBeginLoadProjectState(bool)
{
  mRenderService.Cancel();
  mPendingGeneration = {};
  mPrompt.clear();
  mPanelStatus = "Set a time selection and choose one destination track.";
  if (IGraphics* ui = GetUI())
  {
    if (IControl* control = ui->GetControlWithTag(kCtrlTagPrompt))
    {
      if (auto* promptControl = control->As<ITextControl>())
      {
        promptControl->SetStr("");
        promptControl->SetDirty(false);
      }
    }
  }
}

SA3ReaperExtension::SelectionSnapshot SA3ReaperExtension::ReadSelection() const
{
  SelectionSnapshot snapshot;
  snapshot.itemCount = CountSelectedMediaItems(nullptr);
  snapshot.selectedTrackCount = CountSelectedTracks(nullptr);

  if (snapshot.selectedTrackCount == 1)
  {
    if (MediaTrack* selectedTrack = GetSelectedTrack(nullptr, 0))
    {
      char selectedTrackName[256] = {};
      if (GetTrackName(selectedTrack, selectedTrackName, static_cast<int>(sizeof(selectedTrackName))))
        snapshot.selectedTrackName = selectedTrackName;
    }
  }

  GetSet_LoopTimeRange2(nullptr, false, false, &snapshot.timeStart, &snapshot.timeEnd, false);
  const double contextTime = snapshot.timeEnd > snapshot.timeStart ? snapshot.timeStart : 0.0;
  int timeSigNumerator = 4;
  TimeMap_GetTimeSigAtTime(nullptr, contextTime, &timeSigNumerator, &snapshot.beatUnit, &snapshot.bpm);
  snapshot.beatsPerMeasure = static_cast<double>(timeSigNumerator);

  if (snapshot.itemCount < 1)
    return snapshot;

  MediaItem* item = GetSelectedMediaItem(nullptr, 0);
  if (!item)
    return snapshot;

  snapshot.itemStart = GetMediaItemInfo_Value(item, "D_POSITION");
  snapshot.itemLength = GetMediaItemInfo_Value(item, "D_LENGTH");

  if (snapshot.timeEnd <= snapshot.timeStart)
  {
    TimeMap_GetTimeSigAtTime(nullptr, snapshot.itemStart, &timeSigNumerator,
                            &snapshot.beatUnit, &snapshot.bpm);
    snapshot.beatsPerMeasure = static_cast<double>(timeSigNumerator);
  }

  if (MediaTrack* track = GetMediaItemTrack(item))
  {
    char trackName[256] = {};
    if (GetTrackName(track, trackName, static_cast<int>(sizeof(trackName))))
      snapshot.trackName = trackName;
  }

  if (MediaItem_Take* take = GetActiveTake(item))
  {
    snapshot.hasActiveTake = true;
    snapshot.firstTakeIsAudio = !TakeIsMIDI(take);

    if (const char* takeName = GetTakeName(take))
      snapshot.takeName = takeName;

    if (PCM_source* source = GetMediaItemTake_Source(take))
    {
      char sourceType[64] = {};
      GetMediaSourceType(source, sourceType, static_cast<int>(sizeof(sourceType)));
      snapshot.sourceType = sourceType;
    }
  }

  return snapshot;
}

void SA3ReaperExtension::SelectOperation(Operation operation)
{
  mOperation = operation;
  switch (operation)
  {
    case Operation::Transform: mPanelStatus = "Transform audio extraction is the next implementation slice."; break;
    case Operation::Continue: mPanelStatus = "Continuation splicing is the next implementation slice."; break;
    case Operation::Generate: mPanelStatus = "Set a time selection and choose one destination track."; break;
  }
  if (*GetWindowTogglePtr() == 0)
    ShowHideMainWindow();
  RefreshPanel(true);
}

void SA3ReaperExtension::StartOrCancelGeneration()
{
  if (mRenderService.Busy())
  {
    mRenderService.Cancel();
    RefreshPanel(true);
    return;
  }

  if (mOperation != Operation::Generate)
  {
    mPanelStatus = "Only Generate is connected in this checkpoint.";
    RefreshPanel(true);
    return;
  }

  SyncPromptFromUI();
  const SelectionSnapshot selection = ReadSelection();
  const double duration = selection.timeEnd - selection.timeStart;
  if (duration < 1.0)
  {
    mPanelStatus = "Set a time selection of at least 1 second.";
    RefreshPanel(true);
    return;
  }
  if (duration > 300.0)
  {
    mPanelStatus = "This first Generate workflow supports time selections up to 300 seconds.";
    RefreshPanel(true);
    return;
  }

  MediaTrack* destinationTrack = nullptr;
  if (selection.selectedTrackCount == 1)
    destinationTrack = GetSelectedTrack(nullptr, 0);
  else if (selection.selectedTrackCount == 0 && selection.itemCount == 1)
  {
    if (MediaItem* item = GetSelectedMediaItem(nullptr, 0))
      destinationTrack = GetMediaItemTrack(item);
  }

  if (!destinationTrack)
  {
    mPanelStatus = "Select exactly one destination track before generating.";
    RefreshPanel(true);
    return;
  }

  auto request = gary::LoadSharedTextGenerationRequest(mPrompt, duration, selection.bpm);
  std::string validationError;
  if (!gary::ValidateTextGenerationRequest(request, validationError))
  {
    mPanelStatus = validationError;
    RefreshPanel(true);
    return;
  }

  mPendingGeneration.track = destinationTrack;
  mPendingGeneration.start = selection.timeStart;
  mPendingGeneration.end = selection.timeEnd;
  mPendingGeneration.prompt = mPrompt;
  if (!mRenderService.StartTextGeneration(std::move(request)))
  {
    mPendingGeneration = {};
    mPanelStatus = mRenderService.Status();
  }
  else
  {
    mPanelStatus = "Generation queued. The captured track and time range will be replaced when it finishes.";
  }
  RefreshPanel(true);
}

void SA3ReaperExtension::FinishGeneration(gary::SA3RenderResult result)
{
  const PendingGeneration pending = std::move(mPendingGeneration);
  mPendingGeneration = {};
  if (result.cancelled)
  {
    mPanelStatus = "Generation cancelled; the timeline was not changed.";
    return;
  }
  if (!result.ok)
  {
    mPanelStatus = result.error.empty() ? "Generation failed." : result.error;
    return;
  }
  if (!pending.track || !ValidatePtr2(nullptr, pending.track, "MediaTrack*"))
  {
    mPanelStatus = "Generation finished, but the captured destination track no longer exists.";
    return;
  }

  std::string pathError;
  const std::string outputPath = MakeUniqueOutputPath(pathError);
  if (outputPath.empty())
  {
    mPanelStatus = pathError;
    return;
  }

  const auto file = gary::SaveWavFile(outputPath, result.audio);
  if (!file.ok)
  {
    mPanelStatus = "Could not save generated audio: " + file.error;
    return;
  }

  std::string insertError;
  if (!ReplaceTimeSelectionWithAudio(pending.track, pending.start, pending.end, outputPath,
                                     pending.prompt, insertError))
  {
    mPanelStatus = "Generated audio was saved, but timeline insertion failed: " + insertError;
    return;
  }

  char status[320] = {};
  std::snprintf(status, sizeof(status), "Generated %.2f seconds and replaced the captured range (seed %lld).",
                pending.end - pending.start, static_cast<long long>(result.seed));
  mPanelStatus = status;
}

void SA3ReaperExtension::ContinuePendingPeakBuilds()
{
  // REAPER recommends advancing peak construction periodically. Keep each timer
  // callback short so long generated files cannot make the extension UI stall.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
  bool arrangeNeedsRefresh = false;
  size_t index = 0;
  while (index < mPendingPeakBuilds.size())
  {
    PendingPeakBuild& build = mPendingPeakBuilds[index];
    if (!build.source || !ValidatePtr2(nullptr, build.source, "PCM_source*"))
    {
      mPendingPeakBuilds.erase(mPendingPeakBuilds.begin() + static_cast<ptrdiff_t>(index));
      continue;
    }

    if (PCM_Source_BuildPeaks(build.source, 1) == 0)
    {
      PCM_Source_BuildPeaks(build.source, 2);
      if (build.item && ValidatePtr2(nullptr, build.item, "MediaItem*"))
      {
        UpdateItemInProject(build.item);
        arrangeNeedsRefresh = true;
      }
      mPendingPeakBuilds.erase(mPendingPeakBuilds.begin() + static_cast<ptrdiff_t>(index));
    }
    else
    {
      ++index;
    }

    if (std::chrono::steady_clock::now() >= deadline)
      break;
  }

  if (arrangeNeedsRefresh)
    UpdateArrange();
}

bool SA3ReaperExtension::ReplaceTimeSelectionWithAudio(MediaTrack* track, double start, double end,
                                                        const std::string& wavPath, const std::string& prompt,
                                                        std::string& error)
{
  if (!track || end <= start)
  {
    error = "invalid destination track or time range";
    return false;
  }

  PCM_source* source = PCM_Source_CreateFromFile(wavPath.c_str());
  if (!source)
  {
    error = "REAPER could not open the generated WAV";
    return false;
  }

  std::vector<MediaItem*> overlappingItems;
  const int itemCount = CountTrackMediaItems(track);
  for (int i = 0; i < itemCount; ++i)
  {
    MediaItem* item = GetTrackMediaItem(track, i);
    if (!item) continue;
    const double itemStart = GetMediaItemInfo_Value(item, "D_POSITION");
    const double itemEnd = itemStart + GetMediaItemInfo_Value(item, "D_LENGTH");
    if (itemEnd > start + 0.000001 && itemStart < end - 0.000001)
      overlappingItems.push_back(item);
  }

  Undo_BeginBlock2(nullptr);
  MediaItem* generatedItem = AddMediaItemToTrack(track);
  MediaItem_Take* generatedTake = generatedItem ? AddTakeToMediaItem(generatedItem) : nullptr;
  if (!generatedItem || !generatedTake)
  {
    if (generatedItem)
      DeleteTrackMediaItem(track, generatedItem);
    delete source;
    Undo_EndBlock2(nullptr, "SA3 generation insertion failed", -1);
    error = "REAPER could not create the generated media item";
    return false;
  }

  GetSetMediaItemTakeInfo(generatedTake, "P_SOURCE", source);
  SetMediaItemInfo_Value(generatedItem, "D_POSITION", start);
  SetMediaItemInfo_Value(generatedItem, "D_LENGTH", end - start);
  SetMediaItemInfo_Value(generatedItem, "B_LOOPSRC", 0.0);
  SetMediaItemInfo_Value(generatedItem, "B_UISEL", 1.0);
  std::string takeName = prompt.empty() ? "SA3 generation" : "SA3 - " + prompt.substr(0, 80);
  GetSetMediaItemTakeInfo_String(generatedTake, "P_NAME", takeName.data(), true);

  for (MediaItem* item : overlappingItems)
  {
    if (!ValidatePtr2(nullptr, item, "MediaItem*"))
      continue;
    const double itemStart = GetMediaItemInfo_Value(item, "D_POSITION");
    const double itemEnd = itemStart + GetMediaItemInfo_Value(item, "D_LENGTH");
    MediaItem* inside = item;
    if (itemStart < start - 0.000001)
    {
      inside = SplitMediaItem(item, start);
      if (!inside)
        continue;
    }
    if (itemEnd > end + 0.000001)
      SplitMediaItem(inside, end);
    DeleteTrackMediaItem(track, inside);
  }

  Undo_EndBlock2(nullptr, "SA3: Generate into time selection", -1);
  if (PCM_Source_BuildPeaks(source, 0) != 0)
    mPendingPeakBuilds.push_back({generatedItem, source});
  UpdateItemInProject(generatedItem);
  UpdateArrange();
  MarkProjectDirty(nullptr);
  return true;
}

std::string SA3ReaperExtension::MakeUniqueOutputPath(std::string& error) const
{
  const std::string documents = gary::DocumentsDirectory(&error);
  if (documents.empty())
    return {};

  std::error_code ec;
  const std::filesystem::path directory = std::filesystem::path(documents) / "reaper_audio";
  std::filesystem::create_directories(directory, ec);
  if (ec)
  {
    error = "could not create the REAPER generated-audio folder: " + ec.message();
    return {};
  }

  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  const std::string stem = "sa3-generate-" + std::to_string(milliseconds);
  for (int suffix = 0; suffix < 1000; ++suffix)
  {
    const std::string name = stem + (suffix == 0 ? "" : "-" + std::to_string(suffix)) + ".wav";
    const std::filesystem::path candidate = directory / name;
    if (!std::filesystem::exists(candidate, ec))
      return candidate.string();
  }
  error = "could not allocate a unique generated-audio filename";
  return {};
}

void SA3ReaperExtension::SyncPromptFromUI()
{
  IGraphics* ui = GetUI();
  if (!ui)
    return;
  IControl* control = ui->GetControlWithTag(kCtrlTagPrompt);
  auto* promptControl = control ? control->As<ITextControl>() : nullptr;
  if (!promptControl)
    return;
  const std::string nextPrompt = promptControl->GetStr();
  if (nextPrompt != mPrompt)
  {
    mPrompt = nextPrompt;
    MarkProjectDirty(nullptr);
  }
}

void SA3ReaperExtension::RefreshPanel(bool force)
{
  const SelectionSnapshot current = ReadSelection();
  if (!force && !mRenderService.Busy() && mHasSelectionSnapshot && current == mSelection)
    return;

  mSelection = current;
  mHasSelectionSnapshot = true;

  IGraphics* ui = GetUI();
  if (!ui)
    return;

  char selectionText[768] = {};
  if (current.itemCount == 0)
  {
    const char* destination = current.selectedTrackCount == 1
      ? (current.selectedTrackName.empty() ? "Unnamed track" : current.selectedTrackName.c_str())
      : current.selectedTrackCount > 1 ? "multiple tracks selected" : "no destination track selected";
    std::snprintf(selectionText, sizeof(selectionText), "No media item selected\nDestination: %s", destination);
  }
  else
  {
    const char* track = current.trackName.empty() ? "Unnamed track" : current.trackName.c_str();
    const char* take = current.takeName.empty() ? "Unnamed take" : current.takeName.c_str();
    const char* type = current.sourceType.empty() ? "unknown source" : current.sourceType.c_str();
    std::snprintf(selectionText, sizeof(selectionText),
      "%d item%s selected  |  %.2f s\n%s / %s  |  %s",
      current.itemCount, current.itemCount == 1 ? "" : "s", current.itemLength,
      track, take, type);
  }

  char timingText[256] = {};
  const double timeLength = std::max(0.0, current.timeEnd - current.timeStart);
  if (timeLength > 0.0001)
  {
    std::snprintf(timingText, sizeof(timingText),
      "Time selection: %.2f s   |   %.1f BPM   |   %.0f/%d", timeLength,
      current.bpm, current.beatsPerMeasure, current.beatUnit);
  }
  else
  {
    std::snprintf(timingText, sizeof(timingText),
      "Time selection: none   |   %.1f BPM   |   %.0f/%d", current.bpm,
      current.beatsPerMeasure, current.beatUnit);
  }

  std::string statusText;
  if (mRenderService.Busy())
  {
    char progress[256] = {};
    std::snprintf(progress, sizeof(progress), "%s  %.0f%%",
                  mRenderService.Status().c_str(), mRenderService.Progress() * 100.f);
    statusText = progress;
  }
  else
    statusText = mPanelStatus;

  SetTaggedText(kCtrlTagSelection, selectionText);
  SetTaggedText(kCtrlTagTiming, timingText);
  SetTaggedText(kCtrlTagHint, statusText.c_str());

  if (IControl* control = ui->GetControlWithTag(kCtrlTagTransform))
    control->As<SA3TabControl>()->SetActive(mOperation == Operation::Transform);
  if (IControl* control = ui->GetControlWithTag(kCtrlTagContinue))
    control->As<SA3TabControl>()->SetActive(mOperation == Operation::Continue);
  if (IControl* control = ui->GetControlWithTag(kCtrlTagGenerate))
    control->As<SA3TabControl>()->SetActive(mOperation == Operation::Generate);

  if (IControl* control = ui->GetControlWithTag(kCtrlTagRun))
  {
    auto* runButton = control->As<SA3ActionButton>();
    const bool hasDestination = current.selectedTrackCount == 1
      || (current.selectedTrackCount == 0 && current.itemCount == 1);
    if (mRenderService.Busy())
    {
      runButton->SetLabel("cancel");
      runButton->SetEnabled(true);
    }
    else if (mOperation == Operation::Generate)
    {
      runButton->SetLabel("generate into selection");
      runButton->SetEnabled(timeLength >= 1.0 && hasDestination);
    }
    else
    {
      runButton->SetLabel(mOperation == Operation::Transform ? "transform — coming next"
                                                            : "continue — coming next");
      runButton->SetEnabled(false);
    }
  }
}

void SA3ReaperExtension::SetTaggedText(int tag, const char* text)
{
  if (IGraphics* ui = GetUI())
  {
    if (IControl* control = ui->GetControlWithTag(tag))
    {
      if (auto* textControl = control->As<ITextControl>())
      {
        textControl->SetStr(text);
        textControl->SetDirty(false);
      }
    }
  }
}

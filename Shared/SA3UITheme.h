#pragma once

#include "IGraphicsStructs.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace gary::ui
{

using namespace iplug;
using namespace igraphics;

inline constexpr const char* FontName = "SA3Roboto";
inline constexpr float TitleTextSize = 20.f;
inline constexpr float BodyTextSize = 14.f;
inline constexpr float TabTextSize = 13.f;
inline constexpr float CornerRadius = 4.f;

inline IColor Background() { return IColor(255, 0, 0, 0); }
inline IColor Panel() { return IColor(255, 18, 18, 18); }
inline IColor PanelDark() { return IColor(255, 8, 8, 8); }
inline IColor Frame() { return IColor(255, 68, 68, 68); }
inline IColor FrameSoft() { return IColor(255, 38, 38, 38); }
inline IColor Red() { return IColor(255, 230, 32, 32); }
inline IColor RedDim() { return IColor(135, 230, 32, 32); }
inline IColor RedFaint() { return IColor(70, 230, 32, 32); }
inline IColor Green() { return IColor(255, 72, 210, 120); }
inline IColor TextDim() { return IColor(255, 165, 165, 165); }
inline IColor TextFaint() { return IColor(255, 105, 105, 105); }
inline IColor ButtonFill() { return IColor(255, 16, 16, 16); }
inline IColor ButtonText(bool hovered) { return hovered ? COLOR_BLACK : COLOR_WHITE; }
inline IColor ButtonBorder(bool enabled = true) { return enabled ? Red() : TextFaint(); }
inline IColor ButtonBackground(bool hovered, bool enabled = true)
{
  if (!enabled)
    return IColor(255, 12, 12, 12);
  return hovered ? Red() : ButtonFill();
}

inline void DrawButton(IGraphics& g, const IRECT& bounds, const char* label, const char* fontName,
                       bool hovered = false, bool enabled = true)
{
  g.FillRoundRect(ButtonBackground(hovered, enabled), bounds, CornerRadius);
  g.DrawRoundRect(ButtonBorder(enabled), bounds, CornerRadius);
  g.DrawText(IText(BodyTextSize, ButtonText(hovered), fontName, EAlign::Center, EVAlign::Middle),
             label, bounds.GetPadded(-4.f));
}

inline void DrawTab(IGraphics& g, const IRECT& bounds, const char* label, const char* fontName, bool active)
{
  g.FillRoundRect(active ? Red() : ButtonFill(), bounds, CornerRadius);
  g.DrawRoundRect(active ? Red() : Frame(), bounds, CornerRadius);
  g.DrawText(IText(TabTextSize, active ? COLOR_BLACK : COLOR_WHITE, fontName, EAlign::Center, EVAlign::Middle),
             label, bounds.GetPadded(-4.f));
}

inline void DrawSegment(IGraphics& g, const IRECT& bounds, const char* label, const char* fontName,
                        float textSize, bool active)
{
  g.FillRoundRect(active ? Red() : ButtonFill(), bounds, 3.f);
  g.DrawRoundRect(active ? Red() : Frame(), bounds, 3.f);
  g.DrawText(IText(textSize, active ? COLOR_BLACK : COLOR_WHITE, fontName, EAlign::Center, EVAlign::Middle),
             label, bounds);
}

// The longest start of str that fits on one line of width, cut between characters and marked with dots.
inline std::string FitText(IGraphics& g, const IText& text, const std::string& str, float width)
{
  IRECT measured;
  if (g.MeasureText(text, str.c_str(), measured) <= width)
    return str;
  size_t low = 0;
  size_t high = str.size();
  while (low < high)
  {
    const size_t middle = (low + high + 1) / 2;
    const std::string candidate = str.substr(0, middle) + "...";
    if (g.MeasureText(text, candidate.c_str(), measured) <= width)
      low = middle;
    else
      high = middle - 1;
  }
  while (low > 0 && low < str.size() && (static_cast<unsigned char>(str[low]) & 0xC0) == 0x80)
    --low;
  return str.substr(0, low) + "...";
}

inline float ScrollThumbHeight(const IRECT& track, size_t visible, size_t total)
{
  return std::max(18.f, track.H() * static_cast<float>(visible) / static_cast<float>(std::max<size_t>(1, total)));
}

inline void DrawScrollbar(IGraphics& g, const IRECT& track, size_t first, size_t visible, size_t total)
{
  if (total <= visible)
    return;
  g.FillRoundRect(FrameSoft(), track, 2.f);
  const float thumbHeight = ScrollThumbHeight(track, visible, total);
  const float top = track.T + (track.H() - thumbHeight) * static_cast<float>(first) / static_cast<float>(total - visible);
  g.FillRoundRect(TextDim(), IRECT(track.L, top, track.R, top + thumbHeight), 2.f);
}

// The first row to show with the middle of the scrollbar's thumb at y.
inline size_t ScrollbarFirstAt(const IRECT& track, float y, size_t visible, size_t total)
{
  if (total <= visible)
    return 0;
  const float thumbHeight = ScrollThumbHeight(track, visible, total);
  const float fraction = (y - track.T - thumbHeight * 0.5f) / std::max(1.f, track.H() - thumbHeight);
  return static_cast<size_t>(std::clamp(fraction, 0.f, 1.f) * static_cast<float>(total - visible) + 0.5f);
}

}


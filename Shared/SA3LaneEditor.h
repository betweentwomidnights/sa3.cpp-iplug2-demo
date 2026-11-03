#pragma once

#include "IControl.h"
#include "ITextEntryControl.h"
#include "SA3LaneModel.h"
#include "SA3UITheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gary::ui
{

// Opens text for editing as an overlay on the control: it wraps, and grows to show all of it.
inline void OpenPromptEntry(IControl& control, const IRECT& bounds, const char* text, float textSize, int valIdx = 0)
{
  IGraphics* ui = control.GetUI();
  if (!ui)
    return;
  if (auto* entry = ui->GetTextEntryControl())
    entry->SetNextEditWraps(3, Frame());
  ui->CreateTextEntry(control,
    IText(textSize, COLOR_WHITE, FontName, EAlign::Near, EVAlign::Top).WithTEColors(PanelDark(), COLOR_WHITE),
    bounds, text, valIdx);
}

// Stacked per-LoRA curves over a shared time axis, where a curve's height is the LoRA's strength.
// Redraws only itself, so dragging a point stays responsive however heavy the rest of the host's UI
// is. Below kWideWidth each lane's controls sit above its curve instead of beside it, so the same
// control fits a narrow plugin window, and squeeze onto one line once that would crowd the curves.
// The host owns the lanes and decides when one opens.
class SA3LaneEditorControl final : public IControl
{
public:
  struct Host
  {
    std::function<std::string(const SA3Lane&)> rollPrompt;   // empty when the LoRA has no pool
    std::function<bool(const SA3Lane&)> isMissing;           // its LoRA is no longer available
    std::function<float(const SA3Lane&)> resetStrength;      // where the reset button flattens a curve
    std::function<void(const std::string&)> notify;
    std::function<void()> changed;
    bool canSnap = true;                                     // follows a host's tempo, so not in a standalone
  };

  SA3LaneEditorControl(const IRECT& bounds, std::vector<SA3Lane>& lanes, SA3LaneBlend& blend, SA3LaneSnap& snap, Host host)
  : IControl(bounds)
  , mLanes(lanes)
  , mBlend(blend)
  , mSnap(snap)
  , mHost(std::move(host))
  {
    SetTextEntryLength(1024);
  }

  static IColor LaneColor(int index)
  {
    static const IColor colors[] = {
      IColor(255, 226, 160, 59), IColor(255, 79, 179, 217), IColor(255, 176, 123, 224), IColor(255, 108, 197, 139),
      IColor(255, 229, 103, 127), IColor(255, 214, 214, 90), IColor(255, 95, 143, 240), IColor(255, 224, 138, 92)};
    return colors[static_cast<size_t>(index) % (sizeof(colors) / sizeof(colors[0]))];
  }

  void SetTimeline(double seconds, double bpm, double beatsPerBar)
  {
    if (seconds == mSeconds && bpm == mBpm && beatsPerBar == mBeatsPerBar)
      return;
    mSeconds = seconds;
    mBpm = bpm;
    mBeatsPerBar = beatsPerBar;
    SetDirty(false);
  }

  // Where playback is, in seconds from the start of the curves. Anything outside them hides the cursor.
  void SetPlayhead(double seconds)
  {
    const float position = mSeconds > 0.0 && seconds >= 0.0 && seconds <= mSeconds
                         ? static_cast<float>(seconds / mSeconds) : -1.f;
    if (position == mPlayhead)
      return;
    mPlayhead = position;
    SetDirty(false);
  }

  // Min/max pairs across the range, drawn behind every curve in the strip above the lanes.
  void SetBackdrop(std::vector<float> peaks)
  {
    mPeaks = std::move(peaks);
    SetDirty(false);
  }

  void LanesReplaced()
  {
    mFirstLane = 0;
    mDrag = Drag::None;
    mHoverLane = -1;
    mHoverPoint = -1;
    SetDirty(false);
  }

  // Scrolls a lane into view, for when the host has just opened it.
  void ShowLane(size_t index)
  {
    Layout();
    if (index < mFirstLane)
      mFirstLane = index;
    else if (!mRows.empty() && index >= mFirstLane + mRows.size())
      mFirstLane = index - mRows.size() + 1;
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    Layout();
    g.FillRoundRect(PanelDark(), mRECT, CornerRadius);
    g.DrawRoundRect(FrameSoft(), mRECT, CornerRadius);
    DrawTopBar(g);
    DrawTake(g);
    if (mLanes.empty())
    {
      const IRECT empty(mRECT.L, mTakeCanvas.B, mRECT.R, mRECT.B);
      g.DrawText(IText(12.f, TextDim(), FontName, EAlign::Center, EVAlign::Middle),
                 "add a LoRA to draw its strength over the range", empty);
      return;
    }
    for (size_t i = 0; i < mRows.size(); ++i)
      DrawLane(g, mRows[i], mLanes[mFirstLane + i], static_cast<int>(mFirstLane + i));
    DrawScrollbar(g, mScrollTrack, mFirstLane, mRows.size(), mLanes.size());
    DrawPlayhead(g);
    DrawReadout(g);
  }

  void OnResize() override
  {
    Layout();
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    Layout();
    if (mBlendOneRect.Contains(x, y) || mBlendDistinctRect.Contains(x, y))
    {
      const SA3LaneBlend blend = mBlendOneRect.Contains(x, y) ? SA3LaneBlend::OneSong : SA3LaneBlend::Distinct;
      if (blend != mBlend)
      {
        mBlend = blend;
        Changed();
      }
      return;
    }
    if (mSnapRect.Contains(x, y))
    {
      OpenSnapMenu();
      return;
    }
    if (mScrollTrack.Contains(x, y))
    {
      mDrag = Drag::Scroll;
      ScrollTo(y);
      return;
    }

    for (size_t i = 0; i < mRows.size(); ++i)
    {
      const Row& row = mRows[i];
      const size_t index = mFirstLane + i;
      SA3Lane& lane = mLanes[index];
      // Every LoRA has a lane, so only one whose LoRA has gone can be removed from here. The others
      // have a reset button in that spot.
      if (row.remove.Contains(x, y))
      {
        if (Missing(lane))
          mLanes.erase(mLanes.begin() + static_cast<std::ptrdiff_t>(index));
        else
          ResetCurve(lane);
        mHoverLane = -1;
        mHoverPoint = -1;
        Changed();
        return;
      }
      if (row.dice.Contains(x, y))
      {
        RollPrompt(lane);
        return;
      }
      if (row.prompt.Contains(x, y))
      {
        // Wider than the box, which can be narrow, and over whatever is beside it until the edit ends.
        OpenPromptEntry(*this, IRECT(row.head.L, row.prompt.T, row.canvas.R, row.prompt.B), lane.prompt.c_str(), 12.f,
                        static_cast<int>(index));
        return;
      }
      if (row.canvas.Contains(x, y))
      {
        CanvasDown(x, y, mod, row, lane, static_cast<int>(index));
        return;
      }
    }
  }

  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override
  {
    Layout();
    for (size_t i = 0; i < mRows.size(); ++i)
    {
      if (!mRows[i].canvas.Contains(x, y))
        continue;
      SA3Lane& lane = mLanes[mFirstLane + i];
      const int hit = HitPoint(mRows[i], lane, x, y);
      if (hit >= 0)
      {
        RemovePoint(lane, hit);
        mDrag = Drag::None;
        return;
      }
    }
    OnMouseDown(x, y, mod);
  }

  void OnMouseDrag(float x, float y, float, float, const IMouseMod& mod) override
  {
    if (mDrag == Drag::Scroll)
    {
      ScrollTo(y);
      return;
    }
    if (mDrag == Drag::None || mDragLane < 0 || mDragLane >= static_cast<int>(mLanes.size()))
      return;
    Layout();
    SA3Lane& lane = mLanes[static_cast<size_t>(mDragLane)];
    const int rowIndex = mDragLane - static_cast<int>(mFirstLane);
    if (rowIndex < 0 || rowIndex >= static_cast<int>(mRows.size()))
      return;
    const Row& row = mRows[static_cast<size_t>(rowIndex)];

    float position = PositionAt(row.canvas, x);
    float amount = AmountAt(row.canvas, y);
    Snap(position, amount, mod, mDrag == Drag::Stroke);
    mHoverLane = mDragLane;
    mHoverX = x;
    mHoverY = y;
    mReadoutPosition = position;
    mReadoutAmount = amount;

    if (mDrag == Drag::Point)
    {
      if (mDragPoint < 0 || mDragPoint >= static_cast<int>(lane.points.size()))
        return;
      const size_t i = static_cast<size_t>(mDragPoint);
      const float low = i > 0 ? lane.points[i - 1].position : 0.f;
      const float high = i + 1 < lane.points.size() ? lane.points[i + 1].position : 1.f;
      position = std::clamp(position, low, high);
      mReadoutPosition = position;
      lane.points[i] = {position, amount};
      mHoverPoint = mDragPoint;
    }
    else if (mDrag == Drag::Stroke)
    {
      mStroke.push_back({position, amount});
      ApplyStroke(lane);
    }
    SetDirty(false);
  }

  void OnMouseUp(float, float, const IMouseMod&) override
  {
    if (mDrag == Drag::None)
      return;
    const bool edited = mDrag != Drag::Scroll;
    mDrag = Drag::None;
    mStroke.clear();
    if (edited)
      Changed();
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    Layout();
    int lane = -1;
    int point = -1;
    bool overCanvas = false;
    bool overControl = mBlendOneRect.Contains(x, y) || mBlendDistinctRect.Contains(x, y) || mSnapRect.Contains(x, y)
                    || mScrollTrack.Contains(x, y);
    for (size_t i = 0; i < mRows.size(); ++i)
    {
      const Row& row = mRows[i];
      overControl = overControl || row.remove.Contains(x, y) || row.dice.Contains(x, y);
      if (row.canvas.Contains(x, y))
      {
        overCanvas = true;
        lane = static_cast<int>(mFirstLane + i);
        point = HitPoint(row, mLanes[static_cast<size_t>(lane)], x, y);
        mReadoutPosition = point >= 0 ? mLanes[static_cast<size_t>(lane)].points[static_cast<size_t>(point)].position
                                      : PositionAt(row.canvas, x);
        mReadoutAmount = point >= 0 ? mLanes[static_cast<size_t>(lane)].points[static_cast<size_t>(point)].amount
                                    : AmountAt(row.canvas, y);
        if (point < 0)   // says where a click would land
          Snap(mReadoutPosition, mReadoutAmount, mod, mod.S);
      }
    }
    SetCursor(point >= 0 || overControl ? ECursor::HAND : overCanvas ? ECursor::CROSS : ECursor::ARROW);
    // Only the curve under the mouse needs a redraw, and only while the readout is showing.
    const bool dirty = lane != mHoverLane || point != mHoverPoint || overCanvas;
    mHoverLane = lane;
    mHoverPoint = point;
    mHoverX = x;
    mHoverY = y;
    IControl::OnMouseOver(x, y, IMouseMod());
    if (dirty)
      SetDirty(false);
  }

  void OnMouseOut() override
  {
    SetCursor(ECursor::ARROW);
    mHoverLane = -1;
    mHoverPoint = -1;
    IControl::OnMouseOut();
    SetDirty(false);
  }

  void OnMouseWheel(float, float, const IMouseMod&, float d) override
  {
    Layout();
    if (mLanes.size() <= mRows.size())
      return;
    const size_t last = mLanes.size() - mRows.size();
    if (d < 0.f && mFirstLane < last)
      ++mFirstLane;
    else if (d > 0.f && mFirstLane > 0)
      --mFirstLane;
    SetDirty(false);
  }

  void OnPopupMenuSelection(IPopupMenu* menu, int) override
  {
    if (!menu || menu->GetChosenItemIdx() < 0)
      return;
    mSnap = LaneSnapFromIndex(menu->GetChosenItemIdx());
    Changed();
  }

  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    if (valIdx < 0 || valIdx >= static_cast<int>(mLanes.size()) || !str)
      return;
    mLanes[static_cast<size_t>(valIdx)].prompt = str;
    Changed();
  }

private:
  static constexpr float kWideWidth = 620.f;
  static constexpr float kPad = 14.f;   // leaves the strength labels clear of a point at the very start
  static constexpr float kCurvePad = 5.f;

  enum class Drag
  {
    None,
    Point,
    Stroke,
    Scroll
  };

  struct Row
  {
    IRECT head;
    IRECT canvas;
    IRECT name;
    IRECT remove;
    IRECT prompt;
    IRECT dice;
  };

  void Layout()
  {
    const bool wide = mRECT.W() >= kWideWidth;
    const IRECT inner = mRECT.GetPadded(-6.f);
    const IRECT bar(inner.L, inner.T, inner.R, inner.T + 22.f);
    mTitleRect = IRECT(bar.L, bar.T, bar.L + 74.f, bar.B);
    mBlendOneRect = IRECT(mTitleRect.R + 4.f, bar.T, mTitleRect.R + 70.f, bar.B);
    mBlendDistinctRect = IRECT(mBlendOneRect.R, bar.T, mBlendOneRect.R + 60.f, bar.B);
    mSnapRect = mHost.canSnap ? IRECT(mBlendDistinctRect.R + 6.f, bar.T, mBlendDistinctRect.R + 76.f, bar.B) : IRECT();
    mRangeRect = IRECT((mHost.canSnap ? mSnapRect.R : mBlendDistinctRect.R) + 8.f, bar.T, bar.R, bar.B);

    const float headWidth = wide ? 196.f : 0.f;
    const float canvasLeft = inner.L + headWidth;
    const float space = inner.B - bar.B - 4.f;
    const float takeHeight = std::clamp(space * 0.2f, 36.f, 70.f);
    mTakeCanvas = IRECT(canvasLeft, bar.B + 4.f, inner.R, bar.B + 4.f + takeHeight);
    mTakeHead = IRECT(inner.L, mTakeCanvas.T, canvasLeft - 6.f, mTakeCanvas.B);

    mRows.clear();
    mScrollTrack = {};
    const float top = mTakeCanvas.B + 4.f;
    const float available = std::max(0.f, inner.B - top);
    const float minimum = wide ? 56.f : 60.f;
    const size_t fit = std::max<size_t>(1, static_cast<size_t>(available / minimum));
    const size_t visible = std::min(fit, mLanes.size());
    if (visible == 0)
    {
      mFirstLane = 0;
      return;
    }
    mFirstLane = std::min(mFirstLane, mLanes.size() - visible);
    const float rowHeight = std::min(available / static_cast<float>(visible), wide ? 140.f : 280.f);
    const float right = visible < mLanes.size() ? inner.R - 9.f : inner.R;
    if (visible < mLanes.size())
      mScrollTrack = IRECT(inner.R - 5.f, top, inner.R, top + rowHeight * static_cast<float>(visible) - 3.f);
    // Two lines of controls take 44 of a row, which is too much of a short one.
    const bool oneLine = !wide && rowHeight - 47.f < 70.f;
    for (size_t i = 0; i < visible; ++i)
    {
      const float y = top + rowHeight * static_cast<float>(i);
      const IRECT rowRect(inner.L, y, right, y + rowHeight - 3.f);
      Row row;
      row.head = wide ? IRECT(rowRect.L, rowRect.T, canvasLeft - 6.f, rowRect.B)
                      : IRECT(rowRect.L, rowRect.T, rowRect.R, rowRect.T + (oneLine ? 20.f : 42.f));
      row.canvas = wide ? IRECT(canvasLeft, rowRect.T, rowRect.R, rowRect.B)
                        : IRECT(rowRect.L, row.head.B + 2.f, rowRect.R, rowRect.B);
      const IRECT first(row.head.L, row.head.T, row.head.R, row.head.T + 20.f);
      row.remove = IRECT(first.R - 18.f, first.T + 1.f, first.R, first.B - 1.f);
      if (oneLine)
      {
        row.dice = IRECT(row.remove.L - 26.f, first.T, row.remove.L - 4.f, first.B);
        row.name = IRECT(first.L, first.T + 1.f, first.L + 104.f, first.B - 1.f);
        row.prompt = IRECT(row.name.R + 4.f, first.T, row.dice.L - 4.f, first.B);
      }
      else
      {
        row.name = IRECT(first.L, first.T + 1.f, row.remove.L - 4.f, first.B - 1.f);
        const IRECT second(row.head.L, first.B + 1.f, row.head.R, first.B + 21.f);
        row.dice = IRECT(second.R - 22.f, second.T, second.R, second.B);
        row.prompt = IRECT(second.L, second.T, row.dice.L - 4.f, second.B);
      }
      mRows.push_back(row);
    }
  }

  float XAt(const IRECT& canvas, float position) const
  {
    return canvas.L + kPad + position * (canvas.W() - 2.f * kPad);
  }

  float YAt(const IRECT& canvas, float amount) const
  {
    return canvas.B - kCurvePad - amount / kSA3MaxLaneAmount * (canvas.H() - 2.f * kCurvePad);
  }

  float PositionAt(const IRECT& canvas, float x) const
  {
    return std::clamp((x - canvas.L - kPad) / std::max(1.f, canvas.W() - 2.f * kPad), 0.f, 1.f);
  }

  float AmountAt(const IRECT& canvas, float y) const
  {
    return std::clamp((canvas.B - kCurvePad - y) / std::max(1.f, canvas.H() - 2.f * kCurvePad), 0.f, 1.f) * kSA3MaxLaneAmount;
  }

  // Points land on the chosen division, and freehand strokes stay free. Ctrl steps the other way:
  // free of the division while one is chosen, and onto the beat while none is.
  void Snap(float& position, float& amount, const IMouseMod& mod, bool freehand) const
  {
    const double division = mHost.canSnap ? LaneSnapSeconds(mSnap, mBpm, mBeatsPerBar) : 0.0;
    double step = 0.0;
    if (mod.C)
    {
      const double beat = mBpm > 0.0 ? 60.0 / mBpm : 0.0;
      if (division <= 0.0)
        step = (mSeconds > 0.0 && beat > 0.0) ? beat / mSeconds : 1.0 / 16.0;
      amount = std::round(amount / 0.05f) * 0.05f;
    }
    else if (!freehand && mSeconds > 0.0)
      step = division / mSeconds;
    if (step <= 0.0)
      return;
    const double snapped = std::min(1.0, std::round(position / step) * step);
    // The range rarely ends on the grid, but its end stays reachable.
    position = static_cast<float>(1.0 - position < std::abs(position - snapped) ? 1.0 : snapped);
  }

  int HitPoint(const Row& row, const SA3Lane& lane, float x, float y) const
  {
    for (size_t i = 0; i < lane.points.size(); ++i)
    {
      const float dx = XAt(row.canvas, lane.points[i].position) - x;
      const float dy = YAt(row.canvas, lane.points[i].amount) - y;
      if (dx * dx + dy * dy < 64.f)
        return static_cast<int>(i);
    }
    return -1;
  }

  void CanvasDown(float x, float y, const IMouseMod& mod, const Row& row, SA3Lane& lane, int laneIndex)
  {
    const int hit = HitPoint(row, lane, x, y);
    if (mod.R)
    {
      if (hit >= 0)
        RemovePoint(lane, hit);
      return;
    }

    mDragLane = laneIndex;
    if (hit >= 0)
    {
      mDrag = Drag::Point;
      mDragPoint = hit;
      return;
    }

    float position = PositionAt(row.canvas, x);
    float amount = AmountAt(row.canvas, y);
    Snap(position, amount, mod, mod.S);
    if (mod.S)
    {
      mDrag = Drag::Stroke;
      mStroke.assign(1, {position, amount});
      return;
    }
    if (lane.points.size() >= kSA3MaxLanePoints)
    {
      if (mHost.notify)
        mHost.notify("This lane is at its point limit.");
      return;
    }
    const auto at = std::upper_bound(lane.points.begin(), lane.points.end(), position,
                                     [](float p, const SA3LanePoint& point) { return p < point.position; });
    mDragPoint = static_cast<int>(lane.points.insert(at, {position, amount}) - lane.points.begin());
    mDrag = Drag::Point;
    mHoverLane = laneIndex;
    mHoverPoint = mDragPoint;
    SetDirty(false);
  }

  // The stroke replaces whatever was under it, keeping a point about every tenth of a second.
  void ApplyStroke(SA3Lane& lane)
  {
    std::vector<SA3LanePoint> byPosition = mStroke;
    SortLanePoints(byPosition);
    const float from = byPosition.front().position;
    const float to = byPosition.back().position;
    std::vector<SA3LanePoint> merged;
    for (const SA3LanePoint& point : lane.points)
      if (point.position < from - 1e-6f || point.position > to + 1e-6f)
        merged.push_back(point);
    const float spacing = mSeconds > 0.0 ? static_cast<float>(0.1 / mSeconds) : 0.005f;
    float lastKept = -1.f;
    for (const SA3LanePoint& point : byPosition)
    {
      if (lastKept < 0.f || point.position - lastKept >= spacing)
      {
        merged.push_back(point);
        lastKept = point.position;
      }
    }
    if (lastKept < to)
      merged.push_back(byPosition.back());
    SortLanePoints(merged);
    lane.points = std::move(merged);
  }

  void RemovePoint(SA3Lane& lane, int index)
  {
    lane.points.erase(lane.points.begin() + index);
    mHoverPoint = -1;
    Changed();
  }

  void ResetCurve(SA3Lane& lane)
  {
    const float strength = mHost.resetStrength ? mHost.resetStrength(lane) : 0.f;
    lane.points = {{0.f, strength}, {1.f, strength}};
  }

  void RollPrompt(SA3Lane& lane)
  {
    if (!mHost.rollPrompt)
      return;
    std::string prompt = mHost.rollPrompt(lane);
    if (prompt.empty())
      return;
    lane.prompt = std::move(prompt);
    Changed();
  }

  void ScrollTo(float y)
  {
    mFirstLane = ScrollbarFirstAt(mScrollTrack, y, mRows.size(), mLanes.size());
    SetDirty(false);
  }

  void OpenSnapMenu()
  {
    if (!GetUI())
      return;
    mSnapMenu.Clear();
    for (const char* label : {"off", "1/4", "1/2", "1 bar"})
      mSnapMenu.AddItem(label);
    mSnapMenu.CheckItem(static_cast<int>(mSnap), true);
    GetUI()->CreatePopupMenu(*this, mSnapMenu, mSnapRect);
  }

  void SetCursor(ECursor cursor)
  {
    if (cursor == mCursor)
      return;
    mCursor = cursor;
    if (IGraphics* ui = GetUI())
      ui->SetMouseCursor(cursor);
  }

  void Changed()
  {
    SetDirty(false);
    if (mHost.changed)
      mHost.changed();
  }

  bool Missing(const SA3Lane& lane) const
  {
    return mHost.isMissing && mHost.isMissing(lane);
  }

  void DrawTopBar(IGraphics& g)
  {
    g.DrawText(IText(13.f, COLOR_WHITE, FontName, EAlign::Near, EVAlign::Middle), "automation", mTitleRect);
    const auto segment = [&](const IRECT& rect, const char* label, bool active) {
      g.FillRect(active ? Red() : ButtonFill(), rect);
      g.DrawRect(active ? Red() : Frame(), rect);
      g.DrawText(IText(11.f, active ? COLOR_BLACK : TextDim(), FontName, EAlign::Center, EVAlign::Middle), label, rect);
    };
    segment(mBlendOneRect, "one song", mBlend == SA3LaneBlend::OneSong);
    segment(mBlendDistinctRect, "distinct", mBlend == SA3LaneBlend::Distinct);

    if (mHost.canSnap)
    {
      static const char* labels[] = {"snap off", "snap 1/4", "snap 1/2", "snap 1 bar"};
      const bool on = mSnap != SA3LaneSnap::Off;
      g.FillRect(ButtonFill(), mSnapRect);
      g.DrawRect(on ? Red() : Frame(), mSnapRect);
      g.DrawText(IText(10.f, on ? COLOR_WHITE : TextDim(), FontName, EAlign::Center, EVAlign::Middle),
                 labels[static_cast<int>(mSnap)], mSnapRect);
    }

    std::string range = "set a range";
    if (mSeconds > 0.0)
    {
      char text[64] = {};
      std::snprintf(text, sizeof(text), "%.2f s", mSeconds);
      range = text;
    }
    g.DrawText(IText(11.f, TextDim(), FontName, EAlign::Far, EVAlign::Middle), range.c_str(), mRangeRect);
  }

  void DrawGrid(IGraphics& g, const IRECT& canvas, bool labels)
  {
    if (mSeconds <= 0.0)
      return;
    const float width = canvas.W() - 2.f * kPad;
    const double beat = mBpm > 0.0 ? 60.0 / mBpm : 0.0;
    const double bar = beat * std::max(1.0, mBeatsPerBar);
    if (bar <= 0.0)
      return;
    const float barWidth = static_cast<float>(bar / mSeconds) * width;
    const float beatWidth = static_cast<float>(beat / mSeconds) * width;
    if (beatWidth >= 14.f)
    {
      for (double t = beat; t < mSeconds; t += beat)
        g.DrawVerticalLine(IColor(255, 24, 24, 24), canvas, XAt(canvas, static_cast<float>(t / mSeconds)));
    }
    const int every = std::max(1, static_cast<int>(std::ceil(28.f / std::max(1.f, barWidth))));
    int barIndex = 0;
    for (double t = 0.0; t < mSeconds; t += bar, ++barIndex)
    {
      if (barIndex % every != 0)
        continue;
      const float x = XAt(canvas, static_cast<float>(t / mSeconds));
      g.DrawVerticalLine(FrameSoft(), canvas, x);
      if (labels)
      {
        char label[16] = {};
        std::snprintf(label, sizeof(label), "%d", barIndex + 1);
        g.DrawText(IText(10.f, TextFaint(), FontName, EAlign::Near, EVAlign::Top), label,
                   IRECT(x + 3.f, canvas.T + 1.f, x + 34.f, canvas.T + 13.f));
      }
    }
  }

  void PathCurve(IGraphics& g, const IRECT& canvas, const std::vector<SA3LanePoint>& points)
  {
    g.PathClear();
    g.PathMoveTo(canvas.L + kPad, YAt(canvas, points.front().amount));
    for (const SA3LanePoint& point : points)
      g.PathLineTo(XAt(canvas, point.position), YAt(canvas, point.amount));
    g.PathLineTo(canvas.R - kPad, YAt(canvas, points.back().amount));
  }

  void DrawTake(IGraphics& g)
  {
    if (mTakeHead.W() > 10.f)
      g.DrawText(IText(11.f, TextDim(), FontName, EAlign::Near, EVAlign::Top),
                 mPeaks.empty() ? "no render yet" : "last render", mTakeHead);
    g.FillRect(IColor(255, 12, 12, 12), mTakeCanvas);
    DrawGrid(g, mTakeCanvas, true);

    if (mPeaks.size() >= 2)
    {
      const size_t columns = mPeaks.size() / 2;
      const float width = mTakeCanvas.W() - 2.f * kPad;
      const int steps = std::min(static_cast<int>(columns), static_cast<int>(width));
      const float middle = mTakeCanvas.MH();
      const float scale = mTakeCanvas.H() * 0.46f;
      g.PathClear();
      for (int i = 0; i < steps; ++i)
      {
        const size_t column = static_cast<size_t>(i) * columns / static_cast<size_t>(std::max(1, steps));
        const float x = mTakeCanvas.L + kPad + width * static_cast<float>(i) / static_cast<float>(std::max(1, steps));
        g.PathMoveTo(x, middle - mPeaks[column * 2 + 1] * scale);
        g.PathLineTo(x, middle - mPeaks[column * 2] * scale + 0.5f);
      }
      g.PathStroke(IColor(255, 80, 80, 80), 1.f);
    }

    for (size_t i = 0; i < mLanes.size(); ++i)
    {
      if (mLanes[i].points.empty() || Missing(mLanes[i]))
        continue;
      PathCurve(g, mTakeCanvas, mLanes[i].points);
      g.PathStroke(LaneColor(static_cast<int>(i)), 1.5f);
    }
    g.DrawRect(FrameSoft(), mTakeCanvas);
  }

  void DrawDice(IGraphics& g, const IRECT& rect)
  {
    g.FillRoundRect(ButtonFill(), rect, CornerRadius);
    g.DrawRoundRect(Frame(), rect, CornerRadius);
    const float left = rect.L + rect.W() * 0.31f;
    const float right = rect.R - rect.W() * 0.31f;
    const float top = rect.T + rect.H() * 0.31f;
    const float bottom = rect.B - rect.H() * 0.31f;
    g.FillCircle(COLOR_WHITE, left, top, 1.4f);
    g.FillCircle(COLOR_WHITE, right, top, 1.4f);
    g.FillCircle(COLOR_WHITE, rect.MW(), rect.MH(), 1.4f);
    g.FillCircle(COLOR_WHITE, left, bottom, 1.4f);
    g.FillCircle(COLOR_WHITE, right, bottom, 1.4f);
  }

  void DrawReset(IGraphics& g, const IRECT& rect)
  {
    g.FillRoundRect(ButtonFill(), rect, CornerRadius);
    g.DrawRoundRect(Frame(), rect, CornerRadius);
    const float cx = rect.MW();
    const float cy = rect.MH();
    const float radius = std::min(rect.W(), rect.H()) * 0.27f;
    g.DrawArc(COLOR_WHITE, cx, cy, radius, 70.f, 360.f, nullptr, 1.3f);
    const float tipY = cy - radius;
    g.FillTriangle(COLOR_WHITE, cx + 3.2f, tipY, cx - 0.4f, tipY - 2.6f, cx - 0.4f, tipY + 2.6f);
  }

  void DrawLane(IGraphics& g, const Row& row, const SA3Lane& lane, int index)
  {
    const bool missing = Missing(lane);
    const IColor color = missing ? TextFaint() : LaneColor(index);
    g.FillRoundRect(ButtonFill(), row.name, CornerRadius);
    g.DrawRoundRect(color, row.name, CornerRadius);
    const std::string label = missing ? lane.name + " (missing)" : lane.name;
    g.DrawText(IText(11.f, missing ? TextDim() : COLOR_WHITE, FontName, EAlign::Near, EVAlign::Middle),
               FitText(g, IText(11.f, COLOR_WHITE, FontName), label, row.name.W() - 10.f).c_str(), row.name.GetHPadded(-5.f));
    if (missing)
      DrawButton(g, row.remove, "x", FontName);
    else
      DrawReset(g, row.remove);

    g.FillRoundRect(IColor(255, 14, 14, 14), row.prompt, CornerRadius);
    g.DrawRoundRect(FrameSoft(), row.prompt, CornerRadius);
    const IRECT promptText = row.prompt.GetHPadded(-5.f);
    if (lane.prompt.empty())
      g.DrawText(IText(11.f, TextFaint(), FontName, EAlign::Near, EVAlign::Middle), "uses the base prompt", promptText);
    else
      g.DrawText(IText(11.f, missing ? TextDim() : COLOR_WHITE, FontName, EAlign::Near, EVAlign::Middle),
                 FitText(g, IText(11.f, COLOR_WHITE, FontName), lane.prompt, promptText.W()).c_str(), promptText);
    DrawDice(g, row.dice);

    g.FillRect(IColor(255, 12, 12, 12), row.canvas);
    DrawGrid(g, row.canvas, false);
    const float unity = YAt(row.canvas, 1.f);
    g.DrawHorizontalLine(Frame(), row.canvas, unity);
    const IText scale(9.f, TextFaint(), FontName, EAlign::Near, EVAlign::Middle);
    g.DrawText(scale, "2", IRECT(row.canvas.L + 3.f, row.canvas.T + 1.f, row.canvas.L + kPad, row.canvas.T + 12.f));
    g.DrawText(scale, "1", IRECT(row.canvas.L + 3.f, unity - 11.f, row.canvas.L + kPad, unity));
    g.DrawText(scale, "0", IRECT(row.canvas.L + 3.f, row.canvas.B - 12.f, row.canvas.L + kPad, row.canvas.B - 1.f));
    if (!lane.points.empty())
    {
      PathCurve(g, row.canvas, lane.points);
      g.PathLineTo(row.canvas.R - kPad, row.canvas.B - kCurvePad);
      g.PathLineTo(row.canvas.L + kPad, row.canvas.B - kCurvePad);
      g.PathClose();
      g.PathFill(IColor(44, color.R, color.G, color.B));
      PathCurve(g, row.canvas, lane.points);
      g.PathStroke(color, 2.f);
      for (size_t i = 0; i < lane.points.size(); ++i)
      {
        const bool hovered = index == mHoverLane && static_cast<int>(i) == mHoverPoint;
        const float x = XAt(row.canvas, lane.points[i].position);
        const float y = YAt(row.canvas, lane.points[i].amount);
        g.FillCircle(hovered ? COLOR_WHITE : color, x, y, hovered ? 5.f : 3.5f);
      }
    }
    g.DrawRect(FrameSoft(), row.canvas);
  }

  void DrawPlayhead(IGraphics& g)
  {
    if (mPlayhead < 0.f)
      return;
    const auto line = [&](const IRECT& canvas) {
      const float x = XAt(canvas, mPlayhead);
      g.DrawLine(COLOR_WHITE, x, canvas.T, x, canvas.B, nullptr, 1.5f);
    };
    line(mTakeCanvas);
    for (const Row& row : mRows)
      line(row.canvas);
  }

  void DrawReadout(IGraphics& g)
  {
    if (mHoverLane < 0)
      return;
    const int rowIndex = mHoverLane - static_cast<int>(mFirstLane);
    if (rowIndex < 0 || rowIndex >= static_cast<int>(mRows.size()))
      return;
    const IRECT& canvas = mRows[static_cast<size_t>(rowIndex)].canvas;
    char text[48] = {};
    if (mSeconds > 0.0)
      std::snprintf(text, sizeof(text), "%.2f s  ·  %.2f", mReadoutPosition * mSeconds, mReadoutAmount);
    else
      std::snprintf(text, sizeof(text), "%.0f%%  ·  %.2f", mReadoutPosition * 100.f, mReadoutAmount);
    const float left = std::min(mHoverX + 10.f, canvas.R - 86.f);
    const float top = std::max(canvas.T + 1.f, mHoverY - 20.f);
    const IRECT tip(left, top, left + 84.f, top + 16.f);
    g.FillRoundRect(IColor(230, 0, 0, 0), tip, 2.f);
    g.DrawText(IText(10.f, COLOR_WHITE, FontName, EAlign::Center, EVAlign::Middle), text, tip);
  }

  std::vector<SA3Lane>& mLanes;
  SA3LaneBlend& mBlend;
  SA3LaneSnap& mSnap;
  Host mHost;

  double mSeconds = 0.0;
  double mBpm = 0.0;
  double mBeatsPerBar = 4.0;
  std::vector<float> mPeaks;
  float mPlayhead = -1.f;

  IPopupMenu mSnapMenu;
  IRECT mTitleRect;
  IRECT mBlendOneRect;
  IRECT mBlendDistinctRect;
  IRECT mSnapRect;
  IRECT mRangeRect;
  IRECT mTakeHead;
  IRECT mTakeCanvas;
  std::vector<Row> mRows;
  size_t mFirstLane = 0;
  IRECT mScrollTrack;

  Drag mDrag = Drag::None;
  int mDragLane = -1;
  int mDragPoint = -1;
  std::vector<SA3LanePoint> mStroke;

  int mHoverLane = -1;
  int mHoverPoint = -1;
  float mHoverX = 0.f;
  float mHoverY = 0.f;
  float mReadoutPosition = 0.f;
  float mReadoutAmount = 0.f;
  ECursor mCursor = ECursor::ARROW;
};

}

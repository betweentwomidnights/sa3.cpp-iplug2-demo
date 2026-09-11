#include "SA3ReaperExtension.h"
#include "ReaperExt_include_in_plug_src.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "IControls.h"
#include "roboto.hpp"

namespace
{
const IColor kBackground = IColor(255, 24, 26, 31);
const IColor kPanel = IColor(255, 35, 38, 45);
const IColor kText = IColor(255, 229, 231, 235);
const IColor kMutedText = IColor(255, 164, 169, 180);
const IColor kAccent = IColor(255, 113, 90, 236);

IVStyle ButtonStyle()
{
  return DEFAULT_STYLE
    .WithColor(kBG, kPanel)
    .WithColor(kFG, kAccent)
    .WithColor(kPR, kAccent)
    .WithColor(kFR, IColor(255, 73, 77, 89))
    .WithLabelText(IText(14.f, kText, "Roboto-Regular", EAlign::Center, EVAlign::Middle));
}

bool NearlyEqual(double lhs, double rhs)
{
  return std::abs(lhs - rhs) < 0.0001;
}
}

bool SA3ReaperExtension::SelectionSnapshot::operator==(const SelectionSnapshot& other) const
{
  return itemCount == other.itemCount
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
    && takeName == other.takeName
    && sourceType == other.sourceType;
}

SA3ReaperExtension::SA3ReaperExtension(reaper_plugin_info_t* pRec)
: ReaperExtBase(pRec)
{
  IMPAPI(CountSelectedMediaItems);
  IMPAPI(GetSelectedMediaItem);
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

  SetDockId("SA3ReaperExtension.MainWindow");
  SetMenuName("SA3");

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
    const IRECT bounds = graphics->GetBounds();
    const IRECT content = bounds.GetPadded(-18.f);
    const IRECT title = content.GetFromTop(32.f);
    const IRECT tabs = content.GetFromTop(86.f).GetFromBottom(40.f);
    const IRECT selection = content.GetFromTop(176.f).GetFromBottom(74.f);
    const IRECT timing = content.GetFromTop(228.f).GetFromBottom(32.f);
    const IRECT hint = content.GetFromBottom(92.f);
    const float gap = 7.f;
    const float buttonWidth = (tabs.W() - gap * 2.f) / 3.f;
    const IRECT transformButton = tabs.GetFromLeft(buttonWidth);
    const IRECT continueButton = tabs.GetFromLeft(buttonWidth * 2.f + gap).GetFromRight(buttonWidth);
    const IRECT generateButton = tabs.GetFromRight(buttonWidth);

    if (graphics->NControls())
    {
      graphics->GetControl(0)->SetTargetAndDrawRECTs(bounds);
      graphics->GetControl(1)->SetTargetAndDrawRECTs(title);
      graphics->GetControl(2)->SetTargetAndDrawRECTs(transformButton);
      graphics->GetControl(3)->SetTargetAndDrawRECTs(continueButton);
      graphics->GetControl(4)->SetTargetAndDrawRECTs(generateButton);
      graphics->GetControl(5)->SetTargetAndDrawRECTs(selection);
      graphics->GetControl(6)->SetTargetAndDrawRECTs(timing);
      graphics->GetControl(7)->SetTargetAndDrawRECTs(hint);
      return;
    }

    graphics->SetLayoutOnResize(true);
    if (!graphics->LoadFont("Roboto-Regular", (void*) ROBOTO_REGULAR, ROBOTO_REGULAR_length))
      graphics->LoadFont("Roboto-Regular", "Arial", ETextStyle::Normal);

    graphics->AttachPanelBackground(kBackground);
    graphics->AttachControl(new ITextControl(title, "SA3 FOR REAPER · TRANSFORM",
      IText(22.f, kText, "Roboto-Regular", EAlign::Near, EVAlign::Middle)), kCtrlTagOperation);
    graphics->AttachControl(new IVButtonControl(transformButton,
      [this](IControl*) { SelectOperation(Operation::Transform); }, "TRANSFORM", ButtonStyle()));
    graphics->AttachControl(new IVButtonControl(continueButton,
      [this](IControl*) { SelectOperation(Operation::Continue); }, "CONTINUE", ButtonStyle()));
    graphics->AttachControl(new IVButtonControl(generateButton,
      [this](IControl*) { SelectOperation(Operation::Generate); }, "GENERATE", ButtonStyle()));
    graphics->AttachControl(new IMultiLineTextControl(selection, "No media item selected",
      IText(17.f, kText, "Roboto-Regular", EAlign::Near, EVAlign::Middle)), kCtrlTagSelection);
    graphics->AttachControl(new ITextControl(timing, "Time selection: none",
      IText(14.f, kMutedText, "Roboto-Regular", EAlign::Near, EVAlign::Middle)), kCtrlTagTiming);
    graphics->AttachControl(new IMultiLineTextControl(hint,
      "Select an audio item, then choose Transform or Continue.\nChoose Generate to use the REAPER time selection.",
      IText(14.f, kMutedText, "Roboto-Regular", EAlign::Near, EVAlign::Top)), kCtrlTagHint);

    RefreshPanel(true);
  };
}

void SA3ReaperExtension::OnIdle()
{
  RefreshPanel();
}

void SA3ReaperExtension::OnActionRun(int, int)
{
  RefreshPanel();
}

SA3ReaperExtension::SelectionSnapshot SA3ReaperExtension::ReadSelection() const
{
  SelectionSnapshot snapshot;
  snapshot.itemCount = CountSelectedMediaItems(nullptr);

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
  if (*GetWindowTogglePtr() == 0)
    ShowHideMainWindow();
  RefreshPanel(true);
}

void SA3ReaperExtension::RefreshPanel(bool force)
{
  const SelectionSnapshot current = ReadSelection();
  if (!force && mHasSelectionSnapshot && current == mSelection)
    return;

  mSelection = current;
  mHasSelectionSnapshot = true;

  IGraphics* ui = GetUI();
  if (!ui)
    return;

  char selectionText[768] = {};
  if (current.itemCount == 0)
  {
    std::snprintf(selectionText, sizeof(selectionText), "No media item selected");
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

  const char* hint = nullptr;
  const char* operationTitle = nullptr;
  const bool hasSingleAudioItem = current.itemCount == 1 && current.firstTakeIsAudio;
  switch (mOperation)
  {
    case Operation::Transform:
      operationTitle = "SA3 FOR REAPER · TRANSFORM";
      hint = hasSingleAudioItem
        ? "Ready for the Transform workflow. Rendering controls come next."
        : "Transform will initially require exactly one selected audio item.";
      break;
    case Operation::Continue:
      operationTitle = "SA3 FOR REAPER · CONTINUE";
      hint = hasSingleAudioItem
        ? "Ready for the continuation-splice workflow. Rendering controls come next."
        : "Continue will initially require exactly one selected audio item.";
      break;
    case Operation::Generate:
      operationTitle = "SA3 FOR REAPER · GENERATE";
      hint = timeLength > 0.0001
        ? "Ready to generate into the selected time range. Rendering controls come next."
        : "Set a REAPER time selection to choose the generated audio duration.";
      break;
  }

  SetTaggedText(kCtrlTagOperation, operationTitle);
  SetTaggedText(kCtrlTagSelection, selectionText);
  SetTaggedText(kCtrlTagTiming, timingText);
  SetTaggedText(kCtrlTagHint, hint);
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

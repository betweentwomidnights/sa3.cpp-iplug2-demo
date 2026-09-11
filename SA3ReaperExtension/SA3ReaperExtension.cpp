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
}

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
  IMPAPI(PCM_Source_CreateFromFile);
  IMPAPI(AddMediaItemToTrack);
  IMPAPI(AddTakeToMediaItem);
  IMPAPI(GetSetMediaItemTakeInfo);
  IMPAPI(GetSetMediaItemTakeInfo_String);
  IMPAPI(SetMediaItemInfo_Value);
  IMPAPI(Undo_BeginBlock2);
  IMPAPI(Undo_EndBlock2);
  IMPAPI(UpdateArrange);
  IMPAPI(MarkProjectDirty);

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
    const IRECT selection = content.GetFromTop(152.f).GetFromBottom(40.f);
    const IRECT timing = content.GetFromTop(190.f).GetFromBottom(28.f);
    const IRECT prompt = content.GetFromTop(270.f).GetFromBottom(62.f);
    const IRECT run = content.GetFromTop(322.f).GetFromBottom(38.f);
    const IRECT hint = content.GetFromBottom(54.f);
    const float gap = 7.f;
    const float buttonWidth = (tabs.W() - gap * 2.f) / 3.f;
    const IRECT generateButton = tabs.GetFromLeft(buttonWidth);
    const IRECT transformButton = tabs.GetFromLeft(buttonWidth * 2.f + gap).GetFromRight(buttonWidth);
    const IRECT continueButton = tabs.GetFromRight(buttonWidth);

    if (graphics->NControls())
    {
      graphics->GetControl(0)->SetTargetAndDrawRECTs(bounds);
      graphics->GetControl(1)->SetTargetAndDrawRECTs(title);
      graphics->GetControl(2)->SetTargetAndDrawRECTs(generateButton);
      graphics->GetControl(3)->SetTargetAndDrawRECTs(transformButton);
      graphics->GetControl(4)->SetTargetAndDrawRECTs(continueButton);
      graphics->GetControl(5)->SetTargetAndDrawRECTs(selection);
      graphics->GetControl(6)->SetTargetAndDrawRECTs(timing);
      graphics->GetControl(7)->SetTargetAndDrawRECTs(hint);
      graphics->GetControl(8)->SetTargetAndDrawRECTs(prompt);
      graphics->GetControl(9)->SetTargetAndDrawRECTs(run);
      return;
    }

    graphics->SetLayoutOnResize(true);
    graphics->EnableMouseOver(true);
    graphics->AttachTextEntryControl();
    if (!graphics->LoadFont(gary::ui::FontName, (void*) ROBOTO_REGULAR, ROBOTO_REGULAR_length))
      graphics->LoadFont(gary::ui::FontName, "Arial", ETextStyle::Normal);

    graphics->AttachControl(new SA3BackgroundControl(bounds));
    graphics->AttachControl(new ITextControl(title, "sa3 / REAPER",
      IText(gary::ui::TitleTextSize, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle)),
      kCtrlTagOperation);
    graphics->AttachControl(new SA3TabControl(generateButton, "generate", false,
      [this]() { SelectOperation(Operation::Generate); }), kCtrlTagGenerate);
    graphics->AttachControl(new SA3TabControl(transformButton, "transform", true,
      [this]() { SelectOperation(Operation::Transform); }), kCtrlTagTransform);
    graphics->AttachControl(new SA3TabControl(continueButton, "continue", false,
      [this]() { SelectOperation(Operation::Continue); }), kCtrlTagContinue);
    graphics->AttachControl(new IMultiLineTextControl(selection, "No media item selected",
      IText(17.f, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle)), kCtrlTagSelection);
    graphics->AttachControl(new ITextControl(timing, "Time selection: none",
      IText(gary::ui::BodyTextSize, gary::ui::TextDim(), gary::ui::FontName, EAlign::Near, EVAlign::Middle)),
      kCtrlTagTiming);
    graphics->AttachControl(new IMultiLineTextControl(hint,
      mPanelStatus.c_str(),
      IText(gary::ui::BodyTextSize, gary::ui::TextDim(), gary::ui::FontName, EAlign::Near, EVAlign::Top)),
      kCtrlTagHint);
    graphics->AttachControl(new SA3PromptControl(prompt, mPrompt.c_str(),
      IText(gary::ui::BodyTextSize, COLOR_WHITE, gary::ui::FontName, EAlign::Near, EVAlign::Middle)
        .WithTEColors(gary::ui::PanelDark(), COLOR_WHITE)), kCtrlTagPrompt);
    graphics->AttachControl(new SA3ActionButton(run, "generate into selection",
      [this]() { StartOrCancelGeneration(); }), kCtrlTagRun);

    RefreshPanel(true);
  };
}

SA3ReaperExtension::~SA3ReaperExtension()
{
  mRenderService.Cancel();
}

void SA3ReaperExtension::OnIdle()
{
  SyncPromptFromUI();
  gary::SA3RenderResult result;
  const bool completed = mRenderService.TakeCompletedResult(result);
  if (completed)
    FinishGeneration(std::move(result));
  RefreshPanel(completed || mRenderService.Busy());
}

void SA3ReaperExtension::OnActionRun(int, int)
{
  RefreshPanel();
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

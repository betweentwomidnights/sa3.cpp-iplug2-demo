#pragma once

#include <string>

#include "ReaperExt_include_in_plug_hdr.h"
#include "SA3RenderService.h"

class MediaTrack;

using namespace iplug;
using namespace igraphics;

enum EControlTags
{
  kCtrlTagOperation = 0,
  kCtrlTagSelection,
  kCtrlTagTiming,
  kCtrlTagHint,
  kCtrlTagTransform,
  kCtrlTagContinue,
  kCtrlTagGenerate,
  kCtrlTagPrompt,
  kCtrlTagRun,
  kNumCtrlTags
};

class SA3ReaperExtension final : public ReaperExtBase
{
public:
  explicit SA3ReaperExtension(reaper_plugin_info_t* pRec);
  ~SA3ReaperExtension() override;

  void OnIdle() override;
  void OnActionRun(int commandId, int flag) override;
  void SaveProjectState(ProjectStateContext* ctx) override;
  bool LoadProjectStateLine(const char* line) override;
  void OnBeginLoadProjectState(bool isUndo) override;

private:
  enum class Operation
  {
    Transform,
    Continue,
    Generate
  };

  struct SelectionSnapshot
  {
    int itemCount = 0;
    int selectedTrackCount = 0;
    double itemStart = 0.0;
    double itemLength = 0.0;
    double timeStart = 0.0;
    double timeEnd = 0.0;
    double bpm = 120.0;
    double beatsPerMeasure = 4.0;
    int beatUnit = 4;
    bool hasActiveTake = false;
    bool firstTakeIsAudio = false;
    std::string trackName;
    std::string selectedTrackName;
    std::string takeName;
    std::string sourceType;

    bool operator==(const SelectionSnapshot& other) const;
    bool operator!=(const SelectionSnapshot& other) const { return !(*this == other); }
  };

  SelectionSnapshot ReadSelection() const;
  void SelectOperation(Operation operation);
  void StartOrCancelGeneration();
  void FinishGeneration(gary::SA3RenderResult result);
  bool ReplaceTimeSelectionWithAudio(MediaTrack* track, double start, double end,
                                     const std::string& wavPath, const std::string& prompt,
                                     std::string& error);
  std::string MakeUniqueOutputPath(std::string& error) const;
  void SyncPromptFromUI();
  void RefreshPanel(bool force = false);
  void SetTaggedText(int tag, const char* text);

  struct PendingGeneration
  {
    MediaTrack* track = nullptr;
    double start = 0.0;
    double end = 0.0;
    std::string prompt;
  };

  Operation mOperation = Operation::Transform;
  SelectionSnapshot mSelection;
  bool mHasSelectionSnapshot = false;
  std::string mPrompt;
  std::string mPanelStatus = "Set a time selection and choose one destination track.";
  PendingGeneration mPendingGeneration;
  gary::SA3RenderService mRenderService;
};

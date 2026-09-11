#pragma once

#include <string>

#include "ReaperExt_include_in_plug_hdr.h"

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
  kNumCtrlTags
};

class SA3ReaperExtension final : public ReaperExtBase
{
public:
  explicit SA3ReaperExtension(reaper_plugin_info_t* pRec);

  void OnIdle() override;
  void OnActionRun(int commandId, int flag) override;

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
    std::string takeName;
    std::string sourceType;

    bool operator==(const SelectionSnapshot& other) const;
    bool operator!=(const SelectionSnapshot& other) const { return !(*this == other); }
  };

  SelectionSnapshot ReadSelection() const;
  void SelectOperation(Operation operation);
  void RefreshPanel(bool force = false);
  void SetTaggedText(int tag, const char* text);

  Operation mOperation = Operation::Transform;
  SelectionSnapshot mSelection;
  bool mHasSelectionSnapshot = false;
};

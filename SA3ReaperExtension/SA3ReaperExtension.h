#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ReaperExt_include_in_plug_hdr.h"
#include "SA3RenderService.h"

class MediaTrack;
class MediaItem;
class MediaItem_Take;
class PCM_source;
class SA3CreativeLoraControl;
class SA3RenderOptionsControl;
class SA3SettingsControl;

using namespace iplug;
using namespace igraphics;

enum EControlTags
{
  kCtrlTagBackground = 0,
  kCtrlTagOperation,
  kCtrlTagSelection,
  kCtrlTagTiming,
  kCtrlTagHint,
  kCtrlTagTransform,
  kCtrlTagContinue,
  kCtrlTagGenerate,
  kCtrlTagPrompt,
  kCtrlTagDice,
  kCtrlTagRenderOptions,
  kCtrlTagCreativeLoras,
  kCtrlTagRun,
  kCtrlTagSettingsButton,
  kCtrlTagSettings,
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
  friend class SA3SettingsControl;
  friend class SA3CreativeLoraControl;
  friend class SA3RenderOptionsControl;

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
  void StartOrCancelRender();
  void FinishGeneration(gary::SA3RenderResult result);
  void ContinuePendingPeakBuilds();
  bool CaptureTakeAudio(MediaItem_Take* take, double start, double end,
                        gary::RecordingSnapshot& audio, std::string& error) const;
  bool ReplaceRangeWithAudio(MediaTrack* track, double start, double end,
                             const std::string& wavPath, const std::string& prompt,
                             Operation operation, std::string& error);
  std::string MakeUniqueOutputPath(Operation operation, std::string& error) const;
  void SyncPromptFromUI();
  void RollPrompt();
  void SetDistShift(int distShift);
  void ToggleUseSeed();
  void SetSeedValue(int64_t seed);
  void SetContinueSeconds(double seconds);
  double CurrentSourceLength() const;
  double EffectiveContinueSeconds() const;
  void RefreshPanel(bool force = false);
  void SetTaggedText(int tag, const char* text);
  void ApplyResponsiveLayout(IGraphics* graphics);
  void ToggleSettingsPage();

  void ReloadSharedSettings(bool scanModels = true);
  void ChooseModelsFolder();
  void SelectModelVariant(const char* variant);
  void SetKeepModelsResident(bool enabled);
  void SetDecoderLoraEnabled(bool enabled);
  void ChooseDecoderLora();
  void ClearDecoderLora();
  void StartOrCancelDecoderLoraDownload();
  void DecoderLoraDownloadWorkerMain();
  void StopDecoderLoraDownload();
  void SetDecoderDownloadStatus(const std::string& status);
  std::string DecoderDownloadStatus() const;
  void SetPeakNormalizeEnabled(bool enabled);
  void SetPeakNormalizeDb(float db);
  void SetLimiterEnabled(bool enabled);
  void SetLimiterCeilingDb(float db);
  void SetLimiterKnee(float knee);
  void ResetOutputProcessing();
  void SetRawOutputProcessing();
  void ReloadCreativeLoras();
  void PersistCreativeLoras();
  void AddCreativeLora();
  void RemoveCreativeLora(size_t index);
  void ToggleCreativeLora(size_t index);
  void SetCreativeLoraStrength(size_t index, float strength);

  struct PendingGeneration
  {
    MediaTrack* track = nullptr;
    Operation operation = Operation::Generate;
    double start = 0.0;
    double end = 0.0;
    std::string prompt;
  };

  struct PendingPeakBuild
  {
    MediaItem* item = nullptr;
    PCM_source* source = nullptr;
  };

  struct CreativeLora
  {
    std::string name;
    std::string path;
    float strength = 1.f;
    bool enabled = true;
  };

  Operation mOperation = Operation::Transform;
  SelectionSnapshot mSelection;
  bool mHasSelectionSnapshot = false;
  std::string mPrompt;
  int mDistShift = 0;
  double mContinueSeconds = 0.0;
  bool mUseSeed = false;
  int64_t mSeedValue = 0;
  bool mHasLastSeed = false;
  int64_t mLastSeed = 0;
  std::string mPanelStatus = "Set a time selection and choose one destination track.";
  PendingGeneration mPendingGeneration;
  std::vector<PendingPeakBuild> mPendingPeakBuilds;
  gary::SA3RenderService mRenderService;

  bool mSettingsOpen = false;
  bool mWideLayout = false;
  std::string mSettingsNotice = "Changes apply to the next render.";
  std::string mModelsDir;
  std::string mModelVariant = "medium";
  bool mMediumModelsAvailable = false;
  bool mSmallModelsAvailable = false;
  bool mKeepModelsResident = false;
  std::string mDecoderLoraPath;
  bool mDecoderLoraEnabled = true;
  bool mPeakNormalizeEnabled = true;
  float mPeakNormalizeDb = 2.f;
  bool mLimiterEnabled = true;
  float mLimiterCeilingDb = -0.3f;
  float mLimiterKnee = 0.8f;
  std::vector<CreativeLora> mCreativeLoras;
  int mSettingsRefreshTicks = 0;

  std::thread mDecoderDownloadWorker;
  std::atomic<bool> mDecoderDownloadBusy{false};
  std::atomic<bool> mDecoderDownloadCancel{false};
  std::atomic<float> mDecoderDownloadProgress{0.f};
  std::atomic<uint64_t> mDecoderDownloadRevision{0};
  uint64_t mSeenDecoderDownloadRevision = 0;
  mutable std::mutex mDecoderDownloadStatusMutex;
  std::string mDecoderDownloadStatus;
};

#pragma once

#include "DemoAudioFileStore.h"
#include "IPlug_include_in_plug_hdr.h"
#include "SA3RenderService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

const int kNumPresets = 1;

enum EParams
{
  kStatusParam = 0,
  kNumParams
};

using namespace iplug;
using namespace igraphics;

class SA3IPlug2Demo final : public Plugin
{
public:
  enum class RenderMode
  {
    Text,
    Transform,
    Continue
  };

  struct WaveformPeak
  {
    float minValue = 0.f;
    float maxValue = 0.f;
  };

  struct Waveform
  {
    std::vector<WaveformPeak> peaks;
    int numSamples = 0;
    int sampleRate = 44100;
    int playheadSamples = 0;
    int savedSamples = 0;   // source only: samples up to the frozen "save buffer" point (drawn bright)
    bool active = false;
  };

  struct LoraSlot
  {
    std::string name;
    std::string path;
    std::vector<std::string> prompts;
    float strength = 0.0f;
  };

  SA3IPlug2Demo(const InstanceInfo& info);
  ~SA3IPlug2Demo() override;

  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

#if IPLUG_DSP
  void OnActivate(bool active) override;
  void OnReset() override;
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
#endif

#if IPLUG_EDITOR
  void OnUIClose() override;
  void OnIdle() override;
#endif

  void SetPrompt(const char* text);
  void SetPromptForMode(RenderMode mode, const char* text);
  void RollPromptForCurrentMode();
  std::string Prompt() const;
  std::string PromptForMode(RenderMode mode) const;
  std::string StatusText() const;
  std::string SourceStatusText() const;
  std::string OutputStatusText() const;

  // --- Model management (v0.3.0): the models dir is persisted; generate is gated on the set being present.
  std::string ModelsDir() const;             // persisted setting -> SA3_MODELS_DIR env -> compile default
  std::string ModelVariant() const;          // "medium" | "small-music"
  int ModelVariantIndex() const;             // 0 = medium, 1 = small-music
  bool ModelsPresent() const noexcept { return mModelsPresent.load(std::memory_order_acquire); }
  void RefreshModelsPresent();               // re-scan ModelsDir() for the current variant's file set
  // Point the demo at a folder (from the "point at folder" picker or a finished download). Validates the
  // set for `variant`, persists dir+variant, and tears down any loaded context so the next render reloads.
  bool UseModelsFolder(const std::string& dir, const std::string& variant, bool persist);
  // Switch the active generation variant (must already be present in the current models dir). Persists the
  // choice and drops any loaded context so the next render loads the selected variant.
  bool SelectVariant(const std::string& variant);
  bool VariantAvailable(const std::string& variant) const;   // is that variant's file set in ModelsDir()?
  void StartModelDownload(int variantIdx, const std::string& destDir);   // spawns the download worker
  void CancelModelDownload();
  bool Downloading() const noexcept { return mDownloading.load(std::memory_order_acquire); }
  bool DecoderLoraDownloading() const noexcept { return mDownloadKind.load(std::memory_order_acquire) == 2; }
  float DownloadProgress() const noexcept { return mDownloadProgress.load(std::memory_order_acquire); }

  void SetCurrentRenderMode(RenderMode mode);
  RenderMode CurrentRenderMode() const noexcept;
  void AdjustDuration(int deltaSeconds);
  void AdjustSteps(int deltaSteps);
  void AdjustInitNoise(float delta);
  void SetDurationSeconds(int seconds);
  void SetSteps(int steps);
  void SetInitNoiseLevel(float level);
  void SetUseSeed(bool useSeed);
  void ToggleUseSeed();
  void SetSeedValue(int64_t seed);
  int DurationSeconds() const noexcept { return mDurationSeconds[(size_t)mCurrentMode.load(std::memory_order_acquire)].load(std::memory_order_acquire); }
  int Steps() const noexcept { return mSteps.load(std::memory_order_acquire); }
  float CfgScale() const noexcept { return mCfgScale.load(std::memory_order_acquire); }   // 1.0 = off (single pass)
  void SetCfgScale(float v) { mCfgScale.store(std::clamp(v, 0.5f, 2.0f), std::memory_order_release); }
  float InitNoiseLevel() const noexcept { return mInitNoiseLevel.load(std::memory_order_acquire); }
  bool UseSeed() const noexcept { return mUseSeed.load(std::memory_order_acquire); }
  int64_t SeedValue() const noexcept { return mSeedValue.load(std::memory_order_acquire); }
  int64_t LastSeed() const noexcept { return mLastSeed.load(std::memory_order_acquire); }
  bool HasLastSeed() const noexcept { return mHasLastSeed.load(std::memory_order_acquire); }
  float Progress() const noexcept { return mRenderService.Progress(); }
  bool Busy() const noexcept { return mRenderService.Busy(); }
  bool TransportRunning() const noexcept { return mTransportRunning.load(std::memory_order_acquire); }
  bool OutputPlaying() const noexcept { return mOutputPlaying.load(std::memory_order_acquire); }

  // A frozen init snapshot (myBuffer.wav) exists — set by dropping audio or "save buffer", persisted across
  // sessions. transform/continue use this snapshot as init audio and are disabled until it exists.
  bool HasInit() const noexcept { return mHasInit.load(std::memory_order_acquire); }
  // Generation needs the model set present; transform/continue additionally need a frozen init snapshot.
  bool CanRender(RenderMode mode) const noexcept
  {
    return ModelsPresent() && (mode == RenderMode::Text || HasInit());
  }

  // Musical controls (appended to the prompt; loop drives an exact bar length in Text mode).
  double HostTempo() const noexcept { return mHostTempo.load(std::memory_order_acquire); }
  double Bpm() const noexcept { return mBpm.load(std::memory_order_acquire); }   // effective bpm (host or user)
  bool BpmOverridden() const noexcept { return mBpmOverride.load(std::memory_order_acquire); }
  void AdjustBpm(double delta)   // standalone: drag to set; flips to a user override
  {
    mBpmOverride.store(true, std::memory_order_release);
    mBpm.store(std::clamp(mBpm.load(std::memory_order_acquire) + delta, 20.0, 300.0), std::memory_order_release);
  }
  void ClearBpmOverride() { mBpmOverride.store(false, std::memory_order_release); }   // resume following host
  int KeyRoot() const noexcept { return mKeyRoot.load(std::memory_order_acquire); }   // 0=none, 1..12=C..B
  int KeyMode() const noexcept { return mKeyMode.load(std::memory_order_acquire); }   // 0=major, 1=minor
  void SetKeyRoot(int root) { mKeyRoot.store(std::clamp(root, 0, 12), std::memory_order_release); }
  void SetKeyMode(int scaleMode) { mKeyMode.store(scaleMode ? 1 : 0, std::memory_order_release); }
  int LoopBars() const noexcept { return mLoopBars.load(std::memory_order_acquire); } // 0=off, 4/8/16
  void SetLoopBars(int bars) { mLoopBars.store((bars == 4 || bars == 8 || bars == 16) ? bars : 0, std::memory_order_release); }
  std::string KeyScaleText() const;   // "" when root=none, else "C minor"
  // Each mode remembers its own ending. Transform follows its source and has none.
  bool EndingKeepsGoing(RenderMode mode) const noexcept
  {
    return (mode == RenderMode::Continue ? mContinueKeepsGoing : mGenerateKeepsGoing).load(std::memory_order_acquire);
  }
  void SetEndingKeepsGoing(RenderMode mode, bool keepsGoing);
  int DistShift() const noexcept { return mDistShift.load(std::memory_order_acquire); }   // 0=LogSNR,1=Flux,2=Full,3=None
  void SetDistShift(int idx) { mDistShift.store(std::clamp(idx, 0, 3), std::memory_order_release); }
  const char* DistShiftName() const;  // libsa3 dist_shift string for the current index (default LogSNR)

  void StartRender(RenderMode mode);
  void CancelRender();
  bool LoadDroppedAudioFile(const char* rawPath);
  bool SaveOutputToDisk();
  bool SaveSourceToDisk();   // save the recording/source buffer to myBuffer.wav (gary4juce "save buffer")
  gary::AudioFileInfo CreateOutputDragCopy();
  bool ImportLoraFromDialog();
  void RemoveLora(size_t index);
  void SetLoraStrength(size_t index, float strength);
  std::vector<LoraSlot> Loras() const;

  // Automation: one curve per LoRA in a column beside the main panel, which widens the window.
  bool AutomationOn() const noexcept { return mAutomationOn; }
  void ToggleAutomation();
  int LaneIndexFor(const std::string& path) const;

  // Decoder correction lives in Settings rather than the creative LoRA list. The currently published
  // adapter targets SAME-L, so it is remembered but not submitted while a SAME-S variant is active.
  bool DecoderLoraEnabled() const noexcept { return mDecoderLoraEnabled.load(std::memory_order_acquire); }
  void SetDecoderLoraEnabled(bool enabled);
  bool DecoderLoraInstalled() const;
  bool DecoderLoraActive() const;
  std::string DecoderLoraPath() const;
  std::string DecoderLoraDisplayName() const;
  void StartDecoderLoraDownload();
  bool ImportDecoderLoraFromDialog();
  void ClearDecoderLoraSelection();

  bool KeepModelsResident() const noexcept { return mKeepModelsResident.load(std::memory_order_acquire); }
  void SetKeepModelsResident(bool enabled);

  bool PeakNormalizeEnabled() const noexcept { return mPeakNormalizeEnabled.load(std::memory_order_acquire); }
  float PeakNormalizeDb() const noexcept { return mPeakNormalizeDb.load(std::memory_order_acquire); }
  bool LimiterEnabled() const noexcept { return mLimiterEnabled.load(std::memory_order_acquire); }
  float LimiterCeilingDb() const noexcept { return mLimiterCeilingDb.load(std::memory_order_acquire); }
  float LimiterKnee() const noexcept { return mLimiterKnee.load(std::memory_order_acquire); }
  void SetPeakNormalizeEnabled(bool enabled);
  void SetPeakNormalizeDb(float db);
  void SetLimiterEnabled(bool enabled);
  void SetLimiterCeilingDb(float db);
  void SetLimiterKnee(float knee);
  void ResetOutputProcessing();
  void SetRawOutputProcessing();
  void ToggleOutputPlayback();
  void StopOutputPlayback();
  void SeekOutputPlayback(double seconds);
  double OutputPlayheadSeconds() const;   // negative when nothing has been played from the output

  Waveform SourceWaveform(int bucketCount) const;
  Waveform OutputWaveform(int bucketCount) const;

private:
  static constexpr double kMaxRecordSeconds = 180.0;
  static constexpr double kMaxRecordAllocationSampleRate = 192000.0;
  static constexpr int kPreferredChannels = 2;

  static int ModeIndex(RenderMode mode) noexcept;
  void ResizeRecordBuffer(double sampleRate);
  void RebuildOutputPlaybackBufferFromNativeLocked(int hostSampleRate);
  void StartAutoRecording();
  void StopAutoRecording();
  void LoadPersistedBuffer();   // restore myBuffer.wav into the frozen snapshot on startup
  void CopyInputToRecordBuffer(sample** inputs, int nInChans, int nFrames);
  void MixOutputPlayback(sample** outputs, int nOutChans, int nFrames);
  gary::SA3RenderRequest BuildRenderRequest(RenderMode mode);
  void FinishRender(gary::SA3RenderResult result);
  double SourceSeconds() const;
  double RenderSeconds(RenderMode mode) const;   // what the curves span: Continue's are over the added part
  gary::RecordingSnapshot InitSnapshot() const;
  std::vector<gary::SA3LaneSource> LaneSources() const;
  void SyncLanesToLoras();
  void SetLaneBackdrop(const gary::RecordingSnapshot& audio, double startSeconds);
  int WindowWidth() const noexcept { return mAutomationOn ? PLUG_WIDTH * 2 : PLUG_WIDTH; }
  void ApplyWindowLayout(IGraphics* ui);
  void DownloadWorkerMain(int variantIdx, std::string destDir);
  void DecoderLoraDownloadWorkerMain();
  void StopDownloadWorker();
  void LoadPersistedCreativeLoras();
  void PersistCreativeLoras();
  void SetStatus(const std::string& text);
  void SetSourceStatus(const std::string& text);
  void SetOutputStatus(const std::string& text);
  void InstallOutput(gary::RecordingSnapshot audio);
  static std::string NormalizeDroppedPath(const char* rawPath);

  mutable std::mutex mPromptMutex;
  // empty by default: SA3 describes the music the same way for generate/transform/continue, and unprompted
  // generation is a valid (LoRA-friendly) mode. The dice rolls in real prompts; a greyed placeholder invites typing.
  std::array<std::string, 3> mPrompts = { "", "", "" };

  mutable std::mutex mStatusMutex;
  std::string mStatus = "idle";
  std::string mSourceStatus = "drop WAV/MP3 here or play host transport to record";
  std::string mOutputStatus = "no output yet";

  std::atomic<int> mCurrentMode{0};
  std::array<std::atomic<int>, 3> mDurationSeconds{};   // per mode (text/transform/continue); set in ctor
  std::atomic<int> mSteps{8};
  std::atomic<float> mCfgScale{1.0f};
  std::atomic<float> mInitNoiseLevel{0.5f};   // transform default: mid-strength (LogSNR front-loads noise)
  std::atomic<bool> mUseSeed{false};
  std::atomic<int64_t> mSeedValue{0};
  std::atomic<int64_t> mLastSeed{0};
  std::atomic<bool> mHasLastSeed{false};
  std::atomic<bool> mTransportRunning{false};
  std::atomic<double> mHostTempo{120.0};
  std::atomic<double> mBpm{120.0};
  std::atomic<int> mBeatsPerBar{4};
  std::atomic<bool> mBpmOverride{false};
  std::atomic<int> mKeyRoot{0};
  std::atomic<int> mKeyMode{0};
  std::atomic<int> mLoopBars{0};
  std::atomic<bool> mGenerateKeepsGoing{false};
  std::atomic<bool> mContinueKeepsGoing{true};
  std::atomic<int> mDistShift{0};

  mutable std::mutex mSourceMutex;
  std::vector<std::vector<float>> mSourceBuffer;   // live scratch pad: clears + regrows each transport play
  int mSourceSamples = 0;
  int mSourceSampleRate = 44100;
  int mRecordWritePosition = 0;
  bool mWasTransportRunning = false;
  // frozen init snapshot (== myBuffer.wav) used as init audio for transform/continue; guarded by mSourceMutex
  std::vector<std::vector<float>> mInitBuffer;
  int mInitSamples = 0;
  int mInitSampleRate = 44100;
  std::atomic<int> mSavedSamples{0};   // bright/dim boundary within the live scratch buffer
  std::atomic<bool> mHasInit{false};

  mutable std::mutex mOutputMutex;
  std::vector<std::vector<float>> mOutputBuffer;
  int mOutputSamples = 0;
  int mOutputSampleRate = 44100;

  mutable std::mutex mOutputPlaybackMutex;
  std::vector<std::vector<float>> mOutputPlaybackBuffer;
  std::atomic<int> mOutputPlaybackSamples{0};
  std::atomic<int> mOutputPlaybackSampleRate{44100};
  std::atomic<bool> mOutputPlaying{false};
  std::atomic<int> mOutputPlayhead{0};
  std::atomic<uint64_t> mOutputRevision{0};

  mutable std::mutex mLoraMutex;
  std::vector<LoraSlot> mLoras;
  std::atomic<bool> mCreativeLorasDirty{false};

  // Lanes are only touched on the UI thread, where hosts also save and restore state.
  std::vector<gary::SA3Lane> mLanes;
  gary::SA3LaneBlend mLaneBlend = gary::SA3LaneBlend::OneSong;
  gary::SA3LaneSnap mLaneSnap = gary::SA3LaneSnap::Off;
  std::vector<float> mLaneBackdrop;
  bool mAutomationOn = false;
  bool mLastRenderUsedLanes = false;
  double mLaneTimelineStart = 0.0;
  std::atomic<bool> mWindowLayoutPending{false};

  mutable std::mutex mDecoderLoraMutex;
  std::string mDecoderLoraPath;
  std::atomic<bool> mDecoderLoraEnabled{true};
  std::atomic<bool> mPeakNormalizeEnabled{true};
  std::atomic<float> mPeakNormalizeDb{2.0f};
  std::atomic<bool> mLimiterEnabled{true};
  std::atomic<float> mLimiterCeilingDb{-0.3f};
  std::atomic<float> mLimiterKnee{0.8f};
  std::atomic<bool> mKeepModelsResident{false};

  std::atomic<int> mHostSampleRate{44100};
  gary::SA3RenderService mRenderService;

  // Model management state. mModelsDirSetting ("" = fall through to env/default) + mModelVariant are loaded
  // from settings.txt in the ctor; guarded by mModelsMutex. mModelsPresent caches the file-set scan so Draw
  // doesn't hit the filesystem every frame.
  mutable std::mutex mModelsMutex;
  std::string mModelsDirSetting;
  std::string mModelVariant = "medium";
  std::atomic<bool> mModelsPresent{false};

  std::thread mDownloadWorker;
  std::atomic<bool> mDownloading{false};
  std::atomic<bool> mDownloadCancel{false};
  std::atomic<float> mDownloadProgress{0.0f};
  std::atomic<int> mDownloadKind{0};   // 0=none, 1=model set, 2=SAME-L decoder correction
};

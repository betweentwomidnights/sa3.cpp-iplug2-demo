#pragma once

#include "DemoAudioFileStore.h"
#include "IPlug_include_in_plug_hdr.h"
#include "libsa3.h"

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
    float strength = 1.0f;
    bool enabled = true;
  };

  SA3IPlug2Demo(const InstanceInfo& info);
  ~SA3IPlug2Demo() override;

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
  std::string ModelsDir() const;

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
  float Progress() const noexcept { return mProgress.load(std::memory_order_acquire); }
  bool Busy() const noexcept { return mBusy.load(std::memory_order_acquire); }
  bool TransportRunning() const noexcept { return mTransportRunning.load(std::memory_order_acquire); }
  bool OutputPlaying() const noexcept { return mOutputPlaying.load(std::memory_order_acquire); }

  // A frozen init snapshot (myBuffer.wav) exists — set by dropping audio or "save buffer", persisted across
  // sessions. transform/continue use this snapshot as init audio and are disabled until it exists.
  bool HasInit() const noexcept { return mHasInit.load(std::memory_order_acquire); }
  bool CanRender(RenderMode mode) const noexcept { return mode == RenderMode::Text || HasInit(); }

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
  bool AppendBpm() const noexcept { return mAppendBpm.load(std::memory_order_acquire); }
  void ToggleAppendBpm() { mAppendBpm.store(!AppendBpm(), std::memory_order_release); }
  int KeyRoot() const noexcept { return mKeyRoot.load(std::memory_order_acquire); }   // 0=none, 1..12=C..B
  int KeyMode() const noexcept { return mKeyMode.load(std::memory_order_acquire); }   // 0=major, 1=minor
  void SetKeyRoot(int root) { mKeyRoot.store(std::clamp(root, 0, 12), std::memory_order_release); }
  void SetKeyMode(int scaleMode) { mKeyMode.store(scaleMode ? 1 : 0, std::memory_order_release); }
  int LoopBars() const noexcept { return mLoopBars.load(std::memory_order_acquire); } // 0=off, 4/8/16
  void SetLoopBars(int bars) { mLoopBars.store((bars == 4 || bars == 8 || bars == 16) ? bars : 0, std::memory_order_release); }
  std::string KeyScaleText() const;   // "" when root=none, else "C minor"
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
  void SetLoraEnabled(size_t index, bool enabled);
  std::vector<LoraSlot> Loras() const;
  void ToggleOutputPlayback();
  void StopOutputPlayback();
  void SeekOutputPlayback(double seconds);

  Waveform SourceWaveform(int bucketCount) const;
  Waveform OutputWaveform(int bucketCount) const;

private:
  static constexpr double kMaxRecordSeconds = 180.0;
  static constexpr double kMaxRecordAllocationSampleRate = 192000.0;
  static constexpr int kPreferredChannels = 2;

  struct RenderInput
  {
    std::string prompt;
    RenderMode mode = RenderMode::Text;
    int durationSeconds = 12;
    int steps = 8;
    float cfgScale = 1.0f;
    float initNoiseLevel = 0.85f;
    bool useSeed = false;
    int64_t seed = 0;
    double bpm = 0.0;       // host tempo captured at request time (0 = unknown)
    int loopBars = 0;       // 0 = off; 4/8/16 = generate an exact bar-length loop (Text mode only)
    int distShift = 0;      // 0=LogSNR,1=Flux,2=Full,3=None (libsa3 schedule warp)
    std::vector<std::vector<float>> sourceChannels;
    int sourceSamples = 0;
    int sourceSampleRate = 44100;
    std::vector<LoraSlot> loras;
  };

  static int ModeIndex(RenderMode mode) noexcept;
  void ResizeRecordBuffer(double sampleRate);
  void RebuildOutputPlaybackBufferFromNativeLocked(int hostSampleRate);
  void StartAutoRecording();
  void StopAutoRecording();
  void LoadPersistedBuffer();   // restore myBuffer.wav into the frozen snapshot on startup
  void CopyInputToRecordBuffer(sample** inputs, int nInChans, int nFrames);
  void MixOutputPlayback(sample** outputs, int nOutChans, int nFrames);
  RenderInput CaptureRenderInput(RenderMode mode);
  void RenderWorkerMain(uint64_t requestId, RenderInput input);
  void StopWorker();
  void SetStatus(const std::string& text);
  void SetSourceStatus(const std::string& text);
  void SetOutputStatus(const std::string& text);
  void InstallOutputFromPlanar(const float* samples, int nSamp, int nCh, int sampleRate, int keepSamples = -1);
  gary::RecordingSnapshot SourceSnapshotForSA3(const RenderInput& input) const;
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
  std::atomic<float> mProgress{0.0f};
  std::atomic<bool> mBusy{false};
  std::atomic<bool> mTransportRunning{false};
  std::atomic<double> mHostTempo{120.0};
  std::atomic<double> mBpm{120.0};
  std::atomic<bool> mBpmOverride{false};
  std::atomic<bool> mAppendBpm{true};
  std::atomic<int> mKeyRoot{0};
  std::atomic<int> mKeyMode{0};
  std::atomic<int> mLoopBars{0};
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

  std::atomic<int> mHostSampleRate{44100};
  std::thread mWorker;
  std::atomic<uint64_t> mRequestId{0};
  std::atomic<bool> mCancelRequested{false};
  sa3_context* mContext = nullptr;
};

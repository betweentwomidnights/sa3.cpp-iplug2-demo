#pragma once

#include "DemoAudioFileStore.h"
#include "IPlug_include_in_plug_hdr.h"
#include "libsa3.h"

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
  void OnReset() override;
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
#endif

#if IPLUG_EDITOR
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
  int DurationSeconds() const noexcept { return mDurationSeconds.load(std::memory_order_acquire); }
  int Steps() const noexcept { return mSteps.load(std::memory_order_acquire); }
  float InitNoiseLevel() const noexcept { return mInitNoiseLevel.load(std::memory_order_acquire); }
  float Progress() const noexcept { return mProgress.load(std::memory_order_acquire); }
  bool Busy() const noexcept { return mBusy.load(std::memory_order_acquire); }
  bool TransportRunning() const noexcept { return mTransportRunning.load(std::memory_order_acquire); }
  bool OutputPlaying() const noexcept { return mOutputPlaying.load(std::memory_order_acquire); }

  void StartRender(RenderMode mode);
  void CancelRender();
  bool LoadDroppedAudioFile(const char* rawPath);
  bool SaveOutputToDisk();
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
    float initNoiseLevel = 0.85f;
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
  void CopyInputToRecordBuffer(sample** inputs, int nInChans, int nFrames);
  void MixOutputPlayback(sample** outputs, int nOutChans, int nFrames);
  RenderInput CaptureRenderInput(RenderMode mode);
  void RenderWorkerMain(uint64_t requestId, RenderInput input);
  void StopWorker();
  void SetStatus(const std::string& text);
  void SetSourceStatus(const std::string& text);
  void SetOutputStatus(const std::string& text);
  void InstallOutputFromPlanar(const float* samples, int nSamp, int nCh, int sampleRate);
  gary::RecordingSnapshot SourceSnapshotForSA3(const RenderInput& input) const;
  static std::string NormalizeDroppedPath(const char* rawPath);

  mutable std::mutex mPromptMutex;
  std::array<std::string, 3> mPrompts = {
    "bright electronic loop with warm drums and melodic synths",
    "make this source audio brighter, wider, and more rhythmic",
    "continue this idea into a clean musical ending"
  };

  mutable std::mutex mStatusMutex;
  std::string mStatus = "idle";
  std::string mSourceStatus = "drop WAV/MP3 here or play host transport to record";
  std::string mOutputStatus = "no output yet";

  std::atomic<int> mCurrentMode{0};
  std::atomic<int> mDurationSeconds{12};
  std::atomic<int> mSteps{8};
  std::atomic<float> mInitNoiseLevel{0.85f};
  std::atomic<float> mProgress{0.0f};
  std::atomic<bool> mBusy{false};
  std::atomic<bool> mTransportRunning{false};

  mutable std::mutex mSourceMutex;
  std::vector<std::vector<float>> mSourceBuffer;
  int mSourceSamples = 0;
  int mSourceSampleRate = 44100;
  int mRecordWritePosition = 0;
  bool mWasTransportRunning = false;

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

#pragma once

#include "DemoAudioFileStore.h"
#include "IPlug_include_in_plug_hdr.h"
#include "libsa3.h"

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
  std::string Prompt() const;
  std::string StatusText() const;
  std::string SourceStatusText() const;
  std::string OutputStatusText() const;
  std::string ModelsDir() const;

  void AdjustDuration(int deltaSeconds);
  void AdjustSteps(int deltaSteps);
  void AdjustInitNoise(float delta);
  int DurationSeconds() const noexcept { return mDurationSeconds.load(std::memory_order_acquire); }
  int Steps() const noexcept { return mSteps.load(std::memory_order_acquire); }
  float InitNoiseLevel() const noexcept { return mInitNoiseLevel.load(std::memory_order_acquire); }
  float Progress() const noexcept { return mProgress.load(std::memory_order_acquire); }
  bool Busy() const noexcept { return mBusy.load(std::memory_order_acquire); }
  bool TransportRunning() const noexcept { return mTransportRunning.load(std::memory_order_acquire); }
  bool OutputPlaying() const noexcept { return mOutputPlaying.load(std::memory_order_acquire); }

  void StartRender(RenderMode mode);
  bool LoadDroppedAudioFile(const char* rawPath);
  bool SaveOutputToDisk();
  gary::AudioFileInfo CreateOutputDragCopy();
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
  };

  void ResizeRecordBuffer(double sampleRate);
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
  std::string mPrompt = "bright electronic loop with warm drums and melodic synths";

  mutable std::mutex mStatusMutex;
  std::string mStatus = "idle";
  std::string mSourceStatus = "drop WAV/MP3 here or play host transport to record";
  std::string mOutputStatus = "no output yet";

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
  std::atomic<bool> mOutputPlaying{false};
  std::atomic<int> mOutputPlayhead{0};
  std::atomic<uint64_t> mOutputRevision{0};

  std::thread mWorker;
  std::atomic<uint64_t> mRequestId{0};
  sa3_context* mContext = nullptr;
};


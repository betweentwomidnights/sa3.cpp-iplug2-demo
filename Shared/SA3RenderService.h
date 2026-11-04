#pragma once

#include "DemoAudioFileStore.h"
#include "SA3LaneModel.h"
#include "SA3PromptOptions.h"
#include "libsa3_v1.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gary
{

struct SA3LoraSetting
{
  std::string path;
  float strength = 1.f;
};

// One automation lane as libsa3 reads it. envelope holds (seconds, amount) pairs on the output
// timeline, where amount is the LoRA's strength, and an empty prompt uses the request's.
struct SA3LaneSetting
{
  std::string loraPath;
  std::string prompt;
  std::vector<float> envelope;
};

enum class SA3RenderOperation
{
  Generate,
  Transform,
  Continue
};

struct SA3RenderRequest
{
  SA3RenderOperation operation = SA3RenderOperation::Generate;
  std::string prompt;
  // Generate: output duration. Continue: additional duration. Transform follows sourceAudio.
  double durationSeconds = 30.0;
  double bpm = 120.0;
  float generationTailSeconds = kSA3EndsHereTailSeconds;
  float continuationTailSeconds = kSA3KeepsGoingTailSeconds;
  int steps = 8;
  float cfgScale = 1.f;
  float initNoiseLevel = 0.5f;
  int64_t seed = -1;
  int distShift = 0;
  std::string modelsDir;
  std::string variant = "medium";
  RecordingSnapshot sourceAudio;
  std::vector<SA3LoraSetting> loras;
  std::vector<SA3LaneSetting> lanes;
  float laneIsolation = 1.f;
  float laneIsolationUntil = 0.f;
  float laneAverageUntil = 0.9f;
  bool peakNormalize = true;
  float peakNormalizeDb = 2.f;
  bool limiter = true;
  float limiterCeilingDb = -0.3f;
  float limiterKnee = 0.8f;
  bool keepModelsResident = false;
};

struct SA3RenderResult
{
  bool ok = false;
  bool cancelled = false;
  std::string error;
  RecordingSnapshot audio;
  int64_t seed = 0;
};

SA3RenderRequest LoadSharedRenderRequest(std::string prompt,
                                         double durationSeconds,
                                         double bpm,
                                         SA3RenderOperation operation = SA3RenderOperation::Generate);
bool ValidateRenderRequest(const SA3RenderRequest& request, std::string& error);
// Appends the tempo, replacing one a prompt already carries, so every prompt agrees on the grid.
std::string PromptWithTempo(std::string prompt, double bpm);
// False only when libsa3 can read the file and says a lane can't run it.
bool AdapterSupportsLanes(const std::string& path);

// A LoRA in the host's list, as the lanes see it.
struct SA3LaneSource
{
  std::string name;
  std::string path;
  float strength = 0.f;
};

// Gives every LoRA a lane, in list order. Curves survive automation being switched off, so turning
// it back on returns them as they were. Returns the name of a LoRA that can't run in a lane, if any.
std::string SyncLanes(std::vector<SA3Lane>& lanes, const std::vector<SA3LaneSource>& sources);
// timelineStartSeconds places the curves on the output, which starts with the source for Continue.
void AddLanesToRequest(SA3RenderRequest& request, const std::vector<SA3Lane>& lanes, SA3LaneBlend blend,
                       const std::vector<SA3LaneSource>& sources, double promptBpm, const std::string& keyScale,
                       double timelineStartSeconds, double timelineSeconds);
// Min/max pairs across the audio from startSeconds on, for the automation strip.
std::vector<float> WaveformPeaks(const RecordingSnapshot& audio, double startSeconds, size_t columns = 512);

class SA3RenderService
{
public:
  SA3RenderService() = default;
  ~SA3RenderService();

  SA3RenderService(const SA3RenderService&) = delete;
  SA3RenderService& operator=(const SA3RenderService&) = delete;

  bool StartRender(SA3RenderRequest request);
  void Cancel();
  void ReleaseModels();
  bool Busy() const noexcept { return mBusy.load(std::memory_order_acquire); }
  float Progress() const noexcept { return mProgress.load(std::memory_order_acquire); }
  std::string Status() const;
  bool TakeCompletedResult(SA3RenderResult& result);

private:
  void WorkerMain(uint64_t requestId, SA3RenderRequest request);
  void SetStatus(std::string status);
  void PublishResult(SA3RenderResult result, std::string status);
  void StopWorker();
  void TeardownContext();

  mutable std::mutex mStateMutex;
  std::string mStatus = "idle";
  SA3RenderResult mCompletedResult;
  bool mHasCompletedResult = false;
  std::thread mWorker;
  std::atomic<bool> mBusy{false};
  std::atomic<bool> mCancelRequested{false};
  std::atomic<float> mProgress{0.f};
  std::atomic<uint64_t> mRequestId{0};
  sa3_context* mContext = nullptr;
  std::string mContextModelsDir;
  std::string mContextVariant;
};

}

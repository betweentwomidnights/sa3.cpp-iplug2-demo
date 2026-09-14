#pragma once

#include "DemoAudioFileStore.h"
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
  int steps = 8;
  float cfgScale = 1.f;
  float initNoiseLevel = 0.5f;
  int64_t seed = -1;
  int distShift = 0;
  std::string modelsDir;
  std::string variant = "medium";
  RecordingSnapshot sourceAudio;
  std::vector<SA3LoraSetting> loras;
  bool peakNormalize = true;
  float peakNormalizeDb = 2.f;
  bool limiter = true;
  float limiterCeilingDb = -0.3f;
  float limiterKnee = 0.8f;
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

class SA3RenderService
{
public:
  SA3RenderService() = default;
  ~SA3RenderService();

  SA3RenderService(const SA3RenderService&) = delete;
  SA3RenderService& operator=(const SA3RenderService&) = delete;

  bool StartRender(SA3RenderRequest request);
  void Cancel();
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

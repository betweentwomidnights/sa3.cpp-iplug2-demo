#include "SA3RenderService.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <dlfcn.h>
#endif

#ifndef SA3_DEMO_DEFAULT_MODELS_DIR
#define SA3_DEMO_DEFAULT_MODELS_DIR "models"
#endif

namespace gary
{
namespace
{
bool ParseBool(const std::string& text, bool fallback)
{
  if (text.empty()) return fallback;
  if (text == "1" || text == "true" || text == "on") return true;
  if (text == "0" || text == "false" || text == "off") return false;
  return fallback;
}

float ParseFloat(const std::string& text, float fallback, float low, float high)
{
  if (text.empty()) return fallback;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || (end && *end != '\0') || !std::isfinite(value))
    return fallback;
  return std::clamp(value, low, high);
}

int64_t RequestableSeed(uint64_t seed)
{
  return static_cast<int64_t>(seed & static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
}

struct Sa3Api
{
  using InitFn = sa3_context* (*)(const sa3_config*, char*, int);
  using GenerateExFn = int (*)(sa3_context*, const sa3_request_ex*, sa3_audio*, char*, int);
  using FreeAudioFn = void (*)(sa3_audio*);
  using FreeContextFn = void (*)(sa3_context*);

#ifdef _WIN32
  HMODULE module = nullptr;
#elif defined(__APPLE__)
  void* module = nullptr;
#endif
  InitFn init = nullptr;
  GenerateExFn generateEx = nullptr;
  FreeAudioFn freeAudio = nullptr;
  FreeContextFn freeContext = nullptr;

  bool Ready() const noexcept
  {
#if defined(_WIN32) || defined(__APPLE__)
    return module && init && generateEx && freeAudio && freeContext;
#else
    return false;
#endif
  }

  bool Load(std::string& error)
  {
    if (Ready())
      return true;

#ifdef _WIN32
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&SharedSa3Api), &self))
    {
      error = "could not locate the REAPER extension module";
      return false;
    }

    std::vector<wchar_t> path(1024);
    DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
    while (length >= path.size() - 1)
    {
      path.resize(path.size() * 2);
      length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
    }
    if (length == 0)
    {
      error = "could not read the REAPER extension path";
      return false;
    }

    std::wstring dllPath(path.data(), length);
    const size_t slash = dllPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
      error = "could not derive the REAPER extension directory";
      return false;
    }
    dllPath.resize(slash + 1);
    dllPath += L"SA3ReaperExtension\\sa3.dll";

    module = LoadLibraryExW(dllPath.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
    {
      error = "could not load SA3ReaperExtension/sa3.dll (win32 "
            + std::to_string(GetLastError()) + ")";
      return false;
    }

    return Resolve(init, "sa3_init", error)
        && Resolve(generateEx, "sa3_generate_ex", error)
        && Resolve(freeAudio, "sa3_free_audio", error)
        && Resolve(freeContext, "sa3_free", error);
#elif defined(__APPLE__)
    Dl_info info = {};
    if (dladdr(reinterpret_cast<const void*>(&SharedSa3Api), &info) == 0 || !info.dli_fname)
    {
      error = "could not locate the REAPER extension module";
      return false;
    }
    std::string dllPath(info.dli_fname);
    const size_t slash = dllPath.find_last_of('/');
    if (slash == std::string::npos)
    {
      error = "could not derive the REAPER extension directory";
      return false;
    }
    dllPath.resize(slash + 1);
    dllPath += "SA3ReaperExtension/libsa3.dylib";
    module = dlopen(dllPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
      const char* detail = dlerror();
      error = "could not load SA3ReaperExtension/libsa3.dylib";
      if (detail) error += std::string(": ") + detail;
      return false;
    }
    return Resolve(init, "sa3_init", error)
        && Resolve(generateEx, "sa3_generate_ex", error)
        && Resolve(freeAudio, "sa3_free_audio", error)
        && Resolve(freeContext, "sa3_free", error);
#else
    error = "libsa3 runtime loading is only supported on Windows and macOS";
    return false;
#endif
  }

private:
  static Sa3Api& SharedSa3Api();

  template <typename Fn>
  bool Resolve(Fn& fn, const char* name, std::string& error)
  {
#ifdef _WIN32
    FARPROC proc = GetProcAddress(module, name);
#elif defined(__APPLE__)
    dlerror();
    void* proc = dlsym(module, name);
#else
    void* proc = nullptr;
#endif
    if (!proc)
    {
      error = std::string("could not resolve ") + name + " from the libsa3 runtime";
      return false;
    }
    fn = reinterpret_cast<Fn>(proc);
    return true;
  }
};

Sa3Api& SharedSa3ApiInstance()
{
  static Sa3Api api;
  return api;
}

Sa3Api& Sa3Api::SharedSa3Api()
{
  return SharedSa3ApiInstance();
}

const Sa3Api* LoadSa3Api(std::string& error)
{
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  Sa3Api& api = SharedSa3ApiInstance();
  return api.Load(error) ? &api : nullptr;
}

const Sa3Api* LoadedSa3Api()
{
  const Sa3Api& api = SharedSa3ApiInstance();
  return api.Ready() ? &api : nullptr;
}
}

SA3TextGenerationRequest LoadSharedTextGenerationRequest(std::string prompt,
                                                         double durationSeconds,
                                                         double bpm)
{
  SA3TextGenerationRequest request;
  request.durationSeconds = std::clamp(durationSeconds, 1.0, 300.0);
  request.bpm = bpm;

  const std::string savedModelsDir = LoadSetting("models_dir");
  if (!savedModelsDir.empty())
    request.modelsDir = savedModelsDir;
  else if (const char* envModels = std::getenv("SA3_MODELS_DIR"))
    request.modelsDir = *envModels ? envModels : SA3_DEMO_DEFAULT_MODELS_DIR;
  else
    request.modelsDir = SA3_DEMO_DEFAULT_MODELS_DIR;

  const std::string savedVariant = LoadSetting("variant");
  request.variant = (savedVariant == "small-music" || savedVariant == "small-sfx") ? "small-music" : "medium";
  request.peakNormalize = ParseBool(LoadSetting("peak_normalize_enabled"), true);
  request.peakNormalizeDb = ParseFloat(LoadSetting("peak_normalize_db"), 2.f, -6.f, 6.f);
  request.limiter = ParseBool(LoadSetting("limiter_enabled"), true);
  request.limiterCeilingDb = ParseFloat(LoadSetting("limiter_ceiling_db"), -0.3f, -6.f, 0.f);
  request.limiterKnee = ParseFloat(LoadSetting("limiter_knee"), 0.8f, 0.1f, 1.f);

  const auto creativeLoras = LoadCreativeLoraRegistry(request.variant);
  for (const auto& lora : creativeLoras)
  {
    if (lora.enabled && !lora.path.empty() && FileSizeBytes(lora.path) > 0)
      request.loras.push_back({lora.path, lora.strength});
  }

  if (request.variant == "medium" && ParseBool(LoadSetting("decoder_lora_same_l_enabled"), true))
  {
    const std::string decoderPath = LoadSetting("decoder_lora_same_l_path");
    if (!decoderPath.empty() && FileSizeBytes(decoderPath) > 0)
      request.loras.push_back({decoderPath, 1.f});
  }

  if (bpm > 0.0)
  {
    if (!prompt.empty()) prompt += ' ';
    prompt += std::to_string(static_cast<int>(std::llround(bpm))) + " bpm";
  }
  request.prompt = std::move(prompt);
  return request;
}

bool ValidateTextGenerationRequest(const SA3TextGenerationRequest& request, std::string& error)
{
  if (request.durationSeconds < 1.0 || request.durationSeconds > 300.0)
  {
    error = "generation duration must be between 1 and 300 seconds";
    return false;
  }
  if (request.modelsDir.empty())
  {
    error = "no models folder is configured; configure models in the VST3 settings first";
    return false;
  }

  std::vector<std::string> missing;
  if (!ModelSetComplete(request.modelsDir, request.variant, "f16", missing))
  {
    error = "the " + request.variant + " model set is incomplete";
    if (!missing.empty())
    {
      error += " (missing ";
      for (size_t i = 0; i < missing.size(); ++i)
        error += (i ? ", " : "") + missing[i];
      error += ")";
    }
    return false;
  }
  return true;
}

SA3RenderService::~SA3RenderService()
{
  StopWorker();
  TeardownContext();
}

bool SA3RenderService::StartTextGeneration(SA3TextGenerationRequest request)
{
  if (mBusy.exchange(true, std::memory_order_acq_rel))
    return false;

  if (mWorker.joinable())
    mWorker.join();

  std::string error;
  if (!ValidateTextGenerationRequest(request, error))
  {
    mBusy.store(false, std::memory_order_release);
    SetStatus(error);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mHasCompletedResult = false;
  }
  mCancelRequested.store(false, std::memory_order_release);
  mProgress.store(0.f, std::memory_order_release);
  const uint64_t requestId = mRequestId.fetch_add(1, std::memory_order_acq_rel) + 1;
  SetStatus("queued generation");
  mWorker = std::thread([this, requestId, request = std::move(request)]() mutable {
    WorkerMain(requestId, std::move(request));
  });
  return true;
}

void SA3RenderService::Cancel()
{
  if (!Busy())
    return;
  mCancelRequested.store(true, std::memory_order_release);
  SetStatus("cancelling generation");
}

std::string SA3RenderService::Status() const
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  return mStatus;
}

bool SA3RenderService::TakeCompletedResult(SA3RenderResult& result)
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  if (!mHasCompletedResult)
    return false;
  result = std::move(mCompletedResult);
  mCompletedResult = {};
  mHasCompletedResult = false;
  return true;
}

void SA3RenderService::WorkerMain(uint64_t requestId, SA3TextGenerationRequest request)
{
  auto cancelled = [&]() {
    return mCancelRequested.load(std::memory_order_acquire)
        || requestId != mRequestId.load(std::memory_order_acquire);
  };

  std::string loadError;
  const Sa3Api* sa3 = LoadSa3Api(loadError);
  if (!sa3)
  {
    PublishResult({false, false, "libsa3 load failed: " + loadError}, "libsa3 load failed");
    return;
  }

  if (mContext && (mContextModelsDir != request.modelsDir || mContextVariant != request.variant))
    TeardownContext();

  char error[2048] = {};
  if (!mContext)
  {
    SetStatus("loading " + request.variant + " models");
    sa3_config config = {};
    config.models_dir = request.modelsDir.c_str();
    config.variant = request.variant.c_str();
    config.encoding = "f16";
    mContext = sa3->init(&config, error, static_cast<int>(sizeof(error)));
    if (!mContext)
    {
      PublishResult({false, false, std::string("sa3_init failed: ") + error}, "model load failed");
      return;
    }
    mContextModelsDir = request.modelsDir;
    mContextVariant = request.variant;
  }

  if (cancelled())
  {
    PublishResult({false, true, {}}, "generation cancelled");
    return;
  }

  sa3_request_ex generation = {};
  generation.request.prompt = request.prompt.c_str();
  int frames = std::max(1, static_cast<int>(request.durationSeconds * 44100.0 / 4096.0 + 0.5));
  if (request.variant == "small-music" || request.variant == "small-sfx")
    frames = std::max(2, frames & ~1);
  generation.request.frames = frames;
  generation.request.steps = std::clamp(request.steps, 1, 100);
  generation.request.seed = request.seed;
  generation.request.cfg_scale = request.cfgScale;
  generation.request.duration_padding_sec = 6.f;
  generation.request.keep_models = 0;
  generation.request.loudness.set = 1;
  generation.request.loudness.peak_normalize = request.peakNormalize ? 1 : 0;
  generation.request.loudness.peak_normalize_db = request.peakNormalizeDb;
  generation.request.loudness.limiter = request.limiter ? 1 : 0;
  generation.request.loudness.limiter_ceiling_db = request.limiterCeilingDb;
  generation.request.loudness.limiter_knee = request.limiterKnee;
  generation.request.loudness.latent_rescale = 1.f;
  generation.request.loudness.latent_shift = 0.f;
  static const char* kDistShiftNames[4] = {"LogSNR", "Flux", "Full", "None"};
  generation.request.dist_shift = kDistShiftNames[std::clamp(request.distShift, 0, 3)];
  generation.decode_chunk_size = 128;
  generation.decode_overlap = 32;

  std::vector<const char*> loraPaths;
  std::vector<float> loraStrengths;
  for (const auto& lora : request.loras)
  {
    loraPaths.push_back(lora.path.c_str());
    loraStrengths.push_back(lora.strength);
  }
  generation.request.n_loras = static_cast<int>(loraPaths.size());
  generation.request.lora_names = loraPaths.empty() ? nullptr : loraPaths.data();
  generation.request.lora_strengths = loraStrengths.empty() ? nullptr : loraStrengths.data();

  struct ProgressContext
  {
    SA3RenderService* service;
    uint64_t requestId;
  } progressContext{this, requestId};
  generation.request.user = &progressContext;
  generation.request.on_progress = [](void* user, const char* stage, int step, int total, float fraction) {
    auto* progress = static_cast<ProgressContext*>(user);
    if (!progress || progress->requestId != progress->service->mRequestId.load(std::memory_order_acquire))
      return;
    progress->service->mProgress.store(fraction, std::memory_order_release);
    char status[192] = {};
    std::snprintf(status, sizeof(status), "%s %d/%d %.0f%%", stage ? stage : "generating",
                  step, total, fraction * 100.f);
    progress->service->SetStatus(status);
  };
  generation.cancel_user = &progressContext;
  generation.should_cancel = [](void* user) -> int {
    auto* progress = static_cast<ProgressContext*>(user);
    if (!progress)
      return 1;
    return progress->service->mCancelRequested.load(std::memory_order_acquire)
        || progress->requestId != progress->service->mRequestId.load(std::memory_order_acquire);
  };

  SetStatus("generating audio");
  sa3_audio audio = {};
  const int rc = sa3->generateEx(mContext, &generation, &audio, error, static_cast<int>(sizeof(error)));
  if (rc != 0)
  {
    if (cancelled())
      PublishResult({false, true, {}}, "generation cancelled");
    else
      PublishResult({false, false, std::string("sa3 failed: ") + error}, "generation failed");
    return;
  }

  if (cancelled())
  {
    sa3->freeAudio(&audio);
    PublishResult({false, true, {}}, "generation cancelled");
    return;
  }

  if (!audio.samples || audio.n_samp <= 0 || audio.n_ch <= 0 || audio.sample_rate <= 0)
  {
    sa3->freeAudio(&audio);
    PublishResult({false, false, "libsa3 returned an empty or invalid audio buffer"}, "generation failed");
    return;
  }

  SA3RenderResult result;
  result.ok = true;
  result.seed = RequestableSeed(audio.seed);
  result.audio.numSamples = audio.n_samp;
  result.audio.sampleRate = std::max(1, audio.sample_rate);
  result.audio.channels.assign(static_cast<size_t>(audio.n_ch),
                               std::vector<float>(static_cast<size_t>(audio.n_samp), 0.f));
  for (int channel = 0; channel < audio.n_ch; ++channel)
  {
    const float* source = audio.samples + static_cast<size_t>(channel) * audio.n_samp;
    std::copy(source, source + audio.n_samp, result.audio.channels[static_cast<size_t>(channel)].begin());
  }
  sa3->freeAudio(&audio);
  PublishResult(std::move(result), "generation complete");
}

void SA3RenderService::SetStatus(std::string status)
{
  std::lock_guard<std::mutex> lock(mStateMutex);
  mStatus = std::move(status);
}

void SA3RenderService::PublishResult(SA3RenderResult result, std::string status)
{
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mCompletedResult = std::move(result);
    mHasCompletedResult = true;
    mStatus = std::move(status);
  }
  mProgress.store(0.f, std::memory_order_release);
  mBusy.store(false, std::memory_order_release);
}

void SA3RenderService::StopWorker()
{
  mCancelRequested.store(true, std::memory_order_release);
  mRequestId.fetch_add(1, std::memory_order_acq_rel);
  if (mWorker.joinable())
    mWorker.join();
  mBusy.store(false, std::memory_order_release);
  mCancelRequested.store(false, std::memory_order_release);
}

void SA3RenderService::TeardownContext()
{
  if (mContext)
  {
    if (const Sa3Api* sa3 = LoadedSa3Api())
      sa3->freeContext(mContext);
    mContext = nullptr;
  }
  mContextModelsDir.clear();
  mContextVariant.clear();
}

}

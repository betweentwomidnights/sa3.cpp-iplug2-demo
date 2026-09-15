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

const char* OperationName(SA3RenderOperation operation)
{
  switch (operation)
  {
    case SA3RenderOperation::Transform: return "transform";
    case SA3RenderOperation::Continue: return "continuation";
    default: return "generation";
  }
}

std::vector<float> ToPlanar(const RecordingSnapshot& audio)
{
  const int channels = static_cast<int>(audio.channels.size());
  const int samples = std::max(0, audio.numSamples);
  std::vector<float> planar(static_cast<size_t>(channels) * samples, 0.f);
  for (int channel = 0; channel < channels; ++channel)
  {
    const size_t available = std::min(static_cast<size_t>(samples), audio.channels[static_cast<size_t>(channel)].size());
    std::copy_n(audio.channels[static_cast<size_t>(channel)].data(), available,
                planar.data() + static_cast<size_t>(channel) * samples);
  }
  return planar;
}

struct Sa3Api
{
  using GetApiFn = const sa3_api_v1* (SA3_CALL *)(uint32_t);

#ifdef _WIN32
  HMODULE module = nullptr;
#elif defined(__APPLE__)
  void* module = nullptr;
#endif
  const sa3_api_v1* api = nullptr;

  bool Ready() const noexcept
  {
#if defined(_WIN32) || defined(__APPLE__)
    return module && api;
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

    GetApiFn getApi = nullptr;
    if (!Resolve(getApi, "sa3_get_api", error)) return false;
    api = getApi(SA3_ABI_VERSION_1);
    if (!api || api->abi_version != SA3_ABI_VERSION_1 || api->size < SA3_API_V1_MIN_SIZE)
    {
      error = "libsa3 does not provide the complete C ABI V1 table";
      return false;
    }
    return true;
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
    GetApiFn getApi = nullptr;
    if (!Resolve(getApi, "sa3_get_api", error)) return false;
    api = getApi(SA3_ABI_VERSION_1);
    if (!api || api->abi_version != SA3_ABI_VERSION_1 || api->size < SA3_API_V1_MIN_SIZE)
    {
      error = "libsa3 does not provide the complete C ABI V1 table";
      return false;
    }
    return true;
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

SA3RenderRequest LoadSharedRenderRequest(std::string prompt,
                                         double durationSeconds,
                                         double bpm,
                                         SA3RenderOperation operation)
{
  SA3RenderRequest request;
  request.operation = operation;
  request.durationSeconds = durationSeconds;
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
  request.keepModelsResident = ParseBool(LoadSetting("keep_models_resident"), false);

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

bool ValidateRenderRequest(const SA3RenderRequest& request, std::string& error)
{
  if (request.durationSeconds < 1.0 || request.durationSeconds > 300.0)
  {
    error = std::string(OperationName(request.operation)) + " duration must be between 1 and 300 seconds";
    return false;
  }
  if (request.operation != SA3RenderOperation::Generate)
  {
    if (request.sourceAudio.numSamples <= 0 || request.sourceAudio.sampleRate <= 0
        || request.sourceAudio.channels.empty())
    {
      error = std::string(OperationName(request.operation)) + " needs captured source audio";
      return false;
    }
    for (const auto& channel : request.sourceAudio.channels)
    {
      if (channel.size() < static_cast<size_t>(request.sourceAudio.numSamples))
      {
        error = "captured source audio is incomplete";
        return false;
      }
    }
    const double sourceSeconds = static_cast<double>(request.sourceAudio.numSamples)
                               / request.sourceAudio.sampleRate;
    const double outputSeconds = request.operation == SA3RenderOperation::Continue
      ? sourceSeconds + request.durationSeconds : sourceSeconds;
    if (outputSeconds > 300.0)
    {
      error = std::string(OperationName(request.operation)) + " output would exceed 300 seconds";
      return false;
    }
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

bool SA3RenderService::StartRender(SA3RenderRequest request)
{
  if (mBusy.exchange(true, std::memory_order_acq_rel))
    return false;

  if (mWorker.joinable())
    mWorker.join();

  std::string error;
  if (!ValidateRenderRequest(request, error))
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
  SetStatus(std::string("queued ") + OperationName(request.operation));
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
  SetStatus("cancelling render");
}

void SA3RenderService::ReleaseModels()
{
  if (Busy())
    return;
  if (mWorker.joinable())
    mWorker.join();
  TeardownContext();
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

void SA3RenderService::WorkerMain(uint64_t requestId, SA3RenderRequest request)
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

  sa3_error_v1 error = {};
  error.size = sizeof(error);
  sa3->api->error_init(&error);
  if (!mContext)
  {
    SetStatus("loading " + request.variant + " models");
    sa3_context_config_v1 config = {};
    config.size = sizeof(config);
    sa3->api->context_config_init(&config);
    config.models_dir = request.modelsDir.c_str();
    config.variant = request.variant.c_str();
    config.dit_encoding = "f16";
    const sa3_status_v1 status = sa3->api->context_create(&config, &mContext, &error);
    if (status != SA3_STATUS_OK_V1)
    {
      PublishResult({false, false, std::string("sa3 context failed: ") + error.message}, "model load failed");
      return;
    }
    mContextModelsDir = request.modelsDir;
    mContextVariant = request.variant;
  }

  if (cancelled())
  {
    PublishResult({false, true, {}}, "render cancelled");
    return;
  }

  const bool hasSource = request.operation != SA3RenderOperation::Generate;
  std::vector<float> sourcePlanar;
  if (hasSource)
    sourcePlanar = ToPlanar(request.sourceAudio);

  sa3_request_v1 generation = {};
  generation.size = sizeof(generation);
  sa3->api->request_init(&generation);
  generation.operation = request.operation == SA3RenderOperation::Transform ? SA3_OPERATION_TRANSFORM_V1
                       : request.operation == SA3RenderOperation::Continue ? SA3_OPERATION_CONTINUE_V1
                                                                            : SA3_OPERATION_GENERATE_V1;
  generation.prompt = request.prompt.c_str();
  generation.duration_seconds = request.durationSeconds;
  generation.steps = std::clamp(request.steps, 1, 100);
  generation.seed = request.seed;
  generation.cfg_scale = request.cfgScale;
  generation.distribution_shift = static_cast<sa3_distribution_shift_v1>(std::clamp(request.distShift, 0, 3));
  generation.residency = request.keepModelsResident ? SA3_RESIDENCY_RESIDENT_V1
                                                    : SA3_RESIDENCY_FRUGAL_V1;
  generation.loudness.peak_normalize = request.peakNormalize ? 1 : 0;
  generation.loudness.peak_normalize_db = request.peakNormalizeDb;
  generation.loudness.limiter = request.limiter ? 1 : 0;
  generation.loudness.limiter_ceiling_db = request.limiterCeilingDb;
  generation.loudness.limiter_knee = request.limiterKnee;
  generation.encode_chunk_size = hasSource ? 128 : 0;
  generation.decode_chunk_size = 128;
  generation.input_audio.samples = hasSource ? sourcePlanar.data() : nullptr;
  generation.input_audio.n_samples = hasSource ? static_cast<uint64_t>(request.sourceAudio.numSamples) : 0;
  generation.input_audio.n_channels = hasSource ? static_cast<uint32_t>(request.sourceAudio.channels.size()) : 0;
  generation.input_audio.sample_rate = hasSource ? static_cast<uint32_t>(request.sourceAudio.sampleRate) : 0;
  generation.transform_noise_level = std::clamp(request.initNoiseLevel, 0.f, 1.f);

  std::vector<sa3_adapter_v1> adapters;
  adapters.reserve(request.loras.size());
  for (const auto& lora : request.loras)
  {
    sa3_adapter_v1 adapter = {};
    adapter.size = sizeof(adapter);
    sa3->api->adapter_init(&adapter);
    adapter.path_or_name = lora.path.c_str();
    adapter.strength = lora.strength;
    adapters.push_back(adapter);
  }
  generation.adapters = adapters.empty() ? nullptr : adapters.data();
  generation.adapter_count = static_cast<uint32_t>(adapters.size());

  struct ProgressContext
  {
    SA3RenderService* service;
    uint64_t requestId;
  } progressContext{this, requestId};
  generation.callback_user = &progressContext;
  generation.on_progress = [](void* user, const sa3_progress_v1* update) {
    auto* progress = static_cast<ProgressContext*>(user);
    if (!progress || !update || progress->requestId != progress->service->mRequestId.load(std::memory_order_acquire))
      return;
    progress->service->mProgress.store(update->fraction, std::memory_order_release);
    char status[192] = {};
    std::snprintf(status, sizeof(status), "%s %d/%d %.0f%%",
                  update->stage_name ? update->stage_name : "generating",
                  update->step, update->total, update->fraction * 100.f);
    progress->service->SetStatus(status);
  };
  generation.should_cancel = [](void* user) -> int32_t {
    auto* progress = static_cast<ProgressContext*>(user);
    if (!progress)
      return 1;
    return progress->service->mCancelRequested.load(std::memory_order_acquire)
        || progress->requestId != progress->service->mRequestId.load(std::memory_order_acquire);
  };

  SetStatus(std::string(OperationName(request.operation)) + " in progress");
  sa3_result_v1 audio = {};
  audio.size = sizeof(audio);
  sa3->api->result_init(&audio);
  const sa3_status_v1 status = sa3->api->generate(mContext, &generation, &audio, &error);
  if (status != SA3_STATUS_OK_V1)
  {
    if (status == SA3_STATUS_CANCELLED_V1 || cancelled())
      PublishResult({false, true, {}}, "render cancelled");
    else
      PublishResult({false, false, std::string("sa3 failed: ") + error.message},
                    std::string(OperationName(request.operation)) + " failed");
    return;
  }

  if (cancelled())
  {
    sa3->api->result_free(&audio);
    PublishResult({false, true, {}}, "render cancelled");
    return;
  }

  if (!audio.samples || audio.n_samples == 0 || audio.n_channels == 0 || audio.sample_rate == 0)
  {
    sa3->api->result_free(&audio);
    PublishResult({false, false, "libsa3 returned an empty or invalid audio buffer"},
                  std::string(OperationName(request.operation)) + " failed");
    return;
  }

  SA3RenderResult result;
  result.ok = true;
  result.seed = RequestableSeed(audio.seed);
  const int outputSamples = static_cast<int>(audio.n_samples);
  result.audio.numSamples = outputSamples;
  result.audio.sampleRate = std::max(1, static_cast<int>(audio.sample_rate));
  result.audio.channels.assign(static_cast<size_t>(audio.n_channels),
                               std::vector<float>(static_cast<size_t>(outputSamples), 0.f));
  for (uint32_t channel = 0; channel < audio.n_channels; ++channel)
  {
    const float* source = audio.samples + static_cast<size_t>(channel) * audio.n_samples;
    std::copy(source, source + outputSamples, result.audio.channels[static_cast<size_t>(channel)].begin());
  }
  sa3->api->result_free(&audio);
  PublishResult(std::move(result), std::string(OperationName(request.operation)) + " complete");
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
      sa3->api->context_destroy(mContext);
    mContext = nullptr;
  }
  mContextModelsDir.clear();
  mContextVariant.clear();
}

}

#include "SA3IPlug2Demo.h"
#include "DemoUIPrimitives.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif
#ifdef __APPLE__
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <thread>

// The build system (CMakeLists) always sets this from SA3_CPP_DIR; overridable at runtime with SA3_MODELS_DIR.
#ifndef SA3_DEMO_DEFAULT_MODELS_DIR
#define SA3_DEMO_DEFAULT_MODELS_DIR "models"
#endif

namespace
{
constexpr const char* kDemoFont = gary::ui::FontName;
constexpr int kCtrlTagMain = 1000;
constexpr uint32_t kPluginStateMagic = 0x53334133u; // "SA3" state chunk
constexpr uint32_t kPluginStateVersion = 1u;
constexpr int kMaxPersistedLoras = 64;

std::string CompactText(std::string text, size_t maxChars)
{
  if (text.size() <= maxChars)
    return text;
  if (maxChars <= 3)
    return text.substr(0, maxChars);
  return text.substr(0, maxChars - 3) + "...";
}

size_t FitChars(float width, float approxCharWidth, size_t minChars, size_t maxChars)
{
  const size_t fitted = static_cast<size_t>(std::max(1.f, width / std::max(1.f, approxCharWidth)));
  return std::clamp(fitted, minChars, maxChars);
}

int64_t RequestableSeed(uint64_t seed) noexcept
{
  return static_cast<int64_t>(seed & static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
}

int64_t ParseSeedText(const char* text, int64_t fallback) noexcept
{
  if (!text)
    return fallback;

  while (std::isspace(static_cast<unsigned char>(*text)) != 0)
    ++text;
  if (*text == '\0' || *text == '-')
    return fallback;

  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (end == text)
    return fallback;
  while (end && std::isspace(static_cast<unsigned char>(*end)) != 0)
    ++end;
  if (end && *end != '\0')
    return fallback;
  if (errno == ERANGE || parsed > static_cast<unsigned long long>(std::numeric_limits<int64_t>::max()))
    return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(parsed);
}

bool ParseBoolSetting(const std::string& text, bool fallback) noexcept
{
  if (text.empty()) return fallback;
  if (text == "1" || text == "true" || text == "on") return true;
  if (text == "0" || text == "false" || text == "off") return false;
  return fallback;
}

float ParseFloatSetting(const std::string& text, float fallback, float lo, float hi) noexcept
{
  if (text.empty()) return fallback;
  char* end = nullptr;
  const float parsed = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || (end && *end != '\0') || !std::isfinite(parsed)) return fallback;
  return std::clamp(parsed, lo, hi);
}

std::string SettingFloat(float value)
{
  char text[32] = {};
  std::snprintf(text, sizeof text, "%.3f", value);
  return text;
}

std::string FileNameFromPath(const std::string& path)
{
  const auto slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string FormatBytes(uint64_t bytes)
{
  char text[64] = {};
  if (bytes >= 1024ull * 1024ull)
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  else if (bytes >= 1024ull)
    std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  else
    std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
  return text;
}

std::vector<float> ToPlanar(const gary::RecordingSnapshot& snapshot)
{
  std::vector<float> out((size_t)snapshot.numSamples * snapshot.channels.size(), 0.f);
  for (size_t c = 0; c < snapshot.channels.size(); ++c)
  {
    const auto& src = snapshot.channels[c];
    const int n = std::min(snapshot.numSamples, static_cast<int>(src.size()));
    if (n > 0)
      std::copy(src.begin(), src.begin() + n, out.begin() + (ptrdiff_t)c * snapshot.numSamples);
  }
  return out;
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
#ifdef _WIN32
    return module && init && generateEx && freeAudio && freeContext;
#elif defined(__APPLE__)
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
    const std::wstring dir = ModuleDirectory(error);
    if (dir.empty())
      return false;

    const std::wstring dllPath = dir + L"\\sa3.dll";
    module = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
    {
      error = "LoadLibraryExW failed for " + WideToUtf8(dllPath) + " (win32 " + std::to_string(GetLastError()) + ")";
      return false;
    }

    if (!Resolve(init, "sa3_init", error) ||
        !Resolve(generateEx, "sa3_generate_ex", error) ||
        !Resolve(freeAudio, "sa3_free_audio", error) ||
        !Resolve(freeContext, "sa3_free", error))
    {
      return false;
    }

    return true;
#elif defined(__APPLE__)
    const std::string dir = ModuleDirectory(error);
    if (dir.empty())
      return false;

    const std::string dylibPath = dir + "/libsa3.dylib";
    module = dlopen(dylibPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
      const char* detail = dlerror();
      error = "dlopen failed for " + dylibPath + (detail ? std::string(": ") + detail : std::string());
      return false;
    }

    if (!Resolve(init, "sa3_init", error) ||
        !Resolve(generateEx, "sa3_generate_ex", error) ||
        !Resolve(freeAudio, "sa3_free_audio", error) ||
        !Resolve(freeContext, "sa3_free", error))
    {
      return false;
    }

    return true;
#else
    error = "runtime libsa3 loading is only implemented for Windows/macOS in this demo";
    return false;
#endif
  }

private:
#ifdef _WIN32
  static std::string WideToUtf8(const std::wstring& text)
  {
    if (text.empty())
      return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
      return {};
    std::string out((size_t)required - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), required, nullptr, nullptr);
    return out;
  }

  static std::wstring ModuleDirectory(std::string& error)
  {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self))
    {
      error = "GetModuleHandleExW failed (win32 " + std::to_string(GetLastError()) + ")";
      return {};
    }

    std::vector<wchar_t> buffer(1024);
    for (;;)
    {
      const DWORD length = GetModuleFileNameW(self, buffer.data(), (DWORD)buffer.size());
      if (length == 0)
      {
        error = "GetModuleFileNameW failed (win32 " + std::to_string(GetLastError()) + ")";
        return {};
      }
      if (length < buffer.size() - 1)
      {
        std::wstring path(buffer.data(), length);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
          error = "could not derive module directory from " + WideToUtf8(path);
          return {};
        }
        return path.substr(0, slash);
      }
      buffer.resize(buffer.size() * 2);
    }
  }

  template <typename Fn>
  bool Resolve(Fn& fn, const char* name, std::string& error)
  {
    FARPROC proc = GetProcAddress(module, name);
    if (!proc)
    {
      error = std::string("GetProcAddress failed for ") + name + " (win32 " + std::to_string(GetLastError()) + ")";
      return false;
    }
    fn = reinterpret_cast<Fn>(proc);
    return true;
  }
#elif defined(__APPLE__)
  static std::string ModuleDirectory(std::string& error)
  {
    Dl_info info = {};
    if (dladdr(reinterpret_cast<const void*>(&ModuleDirectory), &info) == 0 || !info.dli_fname)
    {
      error = "dladdr failed while resolving module directory";
      return {};
    }

    std::string path(info.dli_fname);
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
    {
      error = "could not derive module directory from " + path;
      return {};
    }
    return path.substr(0, slash);
  }

  template <typename Fn>
  bool Resolve(Fn& fn, const char* name, std::string& error)
  {
    dlerror();
    void* proc = dlsym(module, name);
    if (!proc)
    {
      const char* detail = dlerror();
      error = std::string("dlsym failed for ") + name + (detail ? std::string(": ") + detail : std::string());
      return false;
    }
    fn = reinterpret_cast<Fn>(proc);
    return true;
  }
#endif
};

Sa3Api& SharedSa3Api()
{
  static Sa3Api api;
  return api;
}

const Sa3Api* LoadSa3Api(std::string& error)
{
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  Sa3Api& api = SharedSa3Api();
  return api.Load(error) ? &api : nullptr;
}

const Sa3Api* LoadedSa3Api()
{
  const Sa3Api& api = SharedSa3Api();
  return api.Ready() ? &api : nullptr;
}

class SA3DemoControl final : public IControl
{
  enum class Hit
  {
    None,
    Prompt,
    Dice,
    Models,
    StatusCopy,
    Settings,
    SettingsClose,
    DecoderToggle,
    DecoderDownload,
    DecoderChoose,
    DecoderClear,
    NormalizeToggle,
    LimiterToggle,
    PeakDbSlider,
    LimiterCeilingSlider,
    LimiterKneeSlider,
    OutputDefaults,
    OutputRaw,
    TabGenerate,
    TabTransform,
    TabContinue,
    DurationSlider,
    StepsSlider,
    CfgSlider,
    NoiseSlider,
    SeedToggle,
    SeedField,
    BpmToggle,
    BpmValue,
    KeyRoot,
    KeyMode,
    DistShift,
    LoopToggle,
    LoopBars,
    Run,
    AddLora,
    LoraToggle,
    LoraRemove,
    LoraSlider,
    OutputPlay,
    OutputStop,
    SaveBuffer
  };

  enum class Slider
  {
    None,
    Duration,
    Steps,
    Cfg,
    Noise,
    Lora,
    Bpm,
    PeakDb,
    LimiterCeiling,
    LimiterKnee
  };

  enum class EditTarget
  {
    None,
    Prompt,
    Seed
  };

  struct HitResult
  {
    Hit hit = Hit::None;
    size_t index = 0;
  };

public:
  SA3DemoControl(const IRECT& bounds, SA3IPlug2Demo& plugin)
  : IControl(bounds)
  , mPlugin(plugin)
  {
    SetTooltip("Embedded libsa3 test surface");
    SetTextEntryLength(2048);
  }

  void Draw(IGraphics& g) override
  {
    using namespace gary::ui;
    g.FillRect(Background(), mRECT);

    const IRECT shell = mRECT.GetPadded(-14.f);
    g.FillRoundRect(Panel(), shell, 7.f);
    g.DrawRoundRect(Frame(), shell, 7.f);

    const float left = shell.L + 18.f;
    const float right = shell.R - 18.f;
    mStatusRect = {};
    mStatusCopyRect = {};
    float y = shell.T + 14.f;
    const SA3IPlug2Demo::RenderMode mode = mPlugin.CurrentRenderMode();

    g.DrawText(IText(20.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               "sa3", IRECT(left, y, left + 120.f, y + 26.f));
    // "models" button (download / point-at-folder) + a ready/no-models indicator to its left.
    mModelsBtnRect = IRECT(right - 62.f, y + 2.f, right, y + 24.f);
    DrawButton(g, mModelsBtnRect, "models", kDemoFont);
    mSettingsBtnRect = IRECT(mModelsBtnRect.L - 70.f, y + 2.f, mModelsBtnRect.L - 8.f, y + 24.f);
    DrawButton(g, mSettingsBtnRect, mSettingsOpen ? "close" : "settings", kDemoFont, mSettingsOpen);
    {
      const bool present = mPlugin.ModelsPresent();
      const bool downloading = mPlugin.Downloading();
      const IColor c = downloading ? TextDim() : present ? Green() : Red();
      const std::string label = downloading ? "downloading…"
                              : present ? (mPlugin.ModelVariant() + " ready")
                                        : "no models — set up →";
      g.DrawText(IText(12.f, c, kDemoFont, EAlign::Far, EVAlign::Middle),
                 label.c_str(), IRECT(left + 124.f, y, mSettingsBtnRect.L - 8.f, y + 26.f));
    }
    y += 30.f;

    if (mSettingsOpen)
    {
      DrawSettings(g, IRECT(left, y, right, shell.B - 14.f));
      return;
    }

    // The status bar doubles as the download meter (download runs off the audio thread, so it's safe here).
    const bool downloading = mPlugin.Downloading();
    const float progress = std::clamp(downloading ? mPlugin.DownloadProgress() : mPlugin.Progress(), 0.f, 1.f);
    const IRECT statusRect(left, y, right, y + 22.f);
    mStatusRect = statusRect;
    mStatusCopyRect = IRECT(statusRect.R - 24.f, statusRect.T, statusRect.R, statusRect.B);
    g.FillRoundRect(PanelDark(), statusRect, 3.f);
    if (mPlugin.Busy() || downloading)
      g.FillRoundRect(RedDim(), IRECT(statusRect.L, statusRect.T, statusRect.L + statusRect.W() * progress, statusRect.B), 3.f);
    g.DrawRoundRect(FrameSoft(), statusRect, 3.f);
    const std::string status = mPlugin.StatusText();
    const IRECT statusTextRect(statusRect.L + 8.f, statusRect.T, mStatusCopyRect.L - 5.f, statusRect.B);
    const size_t statusMaxChars = FitChars(statusTextRect.W(), 6.5f, 18, 96);
    mStatusTruncated = status.size() > statusMaxChars;
    if (!mStatusTruncated && mStatusHovered)
    {
      mStatusHovered = false;
      SetTooltip("Embedded libsa3 test surface");
    }
    g.DrawText(IText(12.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               CompactText(status, statusMaxChars).c_str(), statusTextRect);
    DrawCopyIcon(g, mStatusCopyRect.GetPadded(-4.f),
                 std::chrono::steady_clock::now() < mCopyFlashUntil ? Green()
                 : mCopyHovered ? COLOR_WHITE : TextDim());
    y += 30.f;

    const IRECT sourceRect(left, y, right, y + 148.f);
    DrawWaveformPanel(g, sourceRect, "source", mPlugin.SourceStatusText(), mPlugin.SourceWaveform((int)sourceRect.W()), false);
    y += 158.f;

    DrawTabs(g, IRECT(left, y, right, y + 30.f), mode);
    y += 40.f;

    DrawPrompt(g, IRECT(left, y, right, y + 48.f), mode);
    y += 56.f;

    y = DrawModeControls(g, IRECT(left, y, right, y + 104.f), mode) + 8.f;
    const float loraPanelHeight = mPlugin.Loras().empty() ? 76.f : 108.f;
    y = DrawLoraPanel(g, IRECT(left, y, right, y + loraPanelHeight)) + 8.f;

    mRunRect = IRECT(left, y, left + 180.f, y + 32.f);
    const bool canRender = mPlugin.Busy() || mPlugin.CanRender(mode);   // needs models (+ a snapshot for a2a)
    DrawButton(g, mRunRect, mPlugin.Busy() ? "cancel" : ActionLabel(mode), kDemoFont, false, canRender);
    std::string runHint;
    if (mPlugin.Downloading())            runHint = "download in progress…";
    else if (!mPlugin.ModelsPresent())    runHint = "click 'models' to download or locate your models";
    else if (!canRender)                  runHint = "drop audio or save a recorded buffer first";
    else if (mPlugin.TransportRunning())  runHint = "host rolling: recording input";
    else                                  runHint = "host stopped";
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               runHint.c_str(), IRECT(mRunRect.R + 10.f, y, right, y + 32.f));
    y += 42.f;

    const float outputHeight = std::min(190.f, std::max(150.f, shell.B - y - 14.f));
    const IRECT outputRect(left, y, right, y + outputHeight);
    DrawOutputPanel(g, outputRect);
    if (mStatusHovered && mStatusTruncated)
      DrawStatusTooltip(g, status);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    mOutputPointerDown = false;
    mOutputDragStarted = false;
    mOutputDragStartX = x;
    mOutputDragStartY = y;
    mActiveSlider = Slider::None;

    const HitResult hit = HitTest(x, y);
    switch (hit.hit)
    {
      case Hit::Prompt:
      {
        const auto mode = mPlugin.CurrentRenderMode();
        if (GetUI())
        {
          mEditTarget = EditTarget::Prompt;
          mEditPromptMode = mode;
          const IText promptEntryText = IText(14.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle)
                                          .WithTEColors(gary::ui::PanelDark(), COLOR_WHITE);
          GetUI()->CreateTextEntry(*this,
                                   promptEntryText,
                                   mPromptRect,
                                   mPlugin.PromptForMode(mode).c_str(),
                                   0);
        }
        return;
      }
      case Hit::Dice:          mPlugin.RollPromptForCurrentMode(); SetDirty(false); return;
      case Hit::Models:        OpenModelsMenu(); return;
      case Hit::StatusCopy:    CopyStatusToClipboard(); return;
      case Hit::Settings:
      case Hit::SettingsClose: mSettingsOpen = !mSettingsOpen; SetDirty(false); return;
      case Hit::DecoderToggle: mPlugin.SetDecoderLoraEnabled(!mPlugin.DecoderLoraEnabled()); SetDirty(false); return;
      case Hit::DecoderDownload:
        if (mPlugin.DecoderLoraDownloading()) mPlugin.CancelModelDownload();
        else mPlugin.StartDecoderLoraDownload();
        SetDirty(false); return;
      case Hit::DecoderChoose: mPlugin.ImportDecoderLoraFromDialog(); SetDirty(false); return;
      case Hit::DecoderClear: mPlugin.ClearDecoderLoraSelection(); SetDirty(false); return;
      case Hit::NormalizeToggle: mPlugin.SetPeakNormalizeEnabled(!mPlugin.PeakNormalizeEnabled()); SetDirty(false); return;
      case Hit::LimiterToggle: mPlugin.SetLimiterEnabled(!mPlugin.LimiterEnabled()); SetDirty(false); return;
      case Hit::OutputDefaults: mPlugin.ResetOutputProcessing(); SetDirty(false); return;
      case Hit::OutputRaw: mPlugin.SetRawOutputProcessing(); SetDirty(false); return;
      case Hit::TabGenerate:  mPlugin.SetCurrentRenderMode(SA3IPlug2Demo::RenderMode::Text); SetDirty(false); return;
      case Hit::TabTransform: mPlugin.SetCurrentRenderMode(SA3IPlug2Demo::RenderMode::Transform); SetDirty(false); return;
      case Hit::TabContinue:  mPlugin.SetCurrentRenderMode(SA3IPlug2Demo::RenderMode::Continue); SetDirty(false); return;
      case Hit::DurationSlider: mActiveSlider = Slider::Duration; UpdateSliderFromX(x); return;
      case Hit::StepsSlider:    mActiveSlider = Slider::Steps;    UpdateSliderFromX(x); return;
      case Hit::CfgSlider:      mActiveSlider = Slider::Cfg;      UpdateSliderFromX(x); return;
      case Hit::NoiseSlider:    mActiveSlider = Slider::Noise;    UpdateSliderFromX(x); return;
      case Hit::SeedToggle:     mPlugin.ToggleUseSeed(); SetDirty(false); return;
      case Hit::SeedField:
        if (GetUI())
        {
          const int64_t editSeed = mPlugin.UseSeed() ? mPlugin.SeedValue()
                                  : mPlugin.HasLastSeed() ? mPlugin.LastSeed()
                                                          : 0;
          mEditTarget = EditTarget::Seed;
          const IText seedEntryText = IText(13.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle)
                                      .WithTEColors(gary::ui::PanelDark(), COLOR_WHITE);
          const std::string seedText = std::to_string(std::max<int64_t>(0, editSeed));
          GetUI()->CreateTextEntry(*this, seedEntryText, mSeedFieldRect, seedText.c_str(), 0);
        }
        return;
      case Hit::BpmToggle:      mPlugin.ToggleAppendBpm(); SetDirty(false); return;
      case Hit::BpmValue:       mActiveSlider = Slider::Bpm; return;   // vertical drag adjusts (see OnMouseDrag)
      case Hit::KeyRoot:        OpenKeyRootMenu(); return;
      case Hit::KeyMode:        OpenKeyModeMenu(); return;
      case Hit::DistShift:      OpenDistShiftMenu(); return;
      case Hit::LoopToggle:                              // reveal/hide the 4/8/16 buttons
        mPlugin.SetLoopBars(mPlugin.LoopBars() > 0 ? 0 : 8);
        SetDirty(false);
        return;
      case Hit::LoopBars:
      {
        const int values[3] = {4, 8, 16};
        mPlugin.SetLoopBars(hit.index < 3 ? values[hit.index] : 8);
        SetDirty(false);
        return;
      }
      case Hit::LoraSlider:     mActiveSlider = Slider::Lora; mActiveLoraIndex = hit.index; UpdateSliderFromX(x); return;
      case Hit::PeakDbSlider: mActiveSlider = Slider::PeakDb; UpdateSliderFromX(x); return;
      case Hit::LimiterCeilingSlider: mActiveSlider = Slider::LimiterCeiling; UpdateSliderFromX(x); return;
      case Hit::LimiterKneeSlider: mActiveSlider = Slider::LimiterKnee; UpdateSliderFromX(x); return;
      case Hit::LoraToggle:     ToggleLora(hit.index); SetDirty(false); return;
      case Hit::LoraRemove:     mPlugin.RemoveLora(hit.index); SetDirty(false); return;
      case Hit::AddLora:        mPlugin.ImportLoraFromDialog(); SetDirty(false); return;
      case Hit::Run:
        if (mPlugin.Busy()) mPlugin.CancelRender();
        else if (mPlugin.CanRender(mPlugin.CurrentRenderMode())) mPlugin.StartRender(mPlugin.CurrentRenderMode());
        SetDirty(false);
        return;
      case Hit::OutputPlay:     mPlugin.ToggleOutputPlayback(); SetDirty(false); return;
      case Hit::OutputStop:     mPlugin.StopOutputPlayback(); SetDirty(false); return;
      case Hit::SaveBuffer:     mPlugin.SaveSourceToDisk(); SetDirty(false); return;
      case Hit::None: break;
    }

    if (mOutputWaveformRect.Contains(x, y))
    {
      mOutputPointerDown = true;
      SetDirty(false);
      return;
    }

    IControl::OnMouseDown(x, y, mod);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (mActiveSlider == Slider::Bpm)
    {
      mPlugin.AdjustBpm((double)(-dY) * 0.4);   // drag up = faster; ~2.5px per bpm, deliberate not twitchy
      SetDirty(false);
      return;
    }
    if (mActiveSlider != Slider::None)
    {
      UpdateSliderFromX(x);
      return;
    }

    if (mOutputPointerDown)
    {
      const float dx = x - mOutputDragStartX;
      const float dy = y - mOutputDragStartY;
      if (!mOutputDragStarted && std::sqrt(dx * dx + dy * dy) > 10.f)
      {
        mOutputDragStarted = true;
        mOutputPointerDown = false;
        const auto info = mPlugin.CreateOutputDragCopy();
        if (info.ok && GetUI())
          GetUI()->InitiateExternalFileDragDrop(info.path.c_str(), mOutputWaveformRect);
        SetDirty(false);
        return;
      }
    }
    IControl::OnMouseDrag(x, y, dX, dY, mod);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    if (mActiveSlider != Slider::None)
    {
      mActiveSlider = Slider::None;
      SetDirty(false);
      return;
    }

    if (mOutputPointerDown && !mOutputDragStarted && mOutputWaveformRect.Contains(x, y))
    {
      const float rel = std::clamp(x - mOutputWaveformRect.L, 0.f, std::max(1.f, mOutputWaveformRect.W()));
      const auto wf = mPlugin.OutputWaveform(16);
      if (wf.numSamples > 0)
        mPlugin.SeekOutputPlayback((rel / std::max(1.f, mOutputWaveformRect.W())) * (double)wf.numSamples / std::max(1, wf.sampleRate));
      mOutputPointerDown = false;
      SetDirty(false);
      return;
    }
    mOutputPointerDown = false;
    mOutputDragStarted = false;
    IControl::OnMouseUp(x, y, mod);
  }

  void OnMouseOver(float x, float y, const IMouseMod& mod) override
  {
    const bool statusHovered = mStatusTruncated && mStatusRect.Contains(x, y);
    const bool copyHovered = mStatusCopyRect.Contains(x, y);
    if (statusHovered != mStatusHovered || copyHovered != mCopyHovered)
    {
      mStatusHovered = statusHovered;
      mCopyHovered = copyHovered;
      SetTooltip(statusHovered ? mPlugin.StatusText().c_str() : "Embedded libsa3 test surface");
      SetDirty(false);
    }
    IControl::OnMouseOver(x, y, mod);
  }

  void OnMouseOut() override
  {
    mStatusHovered = false;
    mCopyHovered = false;
    SetTooltip("Embedded libsa3 test surface");
    IControl::OnMouseOut();
  }

  void OnDrop(const char* str) override
  {
    mPlugin.LoadDroppedAudioFile(str);
    SetDirty(false);
  }

  void OnDropMultiple(const std::vector<const char*>& paths) override
  {
    if (!paths.empty())
      OnDrop(paths[0]);
  }

  void OnPopupMenuSelection(IPopupMenu* pMenu, int) override
  {
    if (pMenu)
    {
      const int idx = pMenu->GetChosenItemIdx();
      if (idx >= 0)
      {
        if (mActivePopup == Popup::KeyRoot)         // 0=none, 1..12=C..B (matches KeyRoot)
          mPlugin.SetKeyRoot(idx);
        else if (mActivePopup == Popup::KeyMode)    // 0=major, 1=minor
          mPlugin.SetKeyMode(idx);
        else if (mActivePopup == Popup::DistShift)  // 0=LogSNR,1=Flux,2=Full,3=None
          mPlugin.SetDistShift(idx);
        else if (mActivePopup == Popup::Models)
        {
          const int action = (idx >= 0 && idx < (int)mModelsActions.size()) ? mModelsActions[idx] : kSep;
          switch (action)
          {
            case kCancelDl:  mPlugin.CancelModelDownload(); break;
            case kUseMedium: mPlugin.SelectVariant("medium"); break;
            case kUseSmall:  mPlugin.SelectVariant("small-music"); break;
            case kDlMedium:  StartDownloadFlow(0); break;
            case kDlSmall:   StartDownloadFlow(1); break;
            case kPoint:     PointAtFolderFlow(); break;
            default:         break;   // separator / no-op
          }
        }
      }
    }
    mActivePopup = Popup::None;
    SetDirty(false);
  }

  void OnTextEntryCompletion(const char* str, int valIdx) override
  {
    switch (mEditTarget)
    {
      case EditTarget::Prompt:
        mPlugin.SetPromptForMode(mEditPromptMode, str);
        break;
      case EditTarget::Seed:
      {
        const int64_t parsed = ParseSeedText(str, mPlugin.SeedValue());
        mPlugin.SetSeedValue(parsed);
        mPlugin.SetUseSeed(true);
        break;
      }
      case EditTarget::None:
        break;
    }
    mEditTarget = EditTarget::None;
    SetDirty(false);
  }

private:
  static const char* ActionLabel(SA3IPlug2Demo::RenderMode mode)
  {
    switch (mode)
    {
      case SA3IPlug2Demo::RenderMode::Text: return "generate";
      case SA3IPlug2Demo::RenderMode::Transform: return "transform";
      case SA3IPlug2Demo::RenderMode::Continue: return "continue";
    }
    return "generate";
  }

  HitResult HitTest(float x, float y) const
  {
    if (mSettingsBtnRect.Contains(x, y)) return {Hit::Settings, 0};
    if (mSettingsOpen)
    {
      if (mSettingsCloseRect.Contains(x, y)) return {Hit::SettingsClose, 0};
      if (mDecoderToggleRect.Contains(x, y)) return {Hit::DecoderToggle, 0};
      if (mDecoderDownloadRect.Contains(x, y)) return {Hit::DecoderDownload, 0};
      if (mDecoderChooseRect.Contains(x, y)) return {Hit::DecoderChoose, 0};
      if (mDecoderClearRect.Contains(x, y)) return {Hit::DecoderClear, 0};
      if (mNormalizeToggleRect.Contains(x, y)) return {Hit::NormalizeToggle, 0};
      if (mLimiterToggleRect.Contains(x, y)) return {Hit::LimiterToggle, 0};
      if (mPeakDbSliderRect.Contains(x, y)) return {Hit::PeakDbSlider, 0};
      if (mLimiterCeilingSliderRect.Contains(x, y)) return {Hit::LimiterCeilingSlider, 0};
      if (mLimiterKneeSliderRect.Contains(x, y)) return {Hit::LimiterKneeSlider, 0};
      if (mOutputDefaultsRect.Contains(x, y)) return {Hit::OutputDefaults, 0};
      if (mOutputRawRect.Contains(x, y)) return {Hit::OutputRaw, 0};
      return {};
    }
    if (mDiceRect.Contains(x, y)) return {Hit::Dice, 0};
    if (mModelsBtnRect.Contains(x, y)) return {Hit::Models, 0};
    if (mStatusCopyRect.Contains(x, y)) return {Hit::StatusCopy, 0};
    if (mPromptRect.Contains(x, y)) return {Hit::Prompt, 0};
    if (mGenerateTabRect.Contains(x, y)) return {Hit::TabGenerate, 0};
    if (mTransformTabRect.Contains(x, y)) return {Hit::TabTransform, 0};
    if (mContinueTabRect.Contains(x, y)) return {Hit::TabContinue, 0};
    if (mDurationSliderRect.Contains(x, y)) return {Hit::DurationSlider, 0};
    if (mStepsSliderRect.Contains(x, y)) return {Hit::StepsSlider, 0};
    if (mCfgSliderRect.Contains(x, y)) return {Hit::CfgSlider, 0};
    if (mNoiseSliderRect.Contains(x, y)) return {Hit::NoiseSlider, 0};
    if (mSeedToggleRect.Contains(x, y)) return {Hit::SeedToggle, 0};
    if (mSeedFieldRect.Contains(x, y)) return {Hit::SeedField, 0};
    if (mBpmToggleRect.Contains(x, y)) return {Hit::BpmToggle, 0};
    if (mBpmValueRect.Contains(x, y)) return {Hit::BpmValue, 0};
    if (mKeyRootRect.Contains(x, y)) return {Hit::KeyRoot, 0};
    if (mKeyModeRect.Contains(x, y)) return {Hit::KeyMode, 0};
    if (mDistShiftRect.Contains(x, y)) return {Hit::DistShift, 0};
    if (mLoopToggleRect.Contains(x, y)) return {Hit::LoopToggle, 0};
    for (size_t i = 0; i < mBarsRects.size(); ++i)
      if (mBarsRects[i].Contains(x, y)) return {Hit::LoopBars, i};
    for (size_t i = 0; i < mLoraSliderRects.size(); ++i)
      if (mLoraSliderRects[i].Contains(x, y)) return {Hit::LoraSlider, i};
    for (size_t i = 0; i < mLoraToggleRects.size(); ++i)
      if (mLoraToggleRects[i].Contains(x, y)) return {Hit::LoraToggle, i};
    for (size_t i = 0; i < mLoraRemoveRects.size(); ++i)
      if (mLoraRemoveRects[i].Contains(x, y)) return {Hit::LoraRemove, i};
    if (mAddLoraRect.Contains(x, y)) return {Hit::AddLora, 0};
    if (mRunRect.Contains(x, y)) return {Hit::Run, 0};
    if (mSaveBufferRect.Contains(x, y)) return {Hit::SaveBuffer, 0};
    if (mOutputPlayRect.Contains(x, y)) return {Hit::OutputPlay, 0};
    if (mOutputStopRect.Contains(x, y)) return {Hit::OutputStop, 0};
    return {};
  }

  void DrawCopyIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
  {
    const float w = std::max(7.f, bounds.W() * 0.58f);
    const float h = std::max(8.f, bounds.H() * 0.68f);
    const IRECT back(bounds.L + 1.f, bounds.T + 1.f, bounds.L + 1.f + w, bounds.T + 1.f + h);
    const IRECT front(bounds.R - w - 1.f, bounds.B - h - 1.f, bounds.R - 1.f, bounds.B - 1.f);
    g.DrawRoundRect(color, back, 1.5f, nullptr, 1.2f);
    g.FillRoundRect(gary::ui::PanelDark(), front, 1.5f);
    g.DrawRoundRect(color, front, 1.5f, nullptr, 1.2f);
  }

  void DrawStatusTooltip(IGraphics& g, const std::string& status)
  {
    if (!mStatusTruncated || status.empty() || mStatusRect.Empty()) return;
    using namespace gary::ui;
    const float approxLines = std::ceil((float)status.size() / 48.f);
    const float height = std::clamp(22.f + approxLines * 15.f, 50.f, 230.f);
    const IRECT tip(mStatusRect.L, mStatusRect.B + 5.f, mStatusRect.R,
                    std::min(mRECT.B - 18.f, mStatusRect.B + 5.f + height));
    g.FillRoundRect(IColor(250, 12, 12, 12), tip, 4.f);
    g.DrawRoundRect(Red(), tip, 4.f, nullptr, 1.2f);
    g.DrawMultiLineText(IText(11.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Top),
                        status.c_str(), tip.GetPadded(-9.f));
  }

  void CopyStatusToClipboard()
  {
    if (!GetUI()) return;
    const std::string status = mPlugin.StatusText();
    if (!status.empty() && GetUI()->SetTextInClipboard(status.c_str()))
      mCopyFlashUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    SetDirty(false);
  }

  void DrawTab(IGraphics& g, const IRECT& bounds, const char* label, bool active)
  {
    gary::ui::DrawTab(g, bounds, label, kDemoFont, active);
  }

  void DrawTabs(IGraphics& g, const IRECT& bounds, SA3IPlug2Demo::RenderMode mode)
  {
    const float gap = 8.f;
    const float w = (bounds.W() - gap * 2.f) / 3.f;
    mGenerateTabRect = IRECT(bounds.L, bounds.T, bounds.L + w, bounds.B);
    mTransformTabRect = IRECT(mGenerateTabRect.R + gap, bounds.T, mGenerateTabRect.R + gap + w, bounds.B);
    mContinueTabRect = IRECT(mTransformTabRect.R + gap, bounds.T, bounds.R, bounds.B);
    DrawTab(g, mGenerateTabRect, "generate", mode == SA3IPlug2Demo::RenderMode::Text);
    DrawTab(g, mTransformTabRect, "transform", mode == SA3IPlug2Demo::RenderMode::Transform);
    DrawTab(g, mContinueTabRect, "continue", mode == SA3IPlug2Demo::RenderMode::Continue);
  }

  void DrawPrompt(IGraphics& g, const IRECT& bounds, SA3IPlug2Demo::RenderMode mode)
  {
    using namespace gary::ui;
    g.DrawText(IText(12.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               "prompt", IRECT(bounds.L, bounds.T, bounds.L + 80.f, bounds.T + 16.f));
    mDiceRect = IRECT(bounds.R - 34.f, bounds.T + 18.f, bounds.R, bounds.B);
    DrawIconButton(g, mDiceRect, TransportIcon::Dice);
    mPromptRect = IRECT(bounds.L, bounds.T + 18.f, mDiceRect.L - 8.f, bounds.B);
    g.FillRoundRect(ButtonFill(), mPromptRect, 3.f);
    g.DrawRoundRect(Frame(), mPromptRect, 3.f);
    // empty prompt is fine (the model generates unprompted, great with LoRAs) -> greyed placeholder invites it
    const std::string prompt = mPlugin.PromptForMode(mode);
    const bool empty = prompt.empty();
    const std::string shown = empty ? std::string("type a prompt ...or don't. that's cool too") : prompt;
    g.DrawText(IText(13.f, empty ? TextDim() : COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               CompactText(shown, FitChars(mPromptRect.W() - 16.f, 6.0f, 14, 160)).c_str(), mPromptRect.GetPadded(-8.f));
  }

  float SliderFraction(float value, float minValue, float maxValue) const
  {
    return std::clamp((value - minValue) / std::max(0.0001f, maxValue - minValue), 0.f, 1.f);
  }

  void DrawSlider(IGraphics& g, const IRECT& bounds, const char* label, const char* valueText,
                  float value, float minValue, float maxValue, IRECT& sliderRect, bool enabled = true)
  {
    using namespace gary::ui;
    g.DrawText(IText(11.f, enabled ? TextDim() : FrameSoft(), kDemoFont, EAlign::Near, EVAlign::Middle),
               label, IRECT(bounds.L, bounds.T, bounds.L + 86.f, bounds.B));
    g.DrawText(IText(11.f, enabled ? COLOR_WHITE : TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               valueText, IRECT(bounds.R - 58.f, bounds.T, bounds.R, bounds.B));

    const IRECT sr(bounds.L + 92.f, bounds.MH() - 8.f, bounds.R - 66.f, bounds.MH() + 8.f);
    const IRECT track(sr.L, sr.MH() - 2.f, sr.R, sr.MH() + 2.f);
    g.FillRoundRect(FrameSoft(), track, 2.f);
    const float filled = sr.L + sr.W() * SliderFraction(value, minValue, maxValue);
    g.FillRoundRect(enabled ? Red() : FrameSoft(), IRECT(track.L, track.T, filled, track.B), 2.f);
    g.FillCircle(enabled ? COLOR_WHITE : TextDim(), filled, sr.MH(), enabled ? 6.f : 4.f);
    sliderRect = enabled ? sr : IRECT();   // disabled slider is not registered for hit-testing
  }

  void DrawToggle(IGraphics& g, const IRECT& bounds, const char* label, bool on, IRECT& hitRect)
  {
    using namespace gary::ui;
    hitRect = bounds;
    const IRECT box(bounds.L, bounds.MH() - 8.f, bounds.L + 16.f, bounds.MH() + 8.f);
    g.DrawRoundRect(on ? Red() : Frame(), box, 2.f);
    if (on) g.FillRoundRect(Red(), box.GetPadded(-4.f), 1.f);
    g.DrawText(IText(11.f, on ? COLOR_WHITE : TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               label, IRECT(box.R + 7.f, bounds.T, bounds.R, bounds.B));
  }

  void DrawSettings(IGraphics& g, const IRECT& bounds)
  {
    using namespace gary::ui;
    mDecoderToggleRect = mDecoderDownloadRect = mDecoderChooseRect = mDecoderClearRect = {};
    mNormalizeToggleRect = mLimiterToggleRect = {};
    mPeakDbSliderRect = mLimiterCeilingSliderRect = mLimiterKneeSliderRect = {};

    float y = bounds.T + 4.f;
    g.DrawText(IText(18.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               "settings", IRECT(bounds.L, y, bounds.R - 42.f, y + 28.f));
    mSettingsCloseRect = IRECT(bounds.R - 32.f, y + 2.f, bounds.R, y + 26.f);
    DrawButton(g, mSettingsCloseRect, "x", kDemoFont);
    y += 40.f;

    const IRECT decoder(bounds.L, y, bounds.R, y + 176.f);
    g.FillRoundRect(PanelDark(), decoder, 5.f);
    g.DrawRoundRect(FrameSoft(), decoder, 5.f);
    g.DrawText(IText(14.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               "SAME-L decoder correction", IRECT(decoder.L + 12.f, decoder.T + 8.f, decoder.R - 12.f, decoder.T + 30.f));
    g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               "reduces high-frequency buildup in transforms and continuations", IRECT(decoder.L + 12.f, decoder.T + 31.f, decoder.R - 12.f, decoder.T + 50.f));

    const bool installed = mPlugin.DecoderLoraInstalled();
    const bool medium = mPlugin.ModelVariant() == "medium";
    const bool enabled = mPlugin.DecoderLoraEnabled();
    std::string state = !installed ? "not installed"
                       : !enabled ? "installed - disabled"
                       : !medium ? "installed - saved for medium (SAME-L only)"
                                 : "installed - active for the next render";
    const IColor stateColor = mPlugin.DecoderLoraActive() ? Green() : TextDim();
    g.DrawText(IText(11.f, stateColor, kDemoFont, EAlign::Near, EVAlign::Middle),
               state.c_str(), IRECT(decoder.L + 12.f, decoder.T + 56.f, decoder.R - 12.f, decoder.T + 78.f));
    DrawToggle(g, IRECT(decoder.L + 12.f, decoder.T + 82.f, decoder.L + 156.f, decoder.T + 106.f),
               "use correction", enabled, mDecoderToggleRect);

    mDecoderDownloadRect = IRECT(decoder.L + 12.f, decoder.T + 116.f, decoder.L + 132.f, decoder.T + 144.f);
    mDecoderChooseRect = IRECT(mDecoderDownloadRect.R + 8.f, decoder.T + 116.f, mDecoderDownloadRect.R + 112.f, decoder.T + 144.f);
    mDecoderClearRect = IRECT(mDecoderChooseRect.R + 8.f, decoder.T + 116.f, decoder.R - 12.f, decoder.T + 144.f);
    const bool otherDownload = mPlugin.Downloading() && !mPlugin.DecoderLoraDownloading();
    DrawButton(g, mDecoderDownloadRect, mPlugin.DecoderLoraDownloading() ? "cancel" : installed ? "redownload" : "download 11 MB",
               kDemoFont, false, !otherDownload);
    DrawButton(g, mDecoderChooseRect, "choose file", kDemoFont);
    DrawButton(g, mDecoderClearRect, "clear", kDemoFont, false, installed);
    if (otherDownload) mDecoderDownloadRect = {};
    if (!installed) mDecoderClearRect = {};
    const std::string file = installed ? mPlugin.DecoderLoraDisplayName() : "Published adapter: squeakfix_v3";
    g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               CompactText(file, FitChars(decoder.W() - 24.f, 5.4f, 16, 64)).c_str(),
               IRECT(decoder.L + 12.f, decoder.T + 148.f, decoder.R - 12.f, decoder.B - 6.f));
    y = decoder.B + 12.f;

    const IRECT output(bounds.L, y, bounds.R, y + 230.f);
    g.FillRoundRect(PanelDark(), output, 5.f);
    g.DrawRoundRect(FrameSoft(), output, 5.f);
    g.DrawText(IText(14.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               "output processing", IRECT(output.L + 12.f, output.T + 8.f, output.R - 12.f, output.T + 30.f));
    g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               "practical loudness controls applied after decoding", IRECT(output.L + 12.f, output.T + 30.f, output.R - 12.f, output.T + 48.f));
    DrawToggle(g, IRECT(output.L + 12.f, output.T + 54.f, output.R - 12.f, output.T + 78.f),
               "peak normalize", mPlugin.PeakNormalizeEnabled(), mNormalizeToggleRect);
    char value[32] = {};
    std::snprintf(value, sizeof value, "%+.1f dB", mPlugin.PeakNormalizeDb());
    DrawSlider(g, IRECT(output.L + 12.f, output.T + 78.f, output.R - 12.f, output.T + 104.f),
               "target", value, mPlugin.PeakNormalizeDb(), -6.f, 6.f, mPeakDbSliderRect, mPlugin.PeakNormalizeEnabled());
    DrawToggle(g, IRECT(output.L + 12.f, output.T + 110.f, output.R - 12.f, output.T + 134.f),
               "soft limiter", mPlugin.LimiterEnabled(), mLimiterToggleRect);
    std::snprintf(value, sizeof value, "%.1f dB", mPlugin.LimiterCeilingDb());
    DrawSlider(g, IRECT(output.L + 12.f, output.T + 134.f, output.R - 12.f, output.T + 160.f),
               "ceiling", value, mPlugin.LimiterCeilingDb(), -6.f, 0.f, mLimiterCeilingSliderRect, mPlugin.LimiterEnabled());
    std::snprintf(value, sizeof value, "%.2f", mPlugin.LimiterKnee());
    DrawSlider(g, IRECT(output.L + 12.f, output.T + 164.f, output.R - 12.f, output.T + 190.f),
               "knee", value, mPlugin.LimiterKnee(), 0.1f, 1.f, mLimiterKneeSliderRect, mPlugin.LimiterEnabled());
    mOutputDefaultsRect = IRECT(output.L + 12.f, output.B - 32.f, output.L + 132.f, output.B - 8.f);
    mOutputRawRect = IRECT(mOutputDefaultsRect.R + 8.f, output.B - 32.f, mOutputDefaultsRect.R + 96.f, output.B - 8.f);
    DrawButton(g, mOutputDefaultsRect, "tuned defaults", kDemoFont);
    DrawButton(g, mOutputRawRect, "raw", kDemoFont);
    y = output.B + 14.f;

    const bool downloading = mPlugin.Downloading();
    if (downloading)
    {
      const IRECT meter(bounds.L, y, bounds.R, y + 22.f);
      g.FillRoundRect(PanelDark(), meter, 3.f);
      g.FillRoundRect(RedDim(), IRECT(meter.L, meter.T, meter.L + meter.W() * std::clamp(mPlugin.DownloadProgress(), 0.f, 1.f), meter.B), 3.f);
      g.DrawRoundRect(FrameSoft(), meter, 3.f);
      g.DrawText(IText(11.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
                 CompactText(mPlugin.StatusText(), 56).c_str(), meter.GetPadded(-8.f));
      y += 30.f;
    }
    g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               "Changes apply to the next render. Decoder correction is never sent to SAME-S.",
               IRECT(bounds.L, y, bounds.R, y + 24.f));
  }

  float DrawModeControls(IGraphics& g, const IRECT& bounds, SA3IPlug2Demo::RenderMode mode)
  {
    mDurationSliderRect = {};
    mCfgSliderRect = {};
    mNoiseSliderRect = {};
    mSeedToggleRect = {};
    mSeedFieldRect = {};
    mBpmToggleRect = {};
    mBpmValueRect = {};
    mKeyRootRect = {};
    mKeyModeRect = {};
    mDistShiftRect = {};
    mLoopToggleRect = {};
    mBarsRects = {};
    float y = bounds.T;

    char value[32] = {};
    if (mode == SA3IPlug2Demo::RenderMode::Transform)
    {
      std::snprintf(value, sizeof value, "%.2f", mPlugin.InitNoiseLevel());
      DrawSlider(g, IRECT(bounds.L, y, bounds.R, y + 26.f), "init noise", value,
                 mPlugin.InitNoiseLevel(), 0.01f, 1.0f, mNoiseSliderRect);
      y += 32.f;
    }
    else
    {
      const bool loopLocks = (mode == SA3IPlug2Demo::RenderMode::Text && mPlugin.LoopBars() > 0);
      std::snprintf(value, sizeof value, "%ds", mPlugin.DurationSeconds());
      DrawSlider(g, IRECT(bounds.L, y, bounds.R, y + 26.f),
                 mode == SA3IPlug2Demo::RenderMode::Continue ? "total" : "duration",
                 loopLocks ? "loop" : value, (float)mPlugin.DurationSeconds(), 1.f, 300.f, mDurationSliderRect, !loopLocks);
      y += 32.f;
    }

    std::snprintf(value, sizeof value, "%d", mPlugin.Steps());
    DrawSlider(g, IRECT(bounds.L, y, bounds.R, y + 26.f), "steps", value,
               (float)mPlugin.Steps(), 1.f, 16.f, mStepsSliderRect);
    y += 32.f;

    std::snprintf(value, sizeof value, "%.1f", mPlugin.CfgScale());
    DrawSlider(g, IRECT(bounds.L, y, bounds.R, y + 26.f), "cfg", value,
               mPlugin.CfgScale(), 0.5f, 2.0f, mCfgSliderRect);   // 1.0 = off (single pass)
    y += 32.f;

    {   // distribution shift (sampler schedule warp; default LogSNR) — a libsa3 primitive dropdown
      const IRECT row(bounds.L, y, bounds.R, y + 26.f);
      g.DrawText(IText(11.f, gary::ui::TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
                 "shift", IRECT(row.L, row.T, row.L + 86.f, row.B));
      mDistShiftRect = IRECT(row.L + 92.f, row.T + 1.f, row.L + 200.f, row.B - 1.f);
      DrawDropButton(g, mDistShiftRect, mPlugin.DistShiftName());
    }
    y += 32.f;

    DrawSeedControls(g, IRECT(bounds.L, y, bounds.R, y + 26.f));
    y += 32.f;

    DrawMusicalControls(g, IRECT(bounds.L, y, bounds.R, y + 26.f));
    y += 32.f;

    DrawLoopControls(g, IRECT(bounds.L, y, bounds.R, y + 26.f), mode);
    return y + 26.f;
  }

  void DrawSeedControls(IGraphics& g, const IRECT& bounds)
  {
    using namespace gary::ui;
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               "seed", IRECT(bounds.L, bounds.T, bounds.L + 86.f, bounds.B));

    mSeedToggleRect = IRECT(bounds.L + 92.f, bounds.MH() - 8.f, bounds.L + 108.f, bounds.MH() + 8.f);
    g.DrawRoundRect(mPlugin.UseSeed() ? Red() : Frame(), mSeedToggleRect, 2.f);
    if (mPlugin.UseSeed())
      g.FillRoundRect(Red(), mSeedToggleRect.GetPadded(-4.f), 1.f);
    g.DrawText(IText(11.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               "use", IRECT(mSeedToggleRect.R + 6.f, bounds.T, mSeedToggleRect.R + 40.f, bounds.B));

    // The field shows: the seed in use (white) when locked; otherwise the last generated seed greyed out
    // (hit "use" to reuse+lock it), or "random" before the first generation. One display, no duplication.
    mSeedFieldRect = IRECT(bounds.L + 142.f, bounds.T + 1.f, bounds.R, bounds.B - 1.f);
    g.FillRoundRect(ButtonFill(), mSeedFieldRect, 3.f);
    g.DrawRoundRect(Frame(), mSeedFieldRect, 3.f);
    const bool useSeed = mPlugin.UseSeed();
    const std::string seedText = useSeed ? std::to_string(mPlugin.SeedValue())
                               : mPlugin.HasLastSeed() ? std::to_string(mPlugin.LastSeed())
                                                       : std::string("random");
    g.DrawText(IText(11.f, useSeed ? COLOR_WHITE : TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               CompactText(seedText, FitChars(mSeedFieldRect.W() - 12.f, 6.f, 4, 40)).c_str(),
               mSeedFieldRect.GetPadded(-6.f));
  }

  void DrawDropButton(IGraphics& g, const IRECT& bounds, const char* text)
  {
    using namespace gary::ui;
    g.FillRoundRect(ButtonFill(), bounds, 3.f);
    g.DrawRoundRect(Frame(), bounds, 3.f);
    g.DrawText(IText(11.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               text, IRECT(bounds.L + 8.f, bounds.T, bounds.R - 14.f, bounds.B));
    const float cx = bounds.R - 10.f, cy = bounds.MH();
    g.FillTriangle(TextDim(), cx - 4.f, cy - 2.f, cx + 4.f, cy - 2.f, cx, cy + 3.f);
  }

  void DrawMusicalControls(IGraphics& g, const IRECT& bounds)
  {
    using namespace gary::ui;
    // bpm-append toggle + live host tempo
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               "bpm", IRECT(bounds.L, bounds.T, bounds.L + 30.f, bounds.B));
    mBpmToggleRect = IRECT(bounds.L + 34.f, bounds.MH() - 8.f, bounds.L + 50.f, bounds.MH() + 8.f);
    const bool bpmOn = mPlugin.AppendBpm();
    g.DrawRoundRect(bpmOn ? Red() : Frame(), mBpmToggleRect, 2.f);
    if (bpmOn)
      g.FillRoundRect(Red(), mBpmToggleRect.GetPadded(-4.f), 1.f);
    // draggable bpm value (vertical drag adjusts; follows host tempo until dragged)
    mBpmValueRect = IRECT(mBpmToggleRect.R + 6.f, bounds.T + 2.f, bounds.L + 116.f, bounds.B - 2.f);
    g.FillRoundRect(ButtonFill(), mBpmValueRect, 3.f);
    g.DrawRoundRect(mPlugin.BpmOverridden() ? Frame() : FrameSoft(), mBpmValueRect, 3.f);
    char bpmText[24] = {};
    std::snprintf(bpmText, sizeof bpmText, "%d", (int)std::llround(mPlugin.Bpm()));
    g.DrawText(IText(12.f, bpmOn ? COLOR_WHITE : TextDim(), kDemoFont, EAlign::Center, EVAlign::Middle),
               bpmText, mBpmValueRect);

    // key root (dropdown) + major/minor (click toggle)
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               "key", IRECT(bounds.L + 118.f, bounds.T, bounds.R - 150.f, bounds.B));
    mKeyRootRect = IRECT(bounds.R - 144.f, bounds.T + 1.f, bounds.R - 78.f, bounds.B - 1.f);
    mKeyModeRect = {};
    static const char* kRoots[13] = {"none", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int root = std::clamp(mPlugin.KeyRoot(), 0, 12);
    DrawDropButton(g, mKeyRootRect, kRoots[root]);
    // major/minor only appears once a root is chosen (nothing is appended when key = none)
    if (root > 0)
    {
      mKeyModeRect = IRECT(bounds.R - 74.f, bounds.T + 1.f, bounds.R, bounds.B - 1.f);
      DrawDropButton(g, mKeyModeRect, mPlugin.KeyMode() ? "minor" : "major");
    }
  }

  void DrawLoopControls(IGraphics& g, const IRECT& bounds, SA3IPlug2Demo::RenderMode mode)
  {
    using namespace gary::ui;
    mLoopToggleRect = {};
    mBarsRects = {};
    if (mode != SA3IPlug2Demo::RenderMode::Text)
    {
      g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
                 "loops available in generate mode", IRECT(bounds.L, bounds.T, bounds.R, bounds.B));
      return;
    }
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
               "loop", IRECT(bounds.L, bounds.T, bounds.L + 40.f, bounds.B));
    const int bars = mPlugin.LoopBars();
    const bool on = bars > 0;

    mLoopToggleRect = IRECT(bounds.L + 44.f, bounds.T + 1.f, bounds.L + 84.f, bounds.B - 1.f);
    g.FillRoundRect(on ? Red() : ButtonFill(), mLoopToggleRect, 3.f);
    g.DrawRoundRect(on ? Red() : Frame(), mLoopToggleRect, 3.f);
    g.DrawText(IText(11.f, on ? COLOR_BLACK : COLOR_WHITE, kDemoFont, EAlign::Center, EVAlign::Middle),
               on ? "on" : "off", mLoopToggleRect);

    if (!on)
    {
      g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
                 "generate an exact bar length", IRECT(mLoopToggleRect.R + 10.f, bounds.T, bounds.R, bounds.B));
      return;
    }

    const int values[3] = {4, 8, 16};
    const char* labels[3] = {"4", "8", "16"};
    float bx = mLoopToggleRect.R + 8.f;
    for (int i = 0; i < 3; ++i)
    {
      IRECT r(bx, bounds.T + 1.f, bx + 36.f, bounds.B - 1.f);
      mBarsRects[(size_t)i] = r;
      const bool active = bars == values[i];
      g.FillRoundRect(active ? Red() : ButtonFill(), r, 3.f);
      g.DrawRoundRect(active ? Red() : Frame(), r, 3.f);
      g.DrawText(IText(11.f, active ? COLOR_BLACK : COLOR_WHITE, kDemoFont, EAlign::Center, EVAlign::Middle), labels[i], r);
      bx += 42.f;
    }
    const double bpm = mPlugin.Bpm();
    char hint[48] = {};
    if (bpm > 0.0)
      std::snprintf(hint, sizeof hint, "%.1fs", (60.0 / bpm) * 4.0 * bars);
    else
      std::snprintf(hint, sizeof hint, "need bpm");
    g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               hint, IRECT(bx + 4.f, bounds.T, bounds.R, bounds.B));
  }

  float DrawLoraPanel(IGraphics& g, const IRECT& bounds)
  {
    using namespace gary::ui;
    mAddLoraRect = {};
    mLoraToggleRects.clear();
    mLoraRemoveRects.clear();
    mLoraSliderRects.clear();

    const std::vector<SA3IPlug2Demo::LoraSlot> loras = mPlugin.Loras();
    g.FillRoundRect(PanelDark(), bounds, 4.f);
    g.DrawRoundRect(FrameSoft(), bounds, 4.f);

    const float left = bounds.L + 10.f;
    const float right = bounds.R - 10.f;
    float y = bounds.T + 8.f;
    g.DrawText(IText(12.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               "loras", IRECT(left, y, left + 80.f, y + 20.f));
    mAddLoraRect = IRECT(right - 86.f, y, right, y + 22.f);
    DrawButton(g, mAddLoraRect, "add lora", kDemoFont);
    y += 28.f;

    if (loras.empty())
    {
      g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
                 "no loras imported", IRECT(left, y, right, y + 22.f));
      return bounds.B;
    }

    const size_t rowCount = std::min<size_t>(loras.size(), 2);
    for (size_t i = 0; i < rowCount; ++i)
    {
      const auto& lora = loras[i];
      IRECT row(left, y, right, y + 22.f);
      IRECT toggle(row.L, row.T + 2.f, row.L + 18.f, row.B - 2.f);
      IRECT remove(row.R - 24.f, row.T, row.R, row.B);
      IRECT slider(row.R - 154.f, row.T + 3.f, row.R - 34.f, row.B - 3.f);
      IRECT label(toggle.R + 6.f, row.T, slider.L - 8.f, row.B);

      mLoraToggleRects.push_back(toggle);
      mLoraRemoveRects.push_back(remove);
      mLoraSliderRects.push_back(slider);

      g.DrawRoundRect(lora.enabled ? Red() : Frame(), toggle, 2.f);
      if (lora.enabled)
        g.FillRoundRect(Red(), toggle.GetPadded(-4.f), 1.f);
      std::string labelText = lora.name;
      if (!lora.prompts.empty())
        labelText += " (" + std::to_string(lora.prompts.size()) + ")";
      g.DrawText(IText(11.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
                 CompactText(labelText, FitChars(label.W(), 6.f, 10, 42)).c_str(), label);

      const IRECT track(slider.L, slider.MH() - 2.f, slider.R, slider.MH() + 2.f);
      const float filled = slider.L + slider.W() * SliderFraction(lora.strength, 0.f, 2.f);
      g.FillRoundRect(FrameSoft(), track, 2.f);
      g.FillRoundRect(Red(), IRECT(track.L, track.T, filled, track.B), 2.f);
      g.FillCircle(COLOR_WHITE, filled, slider.MH(), 5.f);

      char value[24] = {};
      std::snprintf(value, sizeof value, "%.2f", lora.strength);
      g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Center, EVAlign::Middle), value, IRECT(slider.L, row.B - 2.f, slider.R, row.B + 12.f));
      DrawButton(g, remove, "x", kDemoFont);
      y += 26.f;
    }

    if (loras.size() > rowCount)
      g.DrawText(IText(10.f, TextDim(), kDemoFont, EAlign::Near, EVAlign::Middle),
                 "+ more loras queued", IRECT(left, y, right, y + 14.f));

    return bounds.B;
  }

  void UpdateSliderFromX(float x)
  {
    auto fraction = [](const IRECT& r, float px) {
      return std::clamp((px - r.L) / std::max(1.f, r.W()), 0.f, 1.f);
    };

    switch (mActiveSlider)
    {
      case Slider::Duration:
        mPlugin.SetDurationSeconds((int)std::llround(1.f + fraction(mDurationSliderRect, x) * 299.f));
        break;
      case Slider::Steps:
        mPlugin.SetSteps((int)std::llround(1.f + fraction(mStepsSliderRect, x) * 15.f));
        break;
      case Slider::Cfg:
        mPlugin.SetCfgScale(0.5f + fraction(mCfgSliderRect, x) * 1.5f);
        break;
      case Slider::Noise:
        mPlugin.SetInitNoiseLevel(0.01f + fraction(mNoiseSliderRect, x) * 0.99f);
        break;
      case Slider::Lora:
        if (mActiveLoraIndex < mLoraSliderRects.size())
          mPlugin.SetLoraStrength(mActiveLoraIndex, fraction(mLoraSliderRects[mActiveLoraIndex], x) * 2.f);
        break;
      case Slider::PeakDb:
        mPlugin.SetPeakNormalizeDb(-6.f + fraction(mPeakDbSliderRect, x) * 12.f);
        break;
      case Slider::LimiterCeiling:
        mPlugin.SetLimiterCeilingDb(-6.f + fraction(mLimiterCeilingSliderRect, x) * 6.f);
        break;
      case Slider::LimiterKnee:
        mPlugin.SetLimiterKnee(0.1f + fraction(mLimiterKneeSliderRect, x) * 0.9f);
        break;
      case Slider::Bpm:   // handled by vertical drag in OnMouseDrag
      case Slider::None:
        break;
    }
    SetDirty(false);
  }

  void ToggleLora(size_t index)
  {
    const auto loras = mPlugin.Loras();
    if (index < loras.size())
      mPlugin.SetLoraEnabled(index, !loras[index].enabled);
  }

  void OpenKeyRootMenu()
  {
    if (!GetUI())
      return;
    static const char* kRoots[13] = {"none", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    mKeyMenu.Clear();
    for (int i = 0; i < 13; ++i)
      mKeyMenu.AddItem(kRoots[i]);
    mKeyMenu.CheckItem(std::clamp(mPlugin.KeyRoot(), 0, 12), true);
    mActivePopup = Popup::KeyRoot;
    GetUI()->CreatePopupMenu(*this, mKeyMenu, mKeyRootRect);
  }

  void OpenKeyModeMenu()
  {
    if (!GetUI())
      return;
    mKeyMenu.Clear();
    mKeyMenu.AddItem("major");
    mKeyMenu.AddItem("minor");
    mKeyMenu.CheckItem(mPlugin.KeyMode() ? 1 : 0, true);
    mActivePopup = Popup::KeyMode;
    GetUI()->CreatePopupMenu(*this, mKeyMenu, mKeyModeRect);
  }

  void OpenDistShiftMenu()
  {
    if (!GetUI())
      return;
    static const char* kNames[4] = {"LogSNR", "Flux", "Full", "None"};
    mKeyMenu.Clear();
    for (int i = 0; i < 4; ++i)
      mKeyMenu.AddItem(kNames[i]);
    mKeyMenu.CheckItem(std::clamp(mPlugin.DistShift(), 0, 3), true);
    mActivePopup = Popup::DistShift;
    GetUI()->CreatePopupMenu(*this, mKeyMenu, mDistShiftRect);
  }

  // Action codes for the dynamic models menu (see mModelsActions).
  enum ModelsAction { kCancelDl = 9, kUseMedium = 10, kUseSmall = 11, kDlMedium = 0, kDlSmall = 1, kPoint = 2, kSep = -1 };

  void OpenModelsMenu()
  {
    if (!GetUI())
      return;
    mKeyMenu.Clear();
    mModelsActions.clear();
    auto add = [&](const char* label, int action, bool checked = false) {
      mKeyMenu.AddItem(label);
      if (checked)
        mKeyMenu.CheckItem(mKeyMenu.NItems() - 1, true);
      mModelsActions.push_back(action);
    };

    if (mPlugin.Downloading())   // while a download runs the menu is just a cancel affordance
    {
      add("cancel download", kCancelDl);
      mActivePopup = Popup::Models;
      GetUI()->CreatePopupMenu(*this, mKeyMenu, mModelsBtnRect);
      return;
    }

    // Radio row: whichever variants are actually present in the current folder can be selected for generation.
    const std::string active = mPlugin.ModelVariant();
    const bool mediumHere = mPlugin.VariantAvailable("medium");
    const bool smallHere = mPlugin.VariantAvailable("small-music");
    if (mediumHere)
      add("use medium", kUseMedium, active == "medium");
    if (smallHere)
      add("use small-music", kUseSmall, active == "small-music");
    if (mediumHere || smallHere)
    {
      mKeyMenu.AddSeparator();
      mModelsActions.push_back(kSep);
    }

    add("download medium (~5.7 GB)", kDlMedium);
    add("download small-music (~2.3 GB)", kDlSmall);
    add("point at an existing folder…", kPoint);
    mActivePopup = Popup::Models;
    GetUI()->CreatePopupMenu(*this, mKeyMenu, mModelsBtnRect);
  }

  // Open the OS directory picker (cross-platform via IGraphics) seeded at `seed`; returns "" if cancelled.
  std::string PickDirectory(const std::string& seed)
  {
    if (!GetUI())
      return {};
    WDL_String dir;
    if (!seed.empty())
      dir.Set(seed.c_str());
    GetUI()->PromptForDirectory(dir);
    return dir.GetLength() > 0 ? std::string(dir.Get()) : std::string();
  }

  void StartDownloadFlow(int variantIdx)
  {
    const std::string dest = PickDirectory(gary::DefaultModelsDirectory());
    if (!dest.empty())
      mPlugin.StartModelDownload(variantIdx, dest);
    SetDirty(false);
  }

  void PointAtFolderFlow()
  {
    const std::string dir = PickDirectory(mPlugin.ModelsPresent() ? mPlugin.ModelsDir() : gary::DefaultModelsDirectory());
    if (dir.empty())
      return;
    // Accept whichever complete variant the folder actually holds; else report medium's missing list.
    std::vector<std::string> missing;
    if (gary::ModelSetComplete(dir, "medium", "f16", missing))
      mPlugin.UseModelsFolder(dir, "medium", true);
    else if (gary::ModelSetComplete(dir, "small-music", "f16", missing))
      mPlugin.UseModelsFolder(dir, "small-music", true);
    else
      mPlugin.UseModelsFolder(dir, "medium", true);
    SetDirty(false);
  }

  void DrawWaveform(IGraphics& g, const IRECT& bounds, const SA3IPlug2Demo::Waveform& wf,
                    const IColor& brightColor, const IColor& dimColor)
  {
    g.FillRoundRect(gary::ui::PanelDark(), bounds, 4.f);
    g.DrawRoundRect(gary::ui::FrameSoft(), bounds, 4.f);
    if (wf.peaks.empty() || wf.numSamples <= 0)
      return;

    const float mid = bounds.MH();
    const float half = bounds.H() * 0.43f;
    const int n = static_cast<int>(wf.peaks.size());
    // buckets up to the frozen "save buffer" point draw bright, the grown-past-save remainder draws dim
    const int savedBuckets = (int)std::ceil((double)wf.savedSamples / std::max(1, wf.numSamples) * n);
    for (int i = 0; i < n; ++i)
    {
      const float x = bounds.L + (i + 0.5f) * bounds.W() / std::max(1, n);
      const auto& p = wf.peaks[(size_t)i];
      const IColor& c = (i < savedBuckets) ? brightColor : dimColor;
      g.DrawLine(c, x, mid - p.maxValue * half, x, mid - p.minValue * half, nullptr, 1.f);
    }

    if (wf.playheadSamples > 0)
    {
      const float px = bounds.L + bounds.W() * (float)wf.playheadSamples / std::max(1, wf.numSamples);
      g.DrawLine(COLOR_WHITE, px, bounds.T, px, bounds.B, nullptr, 1.5f);
    }
  }

  void DrawWaveformPanel(IGraphics& g, const IRECT& bounds, const char* title, const std::string& status,
                         const SA3IPlug2Demo::Waveform& wf, bool output)
  {
    using namespace gary::ui;
    g.FillRoundRect(Panel(), bounds, 5.f);
    g.DrawRoundRect(Frame(), bounds, 5.f);
    g.DrawText(IText(14.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               title, IRECT(bounds.L + 12.f, bounds.T + 8.f, bounds.MW(), bounds.T + 28.f));
    // "save buffer" lives with the source panel (saves the recording buffer to myBuffer.wav); the output
    // panel has no manual save button — output auto-saves to myOutput.wav and drags out with its own name.
    float detailR = bounds.R - 12.f;
    if (!output)
    {
      mSaveBufferRect = IRECT(bounds.R - 96.f, bounds.T + 6.f, bounds.R - 10.f, bounds.T + 26.f);
      DrawButton(g, mSaveBufferRect, "save buffer", kDemoFont);
      detailR = mSaveBufferRect.L - 8.f;
    }
    const double seconds = wf.numSamples > 0 ? (double)wf.numSamples / std::max(1, wf.sampleRate) : 0.0;
    char detail[160] = {};
    std::snprintf(detail, sizeof(detail), "%.2fs @ %d Hz", seconds, std::max(1, wf.sampleRate));
    const IRECT detailRect(bounds.MW(), bounds.T + 8.f, detailR, bounds.T + 28.f);
    const std::string detailText = wf.numSamples > 0
                                 ? std::string(detail)
                                 : CompactText(status, FitChars(detailRect.W(), 6.f, 10, 64));
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               detailText.c_str(), detailRect);

    const IRECT wfRect(bounds.L + 12.f, bounds.T + 34.f, bounds.R - 12.f, bounds.B - (output ? 48.f : 36.f));
    if (output) mOutputWaveformRect = wfRect;
    // source: bright up to the saved point, dim past it; output: uniform bright
    DrawWaveform(g, wfRect, wf, Red(), output ? Red() : RedDim());
    if (!output)
      g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Center, EVAlign::Middle),
                 CompactText(status, FitChars(bounds.W() - 24.f, 6.f, 16, 72)).c_str(),
                 IRECT(bounds.L + 12.f, bounds.B - 28.f, bounds.R - 12.f, bounds.B - 10.f));
  }

  void DrawOutputPanel(IGraphics& g, const IRECT& bounds)
  {
    using namespace gary::ui;
    DrawWaveformPanel(g, bounds, "output", mPlugin.OutputStatusText(), mPlugin.OutputWaveform((int)bounds.W()), true);
    const float y = bounds.B - 40.f;
    mOutputPlayRect = IRECT(bounds.L + 12.f, y, bounds.L + 48.f, y + 30.f);
    mOutputStopRect = IRECT(mOutputPlayRect.R + 8.f, y, mOutputPlayRect.R + 44.f, y + 30.f);
    DrawIconButton(g, mOutputPlayRect, mPlugin.OutputPlaying() ? TransportIcon::Pause : TransportIcon::Play);
    DrawIconButton(g, mOutputStopRect, TransportIcon::Stop);
    // no manual output-save button: output auto-saves to myOutput.wav after each render + drags out separately
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               CompactText(mPlugin.OutputStatusText(), FitChars(bounds.R - mOutputStopRect.R - 24.f, 6.f, 8, 72)).c_str(),
               IRECT(mOutputStopRect.R + 12.f, y, bounds.R - 12.f, y + 30.f));
  }

  SA3IPlug2Demo& mPlugin;
  IRECT mPromptRect;
  IRECT mDiceRect;
  IRECT mStatusRect, mStatusCopyRect;
  IRECT mModelsBtnRect, mSettingsBtnRect, mSettingsCloseRect;
  IRECT mDecoderToggleRect, mDecoderDownloadRect, mDecoderChooseRect, mDecoderClearRect;
  IRECT mNormalizeToggleRect, mLimiterToggleRect;
  IRECT mPeakDbSliderRect, mLimiterCeilingSliderRect, mLimiterKneeSliderRect;
  IRECT mOutputDefaultsRect, mOutputRawRect;
  IRECT mGenerateTabRect, mTransformTabRect, mContinueTabRect;
  IRECT mDurationSliderRect, mStepsSliderRect, mCfgSliderRect, mNoiseSliderRect;
  IRECT mSeedToggleRect, mSeedFieldRect;
  IRECT mBpmToggleRect, mBpmValueRect, mKeyRootRect, mKeyModeRect;
  IRECT mDistShiftRect;
  IRECT mLoopToggleRect;
  std::array<IRECT, 4> mBarsRects{};   // 4, 8, 16 (index 3 unused)
  IRECT mRunRect, mAddLoraRect;
  std::vector<IRECT> mLoraToggleRects;
  std::vector<IRECT> mLoraRemoveRects;
  std::vector<IRECT> mLoraSliderRects;
  IRECT mSaveBufferRect;
  IRECT mOutputWaveformRect, mOutputPlayRect, mOutputStopRect;
  Slider mActiveSlider = Slider::None;
  enum class Popup { None, KeyRoot, KeyMode, DistShift, Models } mActivePopup = Popup::None;
  IPopupMenu mKeyMenu;
  // One action code per item in the (dynamic) models menu, index-aligned with the menu items so a chosen
  // index maps straight to an action. See ModelsAction.
  std::vector<int> mModelsActions;
  EditTarget mEditTarget = EditTarget::None;
  SA3IPlug2Demo::RenderMode mEditPromptMode = SA3IPlug2Demo::RenderMode::Text;
  size_t mActiveLoraIndex = 0;
  bool mOutputPointerDown = false;
  bool mOutputDragStarted = false;
  float mOutputDragStartX = 0.f;
  float mOutputDragStartY = 0.f;
  bool mSettingsOpen = false;
  bool mStatusHovered = false;
  bool mStatusTruncated = false;
  bool mCopyHovered = false;
  std::chrono::steady_clock::time_point mCopyFlashUntil{};
};
} // namespace

SA3IPlug2Demo::SA3IPlug2Demo(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  GetParam(kStatusParam)->InitDouble("Status", 0., 0., 1., 1., "");
  mDurationSeconds[0].store(30, std::memory_order_release);    // generate
  mDurationSeconds[1].store(12, std::memory_order_release);    // transform (duration unused; noise slider instead)
  mDurationSeconds[2].store(120, std::memory_order_release);   // continue (total length incl. source)
  ResizeRecordBuffer(44100.0);
  mHostSampleRate.store(44100, std::memory_order_release);
  LoadPersistedBuffer();   // restore myBuffer.wav (frozen init snapshot) from a previous session

  // Restore the persisted models dir + variant (settings.txt), then scan for the file set. Precedence is
  // persisted setting -> SA3_MODELS_DIR env -> compile default; see ModelsDir().
  {
    const std::string savedDir = gary::LoadSetting("models_dir");
    const std::string savedVariant = gary::LoadSetting("variant");
    std::lock_guard<std::mutex> lock(mModelsMutex);
    if (!savedDir.empty())
      mModelsDirSetting = savedDir;
    if (savedVariant == "small-music" || savedVariant == "small-sfx")
      mModelVariant = "small-music";
    else if (savedVariant == "medium")
      mModelVariant = "medium";
  }
  RefreshModelsPresent();

  // Restore the compact Settings panel. Missing values deliberately resolve to the documented libsa3
  // defaults; a selected decoder adapter is remembered even while SAME-S is active.
  {
    std::lock_guard<std::mutex> lock(mDecoderLoraMutex);
    mDecoderLoraPath = gary::LoadSetting("decoder_lora_same_l_path");
  }
  mDecoderLoraEnabled.store(ParseBoolSetting(gary::LoadSetting("decoder_lora_same_l_enabled"), true), std::memory_order_release);
  mPeakNormalizeEnabled.store(ParseBoolSetting(gary::LoadSetting("peak_normalize_enabled"), true), std::memory_order_release);
  mPeakNormalizeDb.store(ParseFloatSetting(gary::LoadSetting("peak_normalize_db"), 2.0f, -6.f, 6.f), std::memory_order_release);
  mLimiterEnabled.store(ParseBoolSetting(gary::LoadSetting("limiter_enabled"), true), std::memory_order_release);
  mLimiterCeilingDb.store(ParseFloatSetting(gary::LoadSetting("limiter_ceiling_db"), -0.3f, -6.f, 0.f), std::memory_order_release);
  mLimiterKnee.store(ParseFloatSetting(gary::LoadSetting("limiter_knee"), 0.8f, 0.1f, 1.f), std::memory_order_release);
  LoadPersistedCreativeLoras();

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachPanelBackground(COLOR_BLACK);
    pGraphics->EnableMouseOver(true);
    pGraphics->AttachTextEntryControl();
    if (!pGraphics->LoadFont(kDemoFont, ROBOTO_FN))
      pGraphics->LoadFont(kDemoFont, "Arial", ETextStyle::Normal);
    pGraphics->AttachControl(new SA3DemoControl(pGraphics->GetBounds(), *this), kCtrlTagMain);
  };
#endif
}

int SA3IPlug2Demo::ModeIndex(RenderMode mode) noexcept
{
  switch (mode)
  {
    case RenderMode::Text: return 0;
    case RenderMode::Transform: return 1;
    case RenderMode::Continue: return 2;
  }
  return 0;
}

SA3IPlug2Demo::~SA3IPlug2Demo()
{
  if (mCreativeLorasDirty.load(std::memory_order_acquire))
    PersistCreativeLoras();
  StopWorker();
  StopDownloadWorker();
  TeardownContext();
}

#if IPLUG_DSP
void SA3IPlug2Demo::OnActivate(bool active)
{
  if (!active)
    CancelRender();
}

void SA3IPlug2Demo::OnReset()
{
  const int hostRate = std::max(1, (int)(GetSampleRate() + 0.5));
  mHostSampleRate.store(hostRate, std::memory_order_release);
  ResizeRecordBuffer(hostRate);
  {
    std::lock_guard<std::mutex> lock(mOutputMutex);
    RebuildOutputPlaybackBufferFromNativeLocked(hostRate);
  }
  mWasTransportRunning = false;
}

void SA3IPlug2Demo::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  const int nInChans = NInChansConnected();
  const int nOutChans = NOutChansConnected();
  const bool running = GetTransportIsRunning();
  mTransportRunning.store(running, std::memory_order_release);
  const double tempo = GetTempo();
  if (tempo > 0.0)
  {
    mHostTempo.store(tempo, std::memory_order_release);
    if (!mBpmOverride.load(std::memory_order_acquire))   // DAW: follow host tempo unless user dragged it
      mBpm.store(tempo, std::memory_order_release);
  }

  if (running && !mWasTransportRunning)
    StartAutoRecording();
  else if (!running && mWasTransportRunning)
    StopAutoRecording();
  mWasTransportRunning = running;

  if (running)
    CopyInputToRecordBuffer(inputs, nInChans, nFrames);

  for (int c = 0; c < nOutChans; ++c)
  {
    if (!outputs || !outputs[c])
      continue;
    const int sourceChannel = nInChans > 0 ? c % nInChans : 0;
    const sample* in = (inputs && nInChans > 0 && inputs[sourceChannel]) ? inputs[sourceChannel] : nullptr;
    for (int s = 0; s < nFrames; ++s)
      outputs[c][s] = in ? in[s] : 0.0;
  }

  MixOutputPlayback(outputs, nOutChans, nFrames);
}
#endif

#if IPLUG_EDITOR
void SA3IPlug2Demo::OnUIClose()
{
  if (mCreativeLorasDirty.load(std::memory_order_acquire))
    PersistCreativeLoras();
  CancelRender();
}

void SA3IPlug2Demo::OnIdle()
{
  if (auto* ui = GetUI())
    if (auto* control = ui->GetControlWithTag(kCtrlTagMain))
      control->SetDirty(false);
}
#endif

void SA3IPlug2Demo::SetPrompt(const char* text)
{
  SetPromptForMode(CurrentRenderMode(), text);
}

void SA3IPlug2Demo::SetPromptForMode(RenderMode mode, const char* text)
{
  std::lock_guard<std::mutex> lock(mPromptMutex);
  mPrompts[(size_t)ModeIndex(mode)] = text && *text ? text : "";
}

void SA3IPlug2Demo::RollPromptForCurrentMode()
{
  std::vector<std::string> pool;
  auto addUnique = [&pool](const std::string& prompt) {
    if (!prompt.empty() && std::find(pool.begin(), pool.end(), prompt) == pool.end())
      pool.push_back(prompt);
  };

  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    for (const auto& lora : mLoras)
    {
      if (!lora.enabled || lora.strength <= 0.0f)
        continue;
      for (const std::string& prompt : lora.prompts)
        addUnique(prompt);
    }
  }

  const bool usingLoraPrompts = !pool.empty();
  if (pool.empty())
  {
    for (const std::string& prompt : gary::LoadDefaultPromptPool())
      addUnique(prompt);
  }

  if (pool.empty())
  {
    SetStatus("prompt pool empty");
    return;
  }

  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<size_t> dist(0, pool.size() - 1u);
  SetPromptForMode(CurrentRenderMode(), pool[dist(rng)].c_str());
  SetStatus(usingLoraPrompts ? "rolled LoRA prompt" : "rolled default prompt");
}

std::string SA3IPlug2Demo::Prompt() const
{
  return PromptForMode(CurrentRenderMode());
}

std::string SA3IPlug2Demo::PromptForMode(RenderMode mode) const
{
  std::lock_guard<std::mutex> lock(mPromptMutex);
  return mPrompts[(size_t)ModeIndex(mode)];
}

std::string SA3IPlug2Demo::KeyScaleText() const
{
  static const char* kKeyNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int root = KeyRoot();
  if (root < 1 || root > 12)
    return {};
  return std::string(kKeyNames[root - 1]) + (KeyMode() ? " minor" : " major");
}

const char* SA3IPlug2Demo::DistShiftName() const
{
  static const char* kNames[4] = {"LogSNR", "Flux", "Full", "None"};
  return kNames[std::clamp(DistShift(), 0, 3)];
}

std::string SA3IPlug2Demo::StatusText() const
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  return mStatus;
}

std::string SA3IPlug2Demo::SourceStatusText() const
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  return mSourceStatus;
}

std::string SA3IPlug2Demo::OutputStatusText() const
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  return mOutputStatus;
}

std::string SA3IPlug2Demo::ModelsDir() const
{
  {
    std::lock_guard<std::mutex> lock(mModelsMutex);
    if (!mModelsDirSetting.empty())
      return mModelsDirSetting;
  }
  if (const char* e = std::getenv("SA3_MODELS_DIR"))
    if (*e) return e;
  return SA3_DEMO_DEFAULT_MODELS_DIR;
}

std::string SA3IPlug2Demo::ModelVariant() const
{
  std::lock_guard<std::mutex> lock(mModelsMutex);
  return mModelVariant;
}

int SA3IPlug2Demo::ModelVariantIndex() const
{
  return ModelVariant() == "small-music" ? 1 : 0;
}

void SA3IPlug2Demo::RefreshModelsPresent()
{
  const std::string dir = ModelsDir();
  const std::string variant = ModelVariant();
  std::vector<std::string> missing;
  const bool present = gary::ModelSetComplete(dir, variant, "f16", missing);
  mModelsPresent.store(present, std::memory_order_release);
}

void SA3IPlug2Demo::TeardownContext()
{
  if (mContext)
  {
    if (const Sa3Api* sa3 = LoadedSa3Api())
      sa3->freeContext(mContext);
    mContext = nullptr;
  }
}

bool SA3IPlug2Demo::UseModelsFolder(const std::string& dir, const std::string& variant, bool persist)
{
  const std::string v = (variant == "small-music" || variant == "small-sfx") ? "small-music" : "medium";
  std::vector<std::string> missing;
  if (!gary::ModelSetComplete(dir, v, "f16", missing))
  {
    std::string msg = "that folder is missing: ";
    for (size_t i = 0; i < missing.size(); ++i)
      msg += (i ? ", " : "") + missing[i];
    SetStatus(msg);
    return false;
  }

  const bool variantChanged = ModelVariant() != v;
  if (variantChanged && mCreativeLorasDirty.load(std::memory_order_acquire))
    PersistCreativeLoras();

  {
    std::lock_guard<std::mutex> lock(mModelsMutex);
    mModelsDirSetting = dir;
    mModelVariant = v;
  }
  // A previously loaded context may be a different variant/dir — drop it so the next render reloads.
  // Safe here: this is called from the UI thread and generation is gated off while no models were present.
  if (!mBusy.load(std::memory_order_acquire))
    TeardownContext();
  mModelsPresent.store(true, std::memory_order_release);
  if (persist)
  {
    gary::SaveSetting("models_dir", dir);
    gary::SaveSetting("variant", v);
  }
  if (variantChanged)
    LoadPersistedCreativeLoras();
  SetStatus("models ready (" + v + "): " + dir);
  return true;
}

bool SA3IPlug2Demo::VariantAvailable(const std::string& variant) const
{
  const std::string v = (variant == "small-music" || variant == "small-sfx") ? "small-music" : "medium";
  std::vector<std::string> missing;
  return gary::ModelSetComplete(ModelsDir(), v, "f16", missing);
}

bool SA3IPlug2Demo::SelectVariant(const std::string& variant)
{
  const std::string v = (variant == "small-music" || variant == "small-sfx") ? "small-music" : "medium";
  if (!VariantAvailable(v))
  {
    SetStatus(v + " isn't in your models folder yet — download it first");
    return false;
  }

  bool changed = false;
  if (ModelVariant() != v && mCreativeLorasDirty.load(std::memory_order_acquire))
    PersistCreativeLoras();
  {
    std::lock_guard<std::mutex> lock(mModelsMutex);
    if (mModelVariant != v)
    {
      mModelVariant = v;
      changed = true;
    }
  }
  if (changed)
  {
    if (!mBusy.load(std::memory_order_acquire))
      TeardownContext();   // the loaded context is the old variant — reload on next render
    gary::SaveSetting("variant", v);
    LoadPersistedCreativeLoras();
  }
  mModelsPresent.store(true, std::memory_order_release);
  SetStatus("active model: " + v);
  return true;
}

void SA3IPlug2Demo::SetCurrentRenderMode(RenderMode mode)
{
  mCurrentMode.store(ModeIndex(mode), std::memory_order_release);
}

SA3IPlug2Demo::RenderMode SA3IPlug2Demo::CurrentRenderMode() const noexcept
{
  switch (mCurrentMode.load(std::memory_order_acquire))
  {
    case 1: return RenderMode::Transform;
    case 2: return RenderMode::Continue;
    default: return RenderMode::Text;
  }
}

void SA3IPlug2Demo::AdjustDuration(int deltaSeconds)
{
  SetDurationSeconds(DurationSeconds() + deltaSeconds);
}

void SA3IPlug2Demo::AdjustSteps(int deltaSteps)
{
  SetSteps(mSteps.load(std::memory_order_acquire) + deltaSteps);
}

void SA3IPlug2Demo::AdjustInitNoise(float delta)
{
  SetInitNoiseLevel(mInitNoiseLevel.load(std::memory_order_acquire) + delta);
}

void SA3IPlug2Demo::SetDurationSeconds(int seconds)
{
  mDurationSeconds[(size_t)mCurrentMode.load(std::memory_order_acquire)].store(std::clamp(seconds, 1, 300), std::memory_order_release);
}

void SA3IPlug2Demo::SetSteps(int steps)
{
  mSteps.store(std::clamp(steps, 1, 16), std::memory_order_release);   // ARC-distilled model: few steps
}

void SA3IPlug2Demo::SetInitNoiseLevel(float level)
{
  mInitNoiseLevel.store(std::clamp(level, 0.01f, 1.0f), std::memory_order_release);
}

void SA3IPlug2Demo::SetUseSeed(bool useSeed)
{
  if (useSeed && mSeedValue.load(std::memory_order_acquire) <= 0
      && mHasLastSeed.load(std::memory_order_acquire))
  {
    const int64_t lastSeed = mLastSeed.load(std::memory_order_acquire);
    mSeedValue.store(lastSeed, std::memory_order_release);
  }
  mUseSeed.store(useSeed, std::memory_order_release);
}

void SA3IPlug2Demo::ToggleUseSeed()
{
  SetUseSeed(!mUseSeed.load(std::memory_order_acquire));
}

void SA3IPlug2Demo::SetSeedValue(int64_t seed)
{
  mSeedValue.store(std::max<int64_t>(0, seed), std::memory_order_release);
}

void SA3IPlug2Demo::StartRender(RenderMode mode)
{
  if (mDownloading.load(std::memory_order_acquire))
  {
    SetStatus("hang on — models are still downloading");
    return;
  }
  if (!ModelsPresent())
  {
    SetStatus("get your models first — click 'models' to download or point at a folder");
    return;
  }

  if (mBusy.exchange(true, std::memory_order_acq_rel))
  {
    CancelRender();
    return;
  }

  if (mWorker.joinable())
    mWorker.join();

  mCancelRequested.store(false, std::memory_order_release);
  RenderInput input = CaptureRenderInput(mode);
  if (mode != RenderMode::Text && (input.sourceSamples <= 0 || input.sourceChannels.empty()))
  {
    mBusy.store(false, std::memory_order_release);
    SetStatus("drop audio or save a recorded buffer first");
    return;
  }

  // Do NOT stop output playback here: you can generate while the previous output is auditioning. When the new
  // output arrives, InstallOutputFromPlanar swaps the buffer and resets the playhead to 0 (keeps playing).
  mProgress.store(0.f, std::memory_order_release);
  const uint64_t requestId = mRequestId.fetch_add(1, std::memory_order_acq_rel) + 1;
  SetStatus(mode == RenderMode::Text ? "queued text generation"
            : mode == RenderMode::Transform ? "queued transform"
                                            : "queued continuation");
  mWorker = std::thread([this, requestId, input = std::move(input)]() mutable {
    RenderWorkerMain(requestId, std::move(input));
  });
}

void SA3IPlug2Demo::CancelRender()
{
  if (!mBusy.load(std::memory_order_acquire))
    return;

  mCancelRequested.store(true, std::memory_order_release);
  mRequestId.fetch_add(1, std::memory_order_acq_rel);
  mOutputPlaying.store(false, std::memory_order_release);
  SetStatus("cancelling render");
}

bool SA3IPlug2Demo::LoadDroppedAudioFile(const char* rawPath)
{
  const std::string path = NormalizeDroppedPath(rawPath);
  if (path.empty())
  {
    SetSourceStatus("drop failed: empty path");
    return false;
  }

  gary::RecordingSnapshot decoded;
  const auto info = gary::LoadAudioFile(path, decoded);
  if (!info.ok)
  {
    SetSourceStatus(std::string("drop failed: ") + info.error);
    return false;
  }

  const int hostRate = std::max(1, (int)(GetSampleRate() + 0.5));
  gary::RecordingSnapshot playable = decoded.sampleRate == hostRate ? decoded
                                                                    : gary::ResampleSnapshotLinear(decoded, hostRate);
  const int droppedSamples = playable.numSamples;
  const int droppedRate = playable.sampleRate;
  // dropped audio immediately becomes the frozen init snapshot + myBuffer.wav (matches gary), shown all bright
  gary::SaveRecordingWav(playable);
  {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    mInitBuffer = playable.channels;
    mInitSamples = droppedSamples;
    mInitSampleRate = droppedRate;
    mSourceBuffer = std::move(playable.channels);
    mSourceSamples = droppedSamples;
    mSourceSampleRate = droppedRate;
    mRecordWritePosition = droppedSamples;
  }
  mSavedSamples.store(droppedSamples, std::memory_order_release);
  mHasInit.store(true, std::memory_order_release);

  char status[192] = {};
  std::snprintf(status, sizeof(status), "loaded %.2fs from %s",
                (double)droppedSamples / std::max(1, droppedRate),
                CompactText(FileNameFromPath(path), 42).c_str());
  SetSourceStatus(status);
  return true;
}

bool SA3IPlug2Demo::SaveOutputToDisk()
{
  gary::RecordingSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(mOutputMutex);
    if (mOutputSamples <= 0 || mOutputBuffer.empty())
    {
      SetOutputStatus("no output to save");
      return false;
    }
    snapshot.channels = mOutputBuffer;
    snapshot.numSamples = mOutputSamples;
    snapshot.sampleRate = mOutputSampleRate;
  }

  const auto info = gary::SaveOutputWav(snapshot);
  if (!info.ok)
  {
    SetOutputStatus(std::string("save failed: ") + info.error);
    return false;
  }

  SetOutputStatus("saved " + FormatBytes(info.bytes));
  return true;
}

bool SA3IPlug2Demo::SaveSourceToDisk()
{
  gary::RecordingSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    if (mSourceSamples <= 0 || mSourceBuffer.empty())
    {
      SetSourceStatus("no buffer to save");
      return false;
    }
    // freeze the current scratch as the init snapshot (trimmed to the recorded length) + write myBuffer.wav
    mInitBuffer.assign(mSourceBuffer.size(), {});
    for (size_t c = 0; c < mSourceBuffer.size(); ++c)
      mInitBuffer[c].assign(mSourceBuffer[c].begin(), mSourceBuffer[c].begin() + std::min<size_t>(mSourceSamples, mSourceBuffer[c].size()));
    mInitSamples = mSourceSamples;
    mInitSampleRate = mSourceSampleRate;
    snapshot.channels = mInitBuffer;
    snapshot.numSamples = mInitSamples;
    snapshot.sampleRate = mInitSampleRate;
  }
  mSavedSamples.store(snapshot.numSamples, std::memory_order_release);   // this length turns bright red
  mHasInit.store(true, std::memory_order_release);

  const auto info = gary::SaveRecordingWav(snapshot);
  if (!info.ok)
  {
    SetSourceStatus(std::string("save failed: ") + info.error);
    return false;
  }

  SetSourceStatus("saved buffer " + FormatBytes(info.bytes));
  return true;
}

gary::AudioFileInfo SA3IPlug2Demo::CreateOutputDragCopy()
{
  SaveOutputToDisk();
  const auto info = gary::CreateOutputDragCopy();
  if (!info.ok)
    SetOutputStatus(std::string("drag copy failed: ") + info.error);
  return info;
}

void SA3IPlug2Demo::LoadPersistedCreativeLoras()
{
  const auto savedLoras = gary::LoadCreativeLoraRegistry(ModelVariant());
  std::vector<LoraSlot> restored;
  restored.reserve(savedLoras.size());
  int unavailable = 0;
  for (const auto& saved : savedLoras)
  {
    if (saved.path.empty())
      continue;
    const auto info = gary::ImportLoraFile(saved.path);
    if (!info.ok)
    {
      ++unavailable;
      continue;
    }
    LoraSlot slot;
    slot.path = info.path;
    slot.name = info.name.empty() ? FileNameFromPath(info.path) : info.name;
    slot.prompts = info.prompts;
    slot.strength = saved.strength;
    slot.enabled = saved.enabled;
    restored.push_back(std::move(slot));
  }
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    mLoras = std::move(restored);
  }
  mCreativeLorasDirty.store(false, std::memory_order_release);
  if (unavailable > 0)
    SetStatus(std::to_string(unavailable) + " remembered LoRA" + (unavailable == 1 ? " is" : "s are") + " unavailable");
}

void SA3IPlug2Demo::PersistCreativeLoras()
{
  std::vector<LoraSlot> snapshot;
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    snapshot.assign(mLoras.begin(), mLoras.begin()
                    + static_cast<ptrdiff_t>(std::min<size_t>(mLoras.size(), kMaxPersistedLoras)));
  }
  std::vector<gary::PersistedCreativeLora> savedLoras;
  savedLoras.reserve(snapshot.size());
  for (const auto& slot : snapshot)
    savedLoras.push_back({slot.path, slot.strength, slot.enabled});

  const bool saved = gary::SaveCreativeLoraRegistry(ModelVariant(), savedLoras);
  mCreativeLorasDirty.store(!saved, std::memory_order_release);
}

bool SA3IPlug2Demo::ImportLoraFromDialog()
{
#ifdef _WIN32
  char fileName[MAX_PATH] = {};
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = nullptr;
  ofn.lpstrFilter = "SA3 LoRA (*.gguf;*.safetensors;*.ckpt)\0*.gguf;*.safetensors;*.ckpt\0GGUF LoRA (*.gguf)\0*.gguf\0Exported LoRA (*.safetensors)\0*.safetensors\0Checkpoint LoRA (*.ckpt)\0*.ckpt\0All files\0*.*\0";
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  ofn.lpstrTitle = "Import SA3 LoRA";

  if (!GetOpenFileNameA(&ofn))
    return false;

  const auto info = gary::ImportLoraFile(fileName);
  if (!info.ok)
  {
    SetStatus(std::string("LoRA import failed: ") + info.error);
    return false;
  }

  LoraSlot slot;
  slot.path = info.path;
  slot.name = info.name.empty() ? FileNameFromPath(info.path) : info.name;
  slot.prompts = info.prompts;
  slot.strength = 1.0f;
  slot.enabled = true;
  std::string status = "LoRA imported: " + slot.name;
  if (!slot.prompts.empty())
    status += " (" + std::to_string(slot.prompts.size()) + " prompts)";
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    mLoras.push_back(std::move(slot));
  }
  PersistCreativeLoras();
  SetStatus(status);
  return true;
#else
  SetStatus("LoRA file picker is only implemented for Windows in this demo");
  return false;
#endif
}

void SA3IPlug2Demo::RemoveLora(size_t index)
{
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    if (index >= mLoras.size())
      return;
    mLoras.erase(mLoras.begin() + (ptrdiff_t)index);
  }
  PersistCreativeLoras();
}

void SA3IPlug2Demo::SetLoraStrength(size_t index, float strength)
{
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    if (index >= mLoras.size())
      return;
    mLoras[index].strength = std::clamp(strength, 0.0f, 2.0f);
  }
  // Sliders update continuously while dragging; defer the disk write until the UI or instance closes.
  mCreativeLorasDirty.store(true, std::memory_order_release);
}

void SA3IPlug2Demo::SetLoraEnabled(size_t index, bool enabled)
{
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    if (index >= mLoras.size())
      return;
    mLoras[index].enabled = enabled;
  }
  PersistCreativeLoras();
}

std::vector<SA3IPlug2Demo::LoraSlot> SA3IPlug2Demo::Loras() const
{
  std::lock_guard<std::mutex> lock(mLoraMutex);
  return mLoras;
}

bool SA3IPlug2Demo::SerializeState(IByteChunk& chunk) const
{
  chunk.Put(&kPluginStateMagic);
  chunk.Put(&kPluginStateVersion);

  {
    std::lock_guard<std::mutex> lock(mPromptMutex);
    for (const auto& prompt : mPrompts)
      chunk.PutStr(prompt.c_str());
  }

  const int32_t currentMode = std::clamp(mCurrentMode.load(std::memory_order_acquire), 0, 2);
  chunk.Put(&currentMode);
  for (const auto& duration : mDurationSeconds)
  {
    const int32_t value = duration.load(std::memory_order_acquire);
    chunk.Put(&value);
  }

  const int32_t steps = mSteps.load(std::memory_order_acquire);
  const float cfgScale = mCfgScale.load(std::memory_order_acquire);
  const float initNoise = mInitNoiseLevel.load(std::memory_order_acquire);
  const uint8_t useSeed = mUseSeed.load(std::memory_order_acquire) ? 1u : 0u;
  const int64_t seed = mSeedValue.load(std::memory_order_acquire);
  const int64_t lastSeed = mLastSeed.load(std::memory_order_acquire);
  const uint8_t hasLastSeed = mHasLastSeed.load(std::memory_order_acquire) ? 1u : 0u;
  const double bpm = mBpm.load(std::memory_order_acquire);
  const uint8_t bpmOverride = mBpmOverride.load(std::memory_order_acquire) ? 1u : 0u;
  const uint8_t appendBpm = mAppendBpm.load(std::memory_order_acquire) ? 1u : 0u;
  const int32_t keyRoot = mKeyRoot.load(std::memory_order_acquire);
  const int32_t keyMode = mKeyMode.load(std::memory_order_acquire);
  const int32_t loopBars = mLoopBars.load(std::memory_order_acquire);
  const int32_t distShift = mDistShift.load(std::memory_order_acquire);
  chunk.Put(&steps);
  chunk.Put(&cfgScale);
  chunk.Put(&initNoise);
  chunk.Put(&useSeed);
  chunk.Put(&seed);
  chunk.Put(&lastSeed);
  chunk.Put(&hasLastSeed);
  chunk.Put(&bpm);
  chunk.Put(&bpmOverride);
  chunk.Put(&appendBpm);
  chunk.Put(&keyRoot);
  chunk.Put(&keyMode);
  chunk.Put(&loopBars);
  chunk.Put(&distShift);

  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(mLoras.size(), kMaxPersistedLoras));
    chunk.Put(&count);
    for (uint32_t i = 0; i < count; ++i)
    {
      chunk.PutStr(mLoras[i].path.c_str());
      chunk.Put(&mLoras[i].strength);
      const uint8_t enabled = mLoras[i].enabled ? 1u : 0u;
      chunk.Put(&enabled);
    }
  }

  return SerializeParams(chunk);
}

int SA3IPlug2Demo::UnserializeState(const IByteChunk& chunk, int startPos)
{
  const int originalPos = startPos;
  uint32_t magic = 0;
  int next = chunk.Get(&magic, startPos);
  if (next < 0 || magic != kPluginStateMagic)
    return UnserializeParams(chunk, originalPos); // projects saved before custom chunks existed
  startPos = next;

  uint32_t version = 0;
  if ((startPos = chunk.Get(&version, startPos)) < 0 || version != kPluginStateVersion)
    return -1;

  std::array<std::string, 3> prompts;
  auto readString = [&](std::string& value) {
    WDL_String text;
    const int oldPos = startPos;
    const int endPos = chunk.GetStr(text, startPos);
    if (endPos < oldPos + static_cast<int>(sizeof(int)) || text.GetLength() > 65536)
      return false;
    startPos = endPos;
    value = text.Get();
    return true;
  };
  auto readValue = [&](auto& value) {
    const int endPos = chunk.Get(&value, startPos);
    if (endPos < 0)
      return false;
    startPos = endPos;
    return true;
  };

  for (auto& prompt : prompts)
    if (!readString(prompt)) return -1;

  int32_t currentMode = 0;
  std::array<int32_t, 3> durations{};
  int32_t steps = 8;
  float cfgScale = 1.f;
  float initNoise = 0.5f;
  uint8_t useSeed = 0;
  int64_t seed = 0;
  int64_t lastSeed = 0;
  uint8_t hasLastSeed = 0;
  double bpm = 120.0;
  uint8_t bpmOverride = 0;
  uint8_t appendBpm = 1;
  int32_t keyRoot = 0;
  int32_t keyMode = 0;
  int32_t loopBars = 0;
  int32_t distShift = 0;
  if (!readValue(currentMode)) return -1;
  for (auto& duration : durations)
    if (!readValue(duration)) return -1;
  if (!readValue(steps) || !readValue(cfgScale) || !readValue(initNoise)
      || !readValue(useSeed) || !readValue(seed) || !readValue(lastSeed)
      || !readValue(hasLastSeed) || !readValue(bpm) || !readValue(bpmOverride)
      || !readValue(appendBpm) || !readValue(keyRoot) || !readValue(keyMode)
      || !readValue(loopBars) || !readValue(distShift))
    return -1;

  uint32_t loraCount = 0;
  if (!readValue(loraCount) || loraCount > static_cast<uint32_t>(kMaxPersistedLoras))
    return -1;

  struct SavedLora { std::string path; float strength = 1.f; bool enabled = true; };
  std::vector<SavedLora> savedLoras;
  savedLoras.reserve(loraCount);
  for (uint32_t i = 0; i < loraCount; ++i)
  {
    SavedLora saved;
    uint8_t enabled = 1;
    if (!readString(saved.path) || !readValue(saved.strength) || !readValue(enabled))
      return -1;
    saved.enabled = enabled != 0;
    savedLoras.push_back(std::move(saved));
  }

  std::vector<LoraSlot> restoredLoras;
  int unavailableLoras = 0;
  for (const auto& saved : savedLoras)
  {
    const auto info = gary::ImportLoraFile(saved.path);
    if (!info.ok)
    {
      ++unavailableLoras;
      continue;
    }
    LoraSlot slot;
    slot.path = info.path;
    slot.name = info.name.empty() ? FileNameFromPath(info.path) : info.name;
    slot.prompts = info.prompts;
    slot.strength = std::clamp(std::isfinite(saved.strength) ? saved.strength : 1.f, 0.f, 2.f);
    slot.enabled = saved.enabled;
    restoredLoras.push_back(std::move(slot));
  }

  {
    std::lock_guard<std::mutex> lock(mPromptMutex);
    mPrompts = std::move(prompts);
  }
  mCurrentMode.store(std::clamp<int32_t>(currentMode, 0, 2), std::memory_order_release);
  for (size_t i = 0; i < durations.size(); ++i)
    mDurationSeconds[i].store(std::clamp<int32_t>(durations[i], 1, 300), std::memory_order_release);
  mSteps.store(std::clamp<int32_t>(steps, 1, 16), std::memory_order_release);
  mCfgScale.store(std::clamp(std::isfinite(cfgScale) ? cfgScale : 1.f, 0.5f, 2.f), std::memory_order_release);
  mInitNoiseLevel.store(std::clamp(std::isfinite(initNoise) ? initNoise : 0.5f, 0.01f, 1.f), std::memory_order_release);
  mUseSeed.store(useSeed != 0, std::memory_order_release);
  mSeedValue.store(std::max<int64_t>(0, seed), std::memory_order_release);
  mLastSeed.store(std::max<int64_t>(0, lastSeed), std::memory_order_release);
  mHasLastSeed.store(hasLastSeed != 0, std::memory_order_release);
  mBpm.store(std::clamp(std::isfinite(bpm) ? bpm : 120.0, 20.0, 300.0), std::memory_order_release);
  mBpmOverride.store(bpmOverride != 0, std::memory_order_release);
  mAppendBpm.store(appendBpm != 0, std::memory_order_release);
  mKeyRoot.store(std::clamp<int32_t>(keyRoot, 0, 12), std::memory_order_release);
  mKeyMode.store(keyMode ? 1 : 0, std::memory_order_release);
  mLoopBars.store((loopBars == 4 || loopBars == 8 || loopBars == 16) ? loopBars : 0, std::memory_order_release);
  mDistShift.store(std::clamp<int32_t>(distShift, 0, 3), std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    mLoras = std::move(restoredLoras);
  }
  // Host state is per instance; merely loading a preset must not replace the user's global LoRA defaults.
  mCreativeLorasDirty.store(false, std::memory_order_release);
  if (unavailableLoras > 0)
    SetStatus(std::to_string(unavailableLoras) + " saved LoRA" + (unavailableLoras == 1 ? " is" : "s are") + " unavailable");

  return UnserializeParams(chunk, startPos);
}

void SA3IPlug2Demo::SetDecoderLoraEnabled(bool enabled)
{
  mDecoderLoraEnabled.store(enabled, std::memory_order_release);
  gary::SaveSetting("decoder_lora_same_l_enabled", enabled ? "1" : "0");
  SetStatus(enabled ? (ModelVariant() == "medium" ? "SAME-L decoder correction enabled"
                                                   : "decoder correction saved for medium (SAME-L only)")
                    : "decoder correction disabled");
}

std::string SA3IPlug2Demo::DecoderLoraPath() const
{
  std::lock_guard<std::mutex> lock(mDecoderLoraMutex);
  return mDecoderLoraPath;
}

bool SA3IPlug2Demo::DecoderLoraInstalled() const
{
  const std::string path = DecoderLoraPath();
  return !path.empty() && gary::FileSizeBytes(path) > 0;
}

bool SA3IPlug2Demo::DecoderLoraActive() const
{
  return ModelVariant() == "medium" && DecoderLoraEnabled() && DecoderLoraInstalled();
}

std::string SA3IPlug2Demo::DecoderLoraDisplayName() const
{
  const std::string path = DecoderLoraPath();
  return path.empty() ? std::string() : FileNameFromPath(path);
}

void SA3IPlug2Demo::ClearDecoderLoraSelection()
{
  {
    std::lock_guard<std::mutex> lock(mDecoderLoraMutex);
    mDecoderLoraPath.clear();
  }
  gary::SaveSetting("decoder_lora_same_l_path", "");
  SetStatus("decoder correction selection cleared (file kept)");
}

bool SA3IPlug2Demo::ImportDecoderLoraFromDialog()
{
#ifdef _WIN32
  char fileName[MAX_PATH] = {};
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFilter = "SA3 decoder LoRA (*.gguf;*.safetensors)\0*.gguf;*.safetensors\0All files\0*.*\0";
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  ofn.lpstrTitle = "Choose SAME-L decoder correction LoRA";
  if (!GetOpenFileNameA(&ofn)) return false;
  const auto info = gary::ImportLoraFile(fileName);
  if (!info.ok) { SetStatus("decoder LoRA import failed: " + info.error); return false; }
  {
    std::lock_guard<std::mutex> lock(mDecoderLoraMutex);
    mDecoderLoraPath = info.path;
  }
  mDecoderLoraEnabled.store(true, std::memory_order_release);
  gary::SaveSetting("decoder_lora_same_l_path", info.path);
  gary::SaveSetting("decoder_lora_same_l_enabled", "1");
  SetStatus("SAME-L decoder correction selected: " + FileNameFromPath(info.path));
  return true;
#else
  SetStatus("decoder LoRA file picker is currently available on Windows");
  return false;
#endif
}

void SA3IPlug2Demo::SetPeakNormalizeEnabled(bool enabled)
{
  mPeakNormalizeEnabled.store(enabled, std::memory_order_release);
  gary::SaveSetting("peak_normalize_enabled", enabled ? "1" : "0");
}

void SA3IPlug2Demo::SetPeakNormalizeDb(float db)
{
  const float value = std::clamp(db, -6.f, 6.f);
  mPeakNormalizeDb.store(value, std::memory_order_release);
  gary::SaveSetting("peak_normalize_db", SettingFloat(value));
}

void SA3IPlug2Demo::SetLimiterEnabled(bool enabled)
{
  mLimiterEnabled.store(enabled, std::memory_order_release);
  gary::SaveSetting("limiter_enabled", enabled ? "1" : "0");
}

void SA3IPlug2Demo::SetLimiterCeilingDb(float db)
{
  const float value = std::clamp(db, -6.f, 0.f);
  mLimiterCeilingDb.store(value, std::memory_order_release);
  gary::SaveSetting("limiter_ceiling_db", SettingFloat(value));
}

void SA3IPlug2Demo::SetLimiterKnee(float knee)
{
  const float value = std::clamp(knee, 0.1f, 1.f);
  mLimiterKnee.store(value, std::memory_order_release);
  gary::SaveSetting("limiter_knee", SettingFloat(value));
}

void SA3IPlug2Demo::ResetOutputProcessing()
{
  SetPeakNormalizeDb(2.0f);
  SetLimiterCeilingDb(-0.3f);
  SetLimiterKnee(0.8f);
  SetPeakNormalizeEnabled(true);
  SetLimiterEnabled(true);
  SetStatus("output processing reset to tuned defaults");
}

void SA3IPlug2Demo::SetRawOutputProcessing()
{
  SetPeakNormalizeEnabled(false);
  SetLimiterEnabled(false);
  SetStatus("raw output selected (normalize and limiter off)");
}

void SA3IPlug2Demo::ToggleOutputPlayback()
{
  std::lock_guard<std::mutex> lock(mOutputPlaybackMutex);
  if (mOutputPlaybackSamples.load(std::memory_order_acquire) <= 0 || mOutputPlaybackBuffer.empty())
  {
    SetOutputStatus("no output loaded");
    return;
  }

  if (mOutputPlaying.load(std::memory_order_acquire))
  {
    mOutputPlaying.store(false, std::memory_order_release);
    SetOutputStatus("output paused");
  }
  else
  {
    if (mOutputPlayhead.load(std::memory_order_acquire) >= mOutputPlaybackSamples.load(std::memory_order_acquire))
      mOutputPlayhead.store(0, std::memory_order_release);
    mOutputPlaying.store(true, std::memory_order_release);
    SetOutputStatus("playing output");
  }
}

void SA3IPlug2Demo::StopOutputPlayback()
{
  mOutputPlaying.store(false, std::memory_order_release);
  mOutputPlayhead.store(0, std::memory_order_release);
  SetOutputStatus("output stopped");
}

void SA3IPlug2Demo::SeekOutputPlayback(double seconds)
{
  const int sampleRate = std::max(1, mOutputPlaybackSampleRate.load(std::memory_order_acquire));
  const int sampleCount = std::max(0, mOutputPlaybackSamples.load(std::memory_order_acquire));
  const int sample = std::clamp((int)std::llround(seconds * sampleRate), 0, sampleCount);
  mOutputPlayhead.store(sample, std::memory_order_release);
}

SA3IPlug2Demo::Waveform SA3IPlug2Demo::SourceWaveform(int bucketCount) const
{
  bucketCount = std::clamp(bucketCount, 1, 1600);
  Waveform wf;
  wf.peaks.assign((size_t)bucketCount, {});
  std::lock_guard<std::mutex> lock(mSourceMutex);
  wf.numSamples = mSourceSamples;
  wf.sampleRate = mSourceSampleRate;
  wf.savedSamples = std::clamp(mSavedSamples.load(std::memory_order_acquire), 0, mSourceSamples);
  wf.active = mTransportRunning.load(std::memory_order_acquire);
  if (mSourceSamples <= 0 || mSourceBuffer.empty())
    return wf;

  const int channelCount = (int)mSourceBuffer.size();
  for (int x = 0; x < bucketCount; ++x)
  {
    const int start = (int)std::floor((double)x * mSourceSamples / bucketCount);
    const int end = std::max(start + 1, (int)std::ceil((double)(x + 1) * mSourceSamples / bucketCount));
    const int stride = std::max(1, (end - start) / 256);
    float mn = 0.f, mx = 0.f;
    for (int s = start; s < std::min(end, mSourceSamples); s += stride)
    {
      float mixed = 0.f;
      int count = 0;
      for (int c = 0; c < channelCount; ++c)
      {
        const auto& ch = mSourceBuffer[(size_t)c];
        if (s < (int)ch.size())
        {
          mixed += ch[(size_t)s];
          ++count;
        }
      }
      if (count > 0)
        mixed /= (float)count;
      mn = std::min(mn, mixed);
      mx = std::max(mx, mixed);
    }
    wf.peaks[(size_t)x].minValue = std::clamp(mn, -1.f, 1.f);
    wf.peaks[(size_t)x].maxValue = std::clamp(mx, -1.f, 1.f);
  }
  return wf;
}

SA3IPlug2Demo::Waveform SA3IPlug2Demo::OutputWaveform(int bucketCount) const
{
  bucketCount = std::clamp(bucketCount, 1, 1600);
  Waveform wf;
  wf.peaks.assign((size_t)bucketCount, {});
  std::lock_guard<std::mutex> lock(mOutputMutex);
  wf.numSamples = mOutputSamples;
  wf.sampleRate = mOutputSampleRate;
  const int playbackRate = mOutputPlaybackSampleRate.load(std::memory_order_acquire);
  wf.playheadSamples = playbackRate > 0
                     ? (int)std::llround((double)mOutputPlayhead.load(std::memory_order_acquire)
                                         * std::max(1, mOutputSampleRate)
                                         / std::max(1, playbackRate))
                     : 0;
  wf.active = mOutputPlaying.load(std::memory_order_acquire);
  if (mOutputSamples <= 0 || mOutputBuffer.empty())
    return wf;

  const int channelCount = (int)mOutputBuffer.size();
  for (int x = 0; x < bucketCount; ++x)
  {
    const int start = (int)std::floor((double)x * mOutputSamples / bucketCount);
    const int end = std::max(start + 1, (int)std::ceil((double)(x + 1) * mOutputSamples / bucketCount));
    const int stride = std::max(1, (end - start) / 256);
    float mn = 0.f, mx = 0.f;
    for (int s = start; s < std::min(end, mOutputSamples); s += stride)
    {
      float mixed = 0.f;
      int count = 0;
      for (int c = 0; c < channelCount; ++c)
      {
        const auto& ch = mOutputBuffer[(size_t)c];
        if (s < (int)ch.size())
        {
          mixed += ch[(size_t)s];
          ++count;
        }
      }
      if (count > 0)
        mixed /= (float)count;
      mn = std::min(mn, mixed);
      mx = std::max(mx, mixed);
    }
    wf.peaks[(size_t)x].minValue = std::clamp(mn, -1.f, 1.f);
    wf.peaks[(size_t)x].maxValue = std::clamp(mx, -1.f, 1.f);
  }
  return wf;
}

void SA3IPlug2Demo::ResizeRecordBuffer(double sampleRate)
{
  const int maxSamples = std::max(1, (int)(kMaxRecordSeconds * std::min(std::max(1.0, sampleRate), kMaxRecordAllocationSampleRate)));
  std::lock_guard<std::mutex> lock(mSourceMutex);
  mSourceBuffer.assign(kPreferredChannels, std::vector<float>((size_t)maxSamples, 0.f));
  mSourceSamples = 0;
  mSourceSampleRate = std::max(1, (int)(sampleRate + 0.5));
  mRecordWritePosition = 0;
}

void SA3IPlug2Demo::RebuildOutputPlaybackBufferFromNativeLocked(int hostSampleRate)
{
  hostSampleRate = std::max(1, hostSampleRate);
  gary::RecordingSnapshot native;
  native.channels = mOutputBuffer;
  native.numSamples = mOutputSamples;
  native.sampleRate = mOutputSampleRate;

  gary::RecordingSnapshot playback;
  if (native.numSamples > 0 && !native.channels.empty())
    playback = gary::ResampleSnapshotLinear(native, hostSampleRate);
  else
    playback.sampleRate = hostSampleRate;

  {
    std::lock_guard<std::mutex> playbackLock(mOutputPlaybackMutex);
    mOutputPlaybackBuffer = std::move(playback.channels);
    mOutputPlaybackSamples.store(playback.numSamples, std::memory_order_release);
    mOutputPlaybackSampleRate.store(std::max(1, playback.sampleRate), std::memory_order_release);
    mOutputPlayhead.store(0, std::memory_order_release);
    mOutputPlaying.store(false, std::memory_order_release);
  }
}

void SA3IPlug2Demo::StartAutoRecording()
{
  {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    for (auto& ch : mSourceBuffer)
      std::fill(ch.begin(), ch.end(), 0.f);
    mSourceSamples = 0;
    mRecordWritePosition = 0;
  }
  mSavedSamples.store(0, std::memory_order_release);   // fresh scratch take: nothing saved yet (all dim red)
  SetSourceStatus("recording host input");
}

void SA3IPlug2Demo::LoadPersistedBuffer()
{
  const std::string path = gary::RecordingWavPath();
  if (path.empty())
    return;
  gary::RecordingSnapshot snap;
  const auto info = gary::LoadWavFile(path, snap);
  if (!info.ok || snap.numSamples <= 0 || snap.channels.empty())
    return;
  {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    mInitBuffer = std::move(snap.channels);
    mInitSamples = snap.numSamples;
    mInitSampleRate = snap.sampleRate;
  }
  mHasInit.store(true, std::memory_order_release);
}

void SA3IPlug2Demo::StopAutoRecording()
{
  {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    mSourceSamples = mRecordWritePosition;
  }
  SetSourceStatus(mSourceSamples > 0 ? "recording captured" : "transport stopped, no input captured");
}

void SA3IPlug2Demo::CopyInputToRecordBuffer(sample** inputs, int nInChans, int nFrames)
{
  if (!inputs || nInChans <= 0 || nFrames <= 0)
    return;

  std::lock_guard<std::mutex> lock(mSourceMutex);
  if (mSourceBuffer.empty())
    return;
  const int maxSamples = (int)mSourceBuffer[0].size();
  const int copyN = std::min(nFrames, maxSamples - mRecordWritePosition);
  if (copyN <= 0)
    return;

  const int copyChannels = std::min(kPreferredChannels, nInChans);
  for (int c = 0; c < copyChannels; ++c)
  {
    if (!inputs[c])
      continue;
    auto& dst = mSourceBuffer[(size_t)c];
    for (int s = 0; s < copyN; ++s)
      dst[(size_t)mRecordWritePosition + s] = (float)inputs[c][s];
  }

  if (copyChannels == 1 && kPreferredChannels > 1)
  {
    auto& left = mSourceBuffer[0];
    auto& right = mSourceBuffer[1];
    for (int s = 0; s < copyN; ++s)
      right[(size_t)mRecordWritePosition + s] = left[(size_t)mRecordWritePosition + s];
  }

  mRecordWritePosition += copyN;
  mSourceSamples = mRecordWritePosition;
}

void SA3IPlug2Demo::MixOutputPlayback(sample** outputs, int nOutChans, int nFrames)
{
  if (!outputs || nOutChans <= 0 || nFrames <= 0 || !mOutputPlaying.load(std::memory_order_acquire))
    return;

  std::unique_lock<std::mutex> lock(mOutputPlaybackMutex, std::try_to_lock);
  const int playbackSamples = mOutputPlaybackSamples.load(std::memory_order_acquire);
  if (!lock.owns_lock() || playbackSamples <= 0 || mOutputPlaybackBuffer.empty())
    return;

  int playhead = std::clamp(mOutputPlayhead.load(std::memory_order_acquire), 0, playbackSamples);
  const int framesToPlay = std::min(nFrames, playbackSamples - playhead);
  if (framesToPlay <= 0)
  {
    mOutputPlaying.store(false, std::memory_order_release);
    return;
  }

  const int sourceChannels = (int)mOutputPlaybackBuffer.size();
  for (int c = 0; c < nOutChans; ++c)
  {
    if (!outputs[c])
      continue;
    const auto& src = mOutputPlaybackBuffer[(size_t)(c % sourceChannels)];
    for (int s = 0; s < framesToPlay && playhead + s < (int)src.size(); ++s)
      outputs[c][s] += (sample)src[(size_t)playhead + s];
  }

  playhead += framesToPlay;
  if (playhead >= playbackSamples)
  {
    mOutputPlayhead.store(0, std::memory_order_release);
    mOutputPlaying.store(false, std::memory_order_release);
  }
  else
  {
    mOutputPlayhead.store(playhead, std::memory_order_release);
  }
}

SA3IPlug2Demo::RenderInput SA3IPlug2Demo::CaptureRenderInput(RenderMode mode)
{
  RenderInput input;
  input.mode = mode;
  input.durationSeconds = DurationSeconds();
  input.steps = Steps();
  input.cfgScale = CfgScale();
  input.initNoiseLevel = InitNoiseLevel();
  input.useSeed = UseSeed();
  input.seed = SeedValue();
  input.bpm = Bpm();
  input.loopBars = (mode == RenderMode::Text) ? LoopBars() : 0;   // loops are Text-mode only
  input.distShift = DistShift();
  input.variant = ModelVariant();
  input.peakNormalize = PeakNormalizeEnabled();
  input.peakNormalizeDb = PeakNormalizeDb();
  input.limiter = LimiterEnabled();
  input.limiterCeilingDb = LimiterCeilingDb();
  input.limiterKnee = LimiterKnee();

  // Build the prompt actually sent: base + optional " <bpm> bpm" + optional " C minor".
  // Mirrors gary4juce SA3UI (host tempo + key/scale get appended to the text prompt).
  std::string prompt = PromptForMode(mode);
  const std::string keyScale = KeyScaleText();
  if ((AppendBpm() || input.loopBars > 0) && input.bpm > 0.0)
  {
    if (!prompt.empty()) prompt += ' ';
    prompt += std::to_string((int)std::llround(input.bpm)) + " bpm";
  }
  if (!keyScale.empty())
  {
    if (!prompt.empty()) prompt += ' ';
    prompt += keyScale;
  }
  input.prompt = std::move(prompt);
  if (mode != RenderMode::Text)   // transform/continue use the FROZEN snapshot (myBuffer.wav), not the live scratch
  {
    std::lock_guard<std::mutex> lock(mSourceMutex);
    input.sourceChannels = mInitBuffer;
    input.sourceSamples = mInitSamples;
    input.sourceSampleRate = mInitSampleRate;
  }
  {
    std::lock_guard<std::mutex> lock(mLoraMutex);
    for (const auto& lora : mLoras)
      if (lora.enabled && lora.strength > 0.0f && !lora.path.empty())
        input.loras.push_back(lora);
  }
  // The published decoder correction targets SAME-L only. Keep its selection persistent while a small
  // model is active, but never submit it to SAME-S.
  if (input.variant == "medium" && DecoderLoraEnabled())
  {
    const std::string decoderPath = DecoderLoraPath();
    if (!decoderPath.empty() && gary::FileSizeBytes(decoderPath) > 0)
      input.loras.push_back({"SAME-L decoder correction", decoderPath, {}, 1.0f, true});
  }
  return input;
}

void SA3IPlug2Demo::RenderWorkerMain(uint64_t requestId, RenderInput input)
{
  auto finish = [this, requestId](const std::string& status, bool forceStatus = false) {
    if (forceStatus || requestId == mRequestId.load(std::memory_order_acquire))
      SetStatus(status);
    mProgress.store(0.f, std::memory_order_release);
    mBusy.store(false, std::memory_order_release);
  };

  std::string loadError;
  const Sa3Api* sa3 = LoadSa3Api(loadError);
  if (!sa3)
  {
    finish("libsa3 load failed: " + loadError);
    return;
  }

  auto cancelled = [this, requestId]() {
    return mCancelRequested.load(std::memory_order_acquire)
           || requestId != mRequestId.load(std::memory_order_acquire);
  };

  if (cancelled())
  {
    mCancelRequested.store(false, std::memory_order_release);
    finish("render cancelled", true);
    return;
  }

  char err[1024] = {};
  if (!mContext)
  {
    SetStatus("loading libsa3 model (" + input.variant + ")");
    sa3_config cfg = {};
    const std::string models = ModelsDir();
    cfg.models_dir = models.c_str();
    cfg.variant = input.variant.c_str();
    cfg.encoding = "f16";
    mContext = sa3->init(&cfg, err, (int)sizeof err);
    if (!mContext)
    {
      finish(std::string("sa3_init failed: ") + err);
      return;
    }
    if (cancelled())
    {
      sa3->freeContext(mContext);
      mContext = nullptr;
      mCancelRequested.store(false, std::memory_order_release);
      finish("render cancelled", true);
      return;
    }
  }

  gary::RecordingSnapshot source = SourceSnapshotForSA3(input);
  std::vector<float> planar = ToPlanar(source);

  // Loop mode (Text only): generate loop_duration + a little pad with NO schedule ending, then trim to the
  // exact bar length in the plugin. libsa3 has no target_n_samp, so this mirrors sa3-server /generate/loop
  // demo-side (frames carry the pad; duration_padding_sec=0). loopTargetSamples>0 requests the front trim.
  int loopTargetSamples = 0;
  double genSeconds = (double)input.durationSeconds;
  if (input.mode == RenderMode::Text && input.loopBars > 0 && input.bpm > 0.0)
  {
    const double secondsPerBar = (60.0 / input.bpm) * 4.0;   // 4/4
    const double loopSeconds = secondsPerBar * (double)input.loopBars;
    genSeconds = loopSeconds + 2.0;                          // ~2s headroom, trimmed away below
    loopTargetSamples = std::max(1, (int)std::llround(loopSeconds * 44100.0));
  }

  sa3_request_ex req = {};
  req.request.prompt = input.prompt.c_str();
  int frames = std::max(1, (int)(genSeconds * 44100.0 / 4096.0 + 0.5));
  if (input.variant == "small-music" || input.variant == "small-sfx")
    frames = std::max(2, frames & ~1);   // SAME-S needs an even frame count
  req.request.frames = frames;
  req.request.steps = input.steps;
  req.request.seed = input.useSeed ? input.seed : -1;
  req.request.cfg_scale = input.cfgScale;
  req.request.duration_padding_sec = (input.mode == RenderMode::Text && loopTargetSamples == 0) ? 6.0f : 0.0f;
  req.request.keep_models = 0;
  req.request.loudness.set = 1;
  req.request.loudness.peak_normalize = input.peakNormalize ? 1 : 0;
  req.request.loudness.peak_normalize_db = input.peakNormalizeDb;
  req.request.loudness.limiter = input.limiter ? 1 : 0;
  req.request.loudness.limiter_ceiling_db = input.limiterCeilingDb;
  req.request.loudness.limiter_knee = input.limiterKnee;
  // Intentionally not user-facing: these neutral values keep latent-domain controls out of this VST3.
  req.request.loudness.latent_rescale = 1.0f;
  req.request.loudness.latent_shift = 0.0f;
  static const char* kDistShiftNames[4] = {"LogSNR", "Flux", "Full", "None"};   // static: outlives the call
  req.request.dist_shift = kDistShiftNames[std::clamp(input.distShift, 0, 3)];   // params[4] stay 0 -> type defaults
  req.encode_chunk_size = input.mode == RenderMode::Text ? 0 : 128;
  req.encode_overlap = 32;
  req.decode_chunk_size = 128;
  req.decode_overlap = 32;

  std::vector<const char*> loraNames;
  std::vector<float> loraStrengths;
  loraNames.reserve(input.loras.size());
  loraStrengths.reserve(input.loras.size());
  for (const auto& lora : input.loras)
  {
    loraNames.push_back(lora.path.c_str());
    loraStrengths.push_back(lora.strength);
  }
  req.request.n_loras = (int)loraNames.size();
  req.request.lora_names = loraNames.empty() ? nullptr : loraNames.data();
  req.request.lora_strengths = loraStrengths.empty() ? nullptr : loraStrengths.data();

  struct ProgressUser { SA3IPlug2Demo* self; uint64_t requestId; } progressUser{this, requestId};
  req.request.user = &progressUser;
  req.request.on_progress = [](void* user, const char* stage, int step, int total, float fraction) {
    auto* u = static_cast<ProgressUser*>(user);
    if (!u || u->requestId != u->self->mRequestId.load(std::memory_order_acquire))
      return;
    u->self->mProgress.store(fraction, std::memory_order_release);
    char text[160] = {};
    std::snprintf(text, sizeof text, "%s %d/%d %.0f%%", stage ? stage : "render", step, total, fraction * 100.0f);
    u->self->SetStatus(text);
  };
  req.cancel_user = &progressUser;
  req.should_cancel = [](void* user) -> int {
    auto* u = static_cast<ProgressUser*>(user);
    if (!u || !u->self)
      return 1;
    return u->self->mCancelRequested.load(std::memory_order_acquire)
           || u->requestId != u->self->mRequestId.load(std::memory_order_acquire);
  };

  if (input.mode == RenderMode::Transform)
  {
    req.init_audio.mode = SA3_INIT_AUDIO_A2A;
    req.init_audio.samples = planar.data();
    req.init_audio.n_samp = source.numSamples;
    req.init_audio.n_ch = (int)source.channels.size();
    req.init_audio.sample_rate = source.sampleRate;
    req.init_audio.init_noise_level = input.initNoiseLevel;
  }
  else if (input.mode == RenderMode::Continue)
  {
    const float sourceSeconds = source.numSamples > 0 ? (float)source.numSamples / std::max(1, source.sampleRate) : 0.f;
    const float totalSeconds = std::max((float)input.durationSeconds, sourceSeconds + 4.0f);
    req.init_audio.mode = SA3_INIT_AUDIO_INPAINT;
    req.init_audio.samples = planar.data();
    req.init_audio.n_samp = source.numSamples;
    req.init_audio.n_ch = (int)source.channels.size();
    req.init_audio.sample_rate = source.sampleRate;
    req.init_audio.inpaint_start = sourceSeconds;
    req.init_audio.inpaint_end = totalSeconds;
  }

  SetStatus(input.mode == RenderMode::Text ? "generating text audio"
            : input.mode == RenderMode::Transform ? "transforming source"
                                            : "continuing source");
  sa3_audio audio = {};
  const int rc = sa3->generateEx(mContext, &req, &audio, err, (int)sizeof err);
  if (rc != 0)
  {
    if (cancelled())
    {
      mCancelRequested.store(false, std::memory_order_release);
      finish("render cancelled", true);
    }
    else
    {
      finish(std::string("sa3 failed: ") + err);
    }
    return;
  }

  if (cancelled())
  {
    sa3->freeAudio(&audio);
    mCancelRequested.store(false, std::memory_order_release);
    finish("render cancelled", true);
    return;
  }

  mLastSeed.store(RequestableSeed(audio.seed), std::memory_order_release);
  mHasLastSeed.store(true, std::memory_order_release);
  // Loop mode: trim the padded generation back to the exact bar length (native rate is 44100).
  const int keepSamples = (loopTargetSamples > 0) ? std::min(loopTargetSamples, audio.n_samp) : -1;
  InstallOutputFromPlanar(audio.samples, audio.n_samp, audio.n_ch, audio.sample_rate, keepSamples);
  sa3->freeAudio(&audio);
  SaveOutputToDisk();
  finish("render complete");
}

void SA3IPlug2Demo::StopWorker()
{
  mCancelRequested.store(true, std::memory_order_release);
  mRequestId.fetch_add(1, std::memory_order_acq_rel);
  if (mWorker.joinable())
    mWorker.join();
  mBusy.store(false, std::memory_order_release);
  mCancelRequested.store(false, std::memory_order_release);
}

// ----- v0.3.0 model downloader (runs on its own worker thread; never the audio thread) ----------------

namespace
{
std::string HumanBytes(long long bytes)
{
  if (bytes < 0)
    return "?";
  const double mb = (double)bytes / (1024.0 * 1024.0);
  char text[32] = {};
  if (mb >= 1024.0)
    std::snprintf(text, sizeof text, "%.2f GB", mb / 1024.0);
  else
    std::snprintf(text, sizeof text, "%.0f MB", mb);
  return text;
}
} // namespace

void SA3IPlug2Demo::StartModelDownload(int variantIdx, const std::string& destDir)
{
  if (destDir.empty())
  {
    SetStatus("pick a destination folder first");
    return;
  }
  if (mBusy.load(std::memory_order_acquire))
  {
    SetStatus("finish or cancel the current render before downloading");
    return;
  }
  if (mDownloading.exchange(true, std::memory_order_acq_rel))
    return;   // a download is already running

  if (mDownloadWorker.joinable())
    mDownloadWorker.join();
  mDownloadCancel.store(false, std::memory_order_release);
  mDownloadProgress.store(0.0f, std::memory_order_release);
  mDownloadKind.store(1, std::memory_order_release);
  mDownloadWorker = std::thread([this, variantIdx, destDir]() {
    DownloadWorkerMain(variantIdx, destDir);
  });
}

void SA3IPlug2Demo::CancelModelDownload()
{
  if (mDownloading.load(std::memory_order_acquire))
  {
    mDownloadCancel.store(true, std::memory_order_release);
    SetStatus("cancelling download…");
  }
}

void SA3IPlug2Demo::StopDownloadWorker()
{
  mDownloadCancel.store(true, std::memory_order_release);
  if (mDownloadWorker.joinable())
    mDownloadWorker.join();
  mDownloading.store(false, std::memory_order_release);
  mDownloadKind.store(0, std::memory_order_release);
  mDownloadCancel.store(false, std::memory_order_release);
}

void SA3IPlug2Demo::DownloadWorkerMain(int variantIdx, std::string destDir)
{
  namespace fs = std::filesystem;
  const std::string variant = (variantIdx == 1) ? "small-music" : "medium";
  auto finish = [this]() {
    mDownloadProgress.store(0.0f, std::memory_order_release);
    mDownloading.store(false, std::memory_order_release);
    mDownloadKind.store(0, std::memory_order_release);
  };

  const std::vector<gary::ModelDownloadItem> plan = gary::ModelPlan(variant, "f16");

  // Probe sizes up front so the progress bar is byte-accurate. If any HEAD fails we fall back to a
  // coarse file-count bar (still resumable/correct — just a less smooth meter).
  std::vector<long long> sizes(plan.size(), -1);
  long long total = 0;
  bool totalKnown = true;
  for (size_t i = 0; i < plan.size(); ++i)
  {
    if (mDownloadCancel.load(std::memory_order_acquire)) { SetStatus("download cancelled"); finish(); return; }
    SetStatus("checking " + std::string(plan[i].what) + " (" + std::to_string(i + 1) + "/"
              + std::to_string(plan.size()) + ")");
    sizes[i] = gary::HttpContentLength(gary::HuggingFaceResolveUrl(plan[i].repo, plan[i].filename));
    if (sizes[i] > 0) total += sizes[i];
    else totalKnown = false;
  }

  long long completed = 0;   // bytes of fully-finished files (byte mode)
  for (size_t i = 0; i < plan.size(); ++i)
  {
    if (mDownloadCancel.load(std::memory_order_acquire)) { SetStatus("download cancelled"); finish(); return; }

    const std::string dest = (fs::path(destDir) / plan[i].filename).string();
    const std::string url = gary::HuggingFaceResolveUrl(plan[i].repo, plan[i].filename);
    const long long expected = sizes[i];
    long long localSize = (long long)gary::FileSizeBytes(dest);

    if (expected > 0 && localSize == expected)   // already complete — skip (matches models.sh)
    {
      completed += expected;
      if (totalKnown && total > 0)
        mDownloadProgress.store((float)((double)completed / (double)total), std::memory_order_release);
      continue;
    }
    if (expected > 0 && localSize > expected)     // corrupt/oversized — restart this file fresh
    {
      std::remove(dest.c_str());
      localSize = 0;
    }

    std::string spawnErr;
    gary::AsyncProcess proc = gary::StartProcess(
        {"curl", "-fL", "--retry", "3", "-C", "-", "-o", dest, url}, spawnErr);
    if (!proc.valid)
    {
      SetStatus("download failed to start: " + spawnErr);
      finish();
      return;
    }

    int exitCode = 1;
    for (;;)
    {
      const bool done = gary::ProcessTryWait(proc, &exitCode);
      const long long now = (long long)gary::FileSizeBytes(dest);
      if (totalKnown && total > 0)
      {
        const double frac = (double)(completed + now) / (double)total;
        mDownloadProgress.store((float)std::min(1.0, std::max(0.0, frac)), std::memory_order_release);
        char pct[8] = {};
        std::snprintf(pct, sizeof pct, "%.0f%%", std::min(1.0, frac) * 100.0);
        SetStatus("downloading " + std::string(plan[i].what) + " " + std::to_string(i + 1) + "/"
                  + std::to_string(plan.size()) + " — " + pct);
      }
      else
      {
        mDownloadProgress.store((float)i / (float)plan.size(), std::memory_order_release);
        SetStatus("downloading " + std::string(plan[i].what) + " " + std::to_string(i + 1) + "/"
                  + std::to_string(plan.size()) + " — " + HumanBytes(now));
      }

      if (done)
        break;
      if (mDownloadCancel.load(std::memory_order_acquire))
      {
        gary::ProcessTerminate(proc);
        gary::ProcessClose(proc);
        SetStatus("download cancelled (partial files kept — resume any time)");
        finish();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    gary::ProcessClose(proc);

    if (exitCode != 0)
    {
      SetStatus("download failed on " + std::string(plan[i].what) + " (curl exit "
                + std::to_string(exitCode) + ") — click models to retry (resumes)");
      finish();
      return;
    }
    // Integrity guard: a size mismatch means a truncated/garbage file — better to fail loudly here than
    // hand libsa3 a corrupt gguf. Delete it so the retry starts clean instead of resuming garbage.
    if (expected > 0 && (long long)gary::FileSizeBytes(dest) != expected)
    {
      std::remove(dest.c_str());
      SetStatus("download of " + std::string(plan[i].what) + " was incomplete — click models to retry");
      finish();
      return;
    }
    completed += (expected > 0) ? expected : (long long)gary::FileSizeBytes(dest);
  }

  // Validate + persist. UseModelsFolder tears down any stale context and flips ModelsPresent on.
  std::vector<std::string> missing;
  if (gary::ModelSetComplete(destDir, variant, "f16", missing))
  {
    UseModelsFolder(destDir, variant, /*persist=*/true);
    mDownloadProgress.store(1.0f, std::memory_order_release);
    SetStatus("models ready (" + variant + ") — you can generate now");
  }
  else
  {
    std::string msg = "download finished but the set looks incomplete: ";
    for (size_t i = 0; i < missing.size(); ++i)
      msg += (i ? ", " : "") + missing[i];
    SetStatus(msg);
  }
  finish();
}

void SA3IPlug2Demo::StartDecoderLoraDownload()
{
  if (mBusy.load(std::memory_order_acquire))
  {
    SetStatus("finish or cancel the current render before downloading");
    return;
  }
  if (mDownloading.exchange(true, std::memory_order_acq_rel)) return;
  if (mDownloadWorker.joinable()) mDownloadWorker.join();
  mDownloadCancel.store(false, std::memory_order_release);
  mDownloadProgress.store(0.0f, std::memory_order_release);
  mDownloadKind.store(2, std::memory_order_release);
  mDownloadWorker = std::thread([this]() { DecoderLoraDownloadWorkerMain(); });
}

void SA3IPlug2Demo::DecoderLoraDownloadWorkerMain()
{
  namespace fs = std::filesystem;
  auto finish = [this]() {
    mDownloadProgress.store(0.0f, std::memory_order_release);
    mDownloading.store(false, std::memory_order_release);
    mDownloadKind.store(0, std::memory_order_release);
  };

  std::string dirError;
  const std::string dir = gary::LoraDirectory(&dirError);
  if (dir.empty()) { SetStatus("decoder download failed: " + dirError); finish(); return; }

  constexpr const char* kRepo = "thepatch/same-l-decoder-lora";
  constexpr const char* kFile = "squeakfix_v3.safetensors";
  const std::string url = gary::HuggingFaceResolveUrl(kRepo, kFile);
  const std::string dest = (fs::path(dir) / kFile).string();
  SetStatus("checking SAME-L decoder correction");
  const long long expected = gary::HttpContentLength(url);
  long long local = (long long)gary::FileSizeBytes(dest);
  if (expected > 0 && local > expected) { std::remove(dest.c_str()); local = 0; }

  if (!(expected > 0 && local == expected))
  {
    std::string spawnError;
    gary::AsyncProcess proc = gary::StartProcess({"curl", "-fL", "--retry", "3", "-C", "-", "-o", dest, url}, spawnError);
    if (!proc.valid) { SetStatus("decoder download failed to start: " + spawnError); finish(); return; }
    int exitCode = 1;
    for (;;)
    {
      const bool done = gary::ProcessTryWait(proc, &exitCode);
      const long long now = (long long)gary::FileSizeBytes(dest);
      if (expected > 0)
        mDownloadProgress.store((float)std::clamp((double)now / (double)expected, 0.0, 1.0), std::memory_order_release);
      SetStatus("downloading decoder correction - " + (expected > 0 ? HumanBytes(now) + " / " + HumanBytes(expected)
                                                               : HumanBytes(now)));
      if (done) break;
      if (mDownloadCancel.load(std::memory_order_acquire))
      {
        gary::ProcessTerminate(proc);
        gary::ProcessClose(proc);
        SetStatus("decoder download cancelled (partial file kept)");
        finish();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    gary::ProcessClose(proc);
    if (exitCode != 0) { SetStatus("decoder download failed (curl exit " + std::to_string(exitCode) + ")"); finish(); return; }
  }

  if (expected > 0 && (long long)gary::FileSizeBytes(dest) != expected)
  {
    std::remove(dest.c_str());
    SetStatus("decoder download was incomplete - retry");
    finish();
    return;
  }
  if (mDownloadCancel.load(std::memory_order_acquire)) { SetStatus("decoder download cancelled"); finish(); return; }

  mDownloadProgress.store(0.95f, std::memory_order_release);
  SetStatus("converting decoder correction to GGUF");
  const auto info = gary::ImportLoraFile(dest);
  if (!info.ok) { SetStatus("decoder conversion failed: " + info.error); finish(); return; }
  {
    std::lock_guard<std::mutex> lock(mDecoderLoraMutex);
    mDecoderLoraPath = info.path;
  }
  mDecoderLoraEnabled.store(true, std::memory_order_release);
  gary::SaveSetting("decoder_lora_same_l_path", info.path);
  gary::SaveSetting("decoder_lora_same_l_enabled", "1");
  mDownloadProgress.store(1.0f, std::memory_order_release);
  SetStatus(ModelVariant() == "medium" ? "SAME-L decoder correction ready and enabled"
                                       : "decoder correction ready - saved for medium only");
  finish();
}

void SA3IPlug2Demo::SetStatus(const std::string& text)
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  mStatus = text;
}

void SA3IPlug2Demo::SetSourceStatus(const std::string& text)
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  mSourceStatus = text;
}

void SA3IPlug2Demo::SetOutputStatus(const std::string& text)
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  mOutputStatus = text;
}

void SA3IPlug2Demo::InstallOutputFromPlanar(const float* samples, int nSamp, int nCh, int sampleRate, int keepSamples)
{
  if (!samples || nSamp <= 0 || nCh <= 0)
    return;

  // keepSamples < 0 keeps everything; otherwise take the first keepSamples per channel (loop trim), reading
  // with the source's full nSamp stride so the planar channel offsets stay correct.
  const int outSamps = (keepSamples > 0 && keepSamples < nSamp) ? keepSamples : nSamp;
  std::vector<std::vector<float>> next((size_t)nCh, std::vector<float>((size_t)outSamps, 0.f));
  for (int c = 0; c < nCh; ++c)
    std::copy(samples + (size_t)c * nSamp, samples + (size_t)c * nSamp + outSamps, next[(size_t)c].begin());

  {
    std::lock_guard<std::mutex> lock(mOutputMutex);
    mOutputBuffer = std::move(next);
    mOutputSamples = outSamps;
    mOutputSampleRate = std::max(1, sampleRate);
    // resets playhead to 0 and stops playback: the swap ends the previous audition cleanly (user's preference).
    RebuildOutputPlaybackBufferFromNativeLocked(mHostSampleRate.load(std::memory_order_acquire));
  }
  mOutputRevision.fetch_add(1, std::memory_order_acq_rel);
  char status[128] = {};
  std::snprintf(status, sizeof status, "output %.2fs @ %d Hz ready",
                (double)outSamps / std::max(1, sampleRate), std::max(1, sampleRate));
  SetOutputStatus(status);
}

gary::RecordingSnapshot SA3IPlug2Demo::SourceSnapshotForSA3(const RenderInput& input) const
{
  gary::RecordingSnapshot snapshot;
  snapshot.numSamples = input.sourceSamples;
  snapshot.sampleRate = input.sourceSampleRate;
  if (input.sourceSamples <= 0 || input.sourceChannels.empty())
    return snapshot;

  snapshot.channels.assign(kPreferredChannels, std::vector<float>((size_t)input.sourceSamples, 0.f));
  const int sourceChannels = (int)input.sourceChannels.size();
  for (int c = 0; c < kPreferredChannels; ++c)
  {
    const auto& src = input.sourceChannels[(size_t)(c % sourceChannels)];
    const int n = std::min(input.sourceSamples, (int)src.size());
    if (n > 0)
      std::copy(src.begin(), src.begin() + n, snapshot.channels[(size_t)c].begin());
  }
  return snapshot;
}

std::string SA3IPlug2Demo::NormalizeDroppedPath(const char* rawPath)
{
  std::string path = rawPath ? rawPath : "";
  const auto first = path.find_first_not_of(" \t\r\n\"");
  if (first == std::string::npos)
    return {};
  const auto last = path.find_last_not_of(" \t\r\n\"");
  path = path.substr(first, last - first + 1);
  constexpr const char* filePrefix = "file:///";
  if (path.rfind(filePrefix, 0) == 0)
  {
    path = path.substr(std::strlen(filePrefix));
    std::replace(path.begin(), path.end(), '/', '\\');
  }
  return path;
}

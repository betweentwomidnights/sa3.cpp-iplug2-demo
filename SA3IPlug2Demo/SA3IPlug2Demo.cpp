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

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>

// The build system (CMakeLists) always sets this from SA3_CPP_DIR; overridable at runtime with SA3_MODELS_DIR.
#ifndef SA3_DEMO_DEFAULT_MODELS_DIR
#define SA3_DEMO_DEFAULT_MODELS_DIR "models"
#endif

namespace
{
constexpr const char* kDemoFont = "DemoRoboto";
constexpr int kCtrlTagMain = 1000;

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
#endif
  InitFn init = nullptr;
  GenerateExFn generateEx = nullptr;
  FreeAudioFn freeAudio = nullptr;
  FreeContextFn freeContext = nullptr;

  bool Ready() const noexcept
  {
#ifdef _WIN32
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
#else
    error = "runtime libsa3 loading is only implemented for Windows in this demo";
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
    Bpm
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
    float y = shell.T + 14.f;
    const SA3IPlug2Demo::RenderMode mode = mPlugin.CurrentRenderMode();

    g.DrawText(IText(20.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               "sa3 embedded", IRECT(left, y, shell.MW(), y + 26.f));
    g.DrawText(IText(12.f, TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               CompactText(mPlugin.ModelsDir(), FitChars(right - shell.MW(), 6.5f, 12, 42)).c_str(), IRECT(shell.MW(), y, right, y + 26.f));
    y += 30.f;

    const float progress = std::clamp(mPlugin.Progress(), 0.f, 1.f);
    const IRECT statusRect(left, y, right, y + 22.f);
    g.FillRoundRect(PanelDark(), statusRect, 3.f);
    if (mPlugin.Busy())
      g.FillRoundRect(RedDim(), IRECT(statusRect.L, statusRect.T, statusRect.L + statusRect.W() * progress, statusRect.B), 3.f);
    g.DrawRoundRect(FrameSoft(), statusRect, 3.f);
    g.DrawText(IText(12.f, COLOR_WHITE, kDemoFont, EAlign::Near, EVAlign::Middle),
               CompactText(mPlugin.StatusText(), FitChars(statusRect.W() - 16.f, 6.5f, 18, 96)).c_str(), statusRect.GetPadded(-8.f));
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
    const bool canRender = mPlugin.Busy() || mPlugin.CanRender(mode);   // transform/continue need a frozen snapshot
    DrawButton(g, mRunRect, mPlugin.Busy() ? "cancel" : ActionLabel(mode), kDemoFont, false, canRender);
    const char* runHint = !canRender ? "drop audio or save a recorded buffer first"
                        : mPlugin.TransportRunning() ? "host rolling: recording input" : "host stopped";
    g.DrawText(IText(11.f, TextDim(), kDemoFont, EAlign::Far, EVAlign::Middle),
               runHint, IRECT(mRunRect.R + 10.f, y, right, y + 32.f));
    y += 42.f;

    const float outputHeight = std::min(190.f, std::max(150.f, shell.B - y - 14.f));
    const IRECT outputRect(left, y, right, y + outputHeight);
    DrawOutputPanel(g, outputRect);
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
    if (mDiceRect.Contains(x, y)) return {Hit::Dice, 0};
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

  void DrawTab(IGraphics& g, const IRECT& bounds, const char* label, bool active)
  {
    using namespace gary::ui;
    g.FillRoundRect(active ? Red() : ButtonFill(), bounds, 4.f);
    g.DrawRoundRect(active ? Red() : Frame(), bounds, 4.f);
    g.DrawText(IText(13.f, active ? COLOR_BLACK : COLOR_WHITE, kDemoFont, EAlign::Center, EVAlign::Middle),
               label, bounds.GetPadded(-4.f));
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
  enum class Popup { None, KeyRoot, KeyMode, DistShift } mActivePopup = Popup::None;
  IPopupMenu mKeyMenu;
  EditTarget mEditTarget = EditTarget::None;
  SA3IPlug2Demo::RenderMode mEditPromptMode = SA3IPlug2Demo::RenderMode::Text;
  size_t mActiveLoraIndex = 0;
  bool mOutputPointerDown = false;
  bool mOutputDragStarted = false;
  float mOutputDragStartX = 0.f;
  float mOutputDragStartY = 0.f;
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
  StopWorker();
  if (mContext)
  {
    if (const Sa3Api* sa3 = LoadedSa3Api())
      sa3->freeContext(mContext);
    mContext = nullptr;
  }
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
  if (const char* e = std::getenv("SA3_MODELS_DIR"))
    if (*e) return e;
  return SA3_DEMO_DEFAULT_MODELS_DIR;
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
  SetStatus(status);
  return true;
#else
  SetStatus("LoRA file picker is only implemented for Windows in this demo");
  return false;
#endif
}

void SA3IPlug2Demo::RemoveLora(size_t index)
{
  std::lock_guard<std::mutex> lock(mLoraMutex);
  if (index < mLoras.size())
    mLoras.erase(mLoras.begin() + (ptrdiff_t)index);
}

void SA3IPlug2Demo::SetLoraStrength(size_t index, float strength)
{
  std::lock_guard<std::mutex> lock(mLoraMutex);
  if (index < mLoras.size())
    mLoras[index].strength = std::clamp(strength, 0.0f, 2.0f);
}

void SA3IPlug2Demo::SetLoraEnabled(size_t index, bool enabled)
{
  std::lock_guard<std::mutex> lock(mLoraMutex);
  if (index < mLoras.size())
    mLoras[index].enabled = enabled;
}

std::vector<SA3IPlug2Demo::LoraSlot> SA3IPlug2Demo::Loras() const
{
  std::lock_guard<std::mutex> lock(mLoraMutex);
  return mLoras;
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
    SetStatus("loading libsa3 model");
    sa3_config cfg = {};
    const std::string models = ModelsDir();
    cfg.models_dir = models.c_str();
    cfg.variant = "medium";
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
  req.request.frames = std::max(1, (int)(genSeconds * 44100.0 / 4096.0 + 0.5));
  req.request.steps = input.steps;
  req.request.seed = input.useSeed ? input.seed : -1;
  req.request.cfg_scale = input.cfgScale;
  req.request.duration_padding_sec = (input.mode == RenderMode::Text && loopTargetSamples == 0) ? 6.0f : 0.0f;
  req.request.keep_models = 0;
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

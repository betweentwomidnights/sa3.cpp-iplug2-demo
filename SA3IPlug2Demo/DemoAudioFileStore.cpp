#include "DemoAudioFileStore.h"

#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_libs/dr_mp3.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <Windows.h>
  #include <KnownFolders.h>
  #include <ShlObj.h>
#else
  #include <cerrno>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #include <signal.h>
  #include <spawn.h>
  extern char** environ;
#endif

namespace gary
{
namespace
{
constexpr const char* kAppFolderName = "sa3-iplug2-demo";
constexpr const char* kRecordingFileName = "myBuffer.wav";
constexpr const char* kOutputFileName = "myOutput.wav";
constexpr const char* kOutputUndoFileName = "myOutput.undo.wav";
constexpr const char* kDraggedAudioFolderName = "dragged_audio";
constexpr const char* kLoraFolderName = "loras";

// The build system (CMakeLists) always sets this from SA3_CPP_DIR; this is only a no-CMake fallback.
#ifndef SA3_DEMO_SA3_CPP_DIR
#define SA3_DEMO_SA3_CPP_DIR "sa3.cpp"
#endif

std::string ExtensionLower(const std::string& path);

std::string JoinPath(const std::string& left, const char* right)
{
  if (left.empty())
    return right != nullptr ? std::string(right) : std::string();

#if defined(_WIN32)
  constexpr char separator = '\\';
#else
  constexpr char separator = '/';
#endif

  if (left.back() == '\\' || left.back() == '/')
    return left + right;

  return left + separator + right;
}

std::string ToLower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string Trim(std::string text)
{
  const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
  if (begin >= end)
    return {};
  return std::string(begin, end);
}

bool SegmentHasBpm(const std::string& segment)
{
  const std::string lower = ToLower(segment);
  const size_t bpm = lower.find("bpm");
  if (bpm == std::string::npos)
    return false;

  for (size_t i = bpm; i > 0; --i)
  {
    const unsigned char c = static_cast<unsigned char>(lower[i - 1u]);
    if (std::isdigit(c) != 0)
      return true;
    if (std::isspace(c) == 0 && c != '-' && c != '_' && c != '.')
      return false;
  }
  return false;
}

bool SegmentLooksLikeKey(const std::string& segment)
{
  const std::string trimmed = Trim(segment);
  if (trimmed.empty() || trimmed.size() > 24u)
    return false;

  const std::string lower = ToLower(trimmed);
  const char root = lower[0];
  if (root < 'a' || root > 'g')
    return false;

  size_t pos = 1u;
  while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos])) != 0)
    ++pos;

  if (pos < lower.size() && (lower[pos] == '#' || lower[pos] == 'b'))
    ++pos;
  else if (lower.compare(pos, 5u, "sharp") == 0u)
    pos += 5u;
  else if (lower.compare(pos, 4u, "flat") == 0u)
    pos += 4u;

  while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos])) != 0)
    ++pos;

  if (lower.compare(pos, 5u, "major") == 0u)
    pos += 5u;
  else if (lower.compare(pos, 5u, "minor") == 0u)
    pos += 5u;
  else
    return false;

  while (pos < lower.size() && std::isspace(static_cast<unsigned char>(lower[pos])) != 0)
    ++pos;
  return pos == lower.size();
}

std::string StripPromptMetadata(std::string prompt)
{
  std::vector<std::string> kept;
  size_t start = 0u;
  while (start <= prompt.size())
  {
    const size_t comma = prompt.find(',', start);
    std::string segment = Trim(prompt.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    if (!segment.empty() && !SegmentHasBpm(segment) && !SegmentLooksLikeKey(segment))
      kept.push_back(std::move(segment));

    if (comma == std::string::npos)
      break;
    start = comma + 1u;
  }

  std::string stripped;
  for (const auto& segment : kept)
  {
    if (!stripped.empty())
      stripped += ", ";
    stripped += segment;
  }
  return Trim(std::move(stripped));
}

std::string DirectoryOnly(const std::string& path)
{
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string WithoutExtension(const std::string& path)
{
  const size_t slash = path.find_last_of("\\/");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return path;
  return path.substr(0, dot);
}

std::string StemOnly(const std::string& path)
{
  std::string name = path;
  const size_t slash = name.find_last_of("\\/");
  if (slash != std::string::npos)
    name = name.substr(slash + 1u);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos)
    name = name.substr(0, dot);
  return name;
}

bool FileExists(const std::string& path)
{
  return FileSizeBytes(path) > 0;
}

bool DirectoryExists(const std::string& path)
{
  if (path.empty())
    return false;
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesA(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st = {};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string ParentDirectory(const std::string& path)
{
  return DirectoryOnly(DirectoryOnly(path));
}

void AddUniquePrompt(std::vector<std::string>& prompts, std::string prompt)
{
  prompt = Trim(std::move(prompt));
  if (prompt.empty())
    return;
  if (prompt.size() >= 3 && static_cast<unsigned char>(prompt[0]) == 0xef
      && static_cast<unsigned char>(prompt[1]) == 0xbb
      && static_cast<unsigned char>(prompt[2]) == 0xbf)
  {
    prompt.erase(0, 3);
    prompt = Trim(std::move(prompt));
  }
  prompt = StripPromptMetadata(std::move(prompt));
  if (prompt.empty())
    return;
  if (std::find(prompts.begin(), prompts.end(), prompt) == prompts.end())
    prompts.push_back(std::move(prompt));
}

std::string ReadWholeTextFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void AppendJsonArrayStrings(const std::string& path, std::vector<std::string>& prompts)
{
  const std::string text = ReadWholeTextFile(path);
  if (text.empty())
    return;

  int arrayDepth = 0;
  bool inString = false;
  bool escape = false;
  std::string current;
  for (char c : text)
  {
    if (!inString)
    {
      if (c == '[')
        ++arrayDepth;
      else if (c == ']' && arrayDepth > 0)
        --arrayDepth;
      else if (c == '"' && arrayDepth > 0)
      {
        inString = true;
        escape = false;
        current.clear();
      }
      continue;
    }

    if (escape)
    {
      switch (c)
      {
        case 'n': current.push_back('\n'); break;
        case 'r': current.push_back('\r'); break;
        case 't': current.push_back('\t'); break;
        default: current.push_back(c); break;
      }
      escape = false;
      continue;
    }

    if (c == '\\')
    {
      escape = true;
      continue;
    }
    if (c == '"')
    {
      inString = false;
      AddUniquePrompt(prompts, current);
      continue;
    }
    current.push_back(c);
  }
}

void AppendTxtPromptsFromDirectory(const std::string& directory, std::vector<std::string>& prompts)
{
  if (!DirectoryExists(directory))
    return;

#if defined(_WIN32)
  const std::string pattern = JoinPath(directory, "*.txt");
  WIN32_FIND_DATAA findData = {};
  HANDLE find = FindFirstFileA(pattern.c_str(), &findData);
  if (find == INVALID_HANDLE_VALUE)
    return;

  do
  {
    if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      continue;
    AddUniquePrompt(prompts, ReadWholeTextFile(JoinPath(directory, findData.cFileName)));
  } while (FindNextFileA(find, &findData));

  FindClose(find);
#endif
}

std::string NormalizeLoraName(const std::string& pathOrName)
{
  std::string name = StemOnly(pathOrName);
  std::string lower = ToLower(name);
  if (lower.rfind("lora-", 0) == 0 && name.size() > 5)
  {
    name = name.substr(5);
    lower = ToLower(name);
  }

  const char* suffixes[] = { "-f32", "-f16", "-q8_0", "-q5_1", "-q5_0", "-q4_1", "-q4_0" };
  for (const char* suffix : suffixes)
  {
    const std::string s = suffix;
    if (lower.size() > s.size() && lower.rfind(s) == lower.size() - s.size())
    {
      name.resize(name.size() - s.size());
      break;
    }
  }
  return name.empty() ? StemOnly(pathOrName) : name;
}

std::string QuoteArg(const std::string& arg)
{
  std::string out = "\"";
  for (char c : arg)
  {
    if (c == '"')
      out += "\\\"";
    else
      out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string PythonExecutable()
{
  const std::string venv = JoinPath(JoinPath(Sa3CppDirectory(), ".venv"), "Scripts\\python.exe");
  if (FileExists(venv))
    return venv;
  return "python";
}

bool RunProcessAndWait(const std::string& commandLine, const std::string& workDir, std::string& error)
{
#if defined(_WIN32)
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION process = {};
  std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back('\0');

  const BOOL ok = CreateProcessA(nullptr,
                                 mutableCommand.data(),
                                 nullptr,
                                 nullptr,
                                 FALSE,
                                 CREATE_NO_WINDOW,
                                 nullptr,
                                 workDir.empty() ? nullptr : workDir.c_str(),
                                 &startup,
                                 &process);
  if (!ok)
  {
    error = "failed to launch converter, win32 error " + std::to_string(GetLastError());
    return false;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exitCode != 0)
  {
    error = "converter exited with code " + std::to_string(exitCode);
    return false;
  }
  return true;
#else
  (void)commandLine;
  (void)workDir;
  error = "LoRA conversion is only wired for Windows in this demo";
  return false;
#endif
}

bool LoraExportPairExists(const std::string& base)
{
  return FileExists(base + ".safetensors") && FileExists(base + ".json");
}

std::string FindExportedLoraBase(const std::string& path, std::string& error)
{
  const std::string ext = ExtensionLower(path);
  const std::string base = WithoutExtension(path);
  if (ext == "safetensors")
  {
    if (LoraExportPairExists(base))
      return base;
    error = "safetensors import needs matching " + StemOnly(path) + ".json metadata";
    return {};
  }

  if (ext != "ckpt")
  {
    error = "unsupported LoRA source type";
    return {};
  }

  std::vector<std::string> candidates;
  candidates.push_back(base);

  const std::string dir = DirectoryOnly(path);
  const std::string parent = ParentDirectory(path);
  const std::string stem = StemOnly(path);
  if (!parent.empty())
    candidates.push_back(JoinPath(parent, stem.c_str()));
  if (!dir.empty())
    candidates.push_back(JoinPath(dir, NormalizeLoraName(path).c_str()));

  for (const std::string& candidate : candidates)
    if (LoraExportPairExists(candidate))
      return candidate;

  error = "ckpt import needs exported " + stem + ".safetensors and " + stem + ".json; run tools/lora_ckpt_export.py first";
  return {};
}

std::vector<std::string> LoadPromptPoolForLoraSource(const std::string& sourcePath, const std::string& displayName)
{
  std::vector<std::string> prompts;
  const std::string dir = DirectoryOnly(sourcePath);
  const std::string parent = ParentDirectory(sourcePath);
  const std::string stem = NormalizeLoraName(displayName.empty() ? sourcePath : displayName);

  AppendTxtPromptsFromDirectory(dir, prompts);
  if (!dir.empty())
    AppendTxtPromptsFromDirectory(JoinPath(dir, stem.c_str()), prompts);
  if (!parent.empty())
    AppendTxtPromptsFromDirectory(JoinPath(parent, stem.c_str()), prompts);

  const std::string sa3Root = Sa3CppDirectory();
  AppendTxtPromptsFromDirectory(JoinPath(JoinPath(sa3Root, "loras"), stem.c_str()), prompts);
  AppendJsonArrayStrings(JoinPath(JoinPath(sa3Root, "prompts"), (stem + ".json").c_str()), prompts);
  return prompts;
}

#if defined(_WIN32)
// Native in-process safetensors->gguf via libsa3's sa3_convert_lora (no Python). Loads sa3.dll from beside
// this module (same resolution the render path uses). Returns: 1 converted, 0 native unavailable (fall back
// to Python), -1 native ran but the conversion failed (error set).
int TryNativeConvertLora(const std::string& exportedBase, const std::string& destination, std::string& error)
{
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&TryNativeConvertLora), &self))
    return 0;
  wchar_t buffer[1024];
  const DWORD len = GetModuleFileNameW(self, buffer, 1024);
  if (len == 0 || len >= 1024)
    return 0;
  std::wstring path(buffer, len);
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos)
    return 0;
  const std::wstring dllPath = path.substr(0, slash) + L"\\sa3.dll";

  HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!dll)
    return 0;
  using ConvFn = int (*)(const char*, const char*, const char*, char*, int);
  auto fn = reinterpret_cast<ConvFn>(GetProcAddress(dll, "sa3_convert_lora"));
  if (!fn) { FreeLibrary(dll); return 0; }   // older sa3.dll without the function

  const std::string safet = exportedBase + ".safetensors";
  const std::string json  = exportedBase + ".json";
  char err[512] = {};
  const int rc = fn(safet.c_str(), json.c_str(), destination.c_str(), err, (int)sizeof err);
  FreeLibrary(dll);
  if (rc != 0) { error = std::string("libsa3 convert: ") + err; return -1; }
  return 1;
}
#endif

bool ConvertLoraToGguf(const std::string& exportedBase, const std::string& destination, std::string& error)
{
#if defined(_WIN32)
  // Prefer the native in-process converter (no Python); fall back to the .venv script only if unavailable.
  {
    std::string nativeErr;
    const int native = TryNativeConvertLora(exportedBase, destination, nativeErr);
    if (native == 1)
    {
      if (!FileExists(destination)) { error = "converter did not create a gguf file"; return false; }
      return true;
    }
    if (native == -1) { error = nativeErr; return false; }   // conversion genuinely failed
    // native == 0: sa3.dll/function unavailable -> fall through to the Python converter
  }
#endif
  const std::string sa3Root = Sa3CppDirectory();
  const std::string script = JoinPath(JoinPath(sa3Root, "tools"), "convert_lora.py");
  if (!FileExists(script))
  {
    error = "missing converter script at " + script;
    return false;
  }

  const std::string command = QuoteArg(PythonExecutable())
                            + " "
                            + QuoteArg(script)
                            + " --in "
                            + QuoteArg(exportedBase)
                            + " --out "
                            + QuoteArg(destination);
  if (!RunProcessAndWait(command, sa3Root, error))
    return false;
  if (!FileExists(destination))
  {
    error = "converter did not create a gguf file";
    return false;
  }
  return true;
}

#if defined(_WIN32)
std::string WideToUtf8(const wchar_t* text)
{
  if (text == nullptr || text[0] == L'\0')
    return {};

  const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1)
    return {};

  std::string result(static_cast<size_t>(required - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], required, nullptr, nullptr);
  return result;
}
#endif

std::string UserDocumentsPath(std::string* error)
{
#if defined(_WIN32)
  PWSTR widePath = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &widePath);
  if (SUCCEEDED(hr) && widePath != nullptr)
  {
    std::string path = WideToUtf8(widePath);
    CoTaskMemFree(widePath);
    if (!path.empty())
      return path;
  }

  if (widePath != nullptr)
    CoTaskMemFree(widePath);

  if (const char* userProfile = std::getenv("USERPROFILE"))
    return JoinPath(userProfile, "Documents");

  if (error != nullptr)
    *error = "could not locate the Windows Documents folder";
  return {};
#else
  if (const char* home = std::getenv("HOME"))
    return JoinPath(home, "Documents");

  if (error != nullptr)
    *error = "could not locate the home Documents folder";
  return {};
#endif
}

bool EnsureDirectoryExists(const std::string& path, std::string* error)
{
  if (path.empty())
  {
    if (error != nullptr)
      *error = "empty directory path";
    return false;
  }

#if defined(_WIN32)
  if (CreateDirectoryA(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
    return true;

  if (error != nullptr)
  {
    char message[128] = {};
    std::snprintf(message, sizeof(message), "failed to create folder, win32 error %lu", GetLastError());
    *error = message;
  }
  return false;
#else
  if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
    return true;

  if (error != nullptr)
    *error = std::string("failed to create folder: ") + std::strerror(errno);
  return false;
#endif
}

void WriteU16LE(std::ofstream& out, uint16_t value)
{
  const char bytes[2] = {
    static_cast<char>(value & 0xff),
    static_cast<char>((value >> 8) & 0xff)
  };
  out.write(bytes, sizeof(bytes));
}

void WriteU32LE(std::ofstream& out, uint32_t value)
{
  const char bytes[4] = {
    static_cast<char>(value & 0xff),
    static_cast<char>((value >> 8) & 0xff),
    static_cast<char>((value >> 16) & 0xff),
    static_cast<char>((value >> 24) & 0xff)
  };
  out.write(bytes, sizeof(bytes));
}

uint16_t ReadU16LE(const unsigned char* data)
{
  return static_cast<uint16_t>(data[0])
       | static_cast<uint16_t>(data[1] << 8);
}

uint32_t ReadU32LE(const unsigned char* data)
{
  return static_cast<uint32_t>(data[0])
       | (static_cast<uint32_t>(data[1]) << 8)
       | (static_cast<uint32_t>(data[2]) << 16)
       | (static_cast<uint32_t>(data[3]) << 24);
}

int32_t ReadS24LE(const unsigned char* data)
{
  int32_t value = static_cast<int32_t>(data[0])
                | (static_cast<int32_t>(data[1]) << 8)
                | (static_cast<int32_t>(data[2]) << 16);
  if ((value & 0x00800000) != 0)
    value |= ~0x00ffffff;
  return value;
}

int32_t ReadS32LE(const unsigned char* data)
{
  return static_cast<int32_t>(ReadU32LE(data));
}

int16_t FloatToPcm16(float value)
{
  const float clamped = std::max(-1.f, std::min(1.f, value));
  const float scaled = clamped < 0.f ? clamped * 32768.f : clamped * 32767.f;
  return static_cast<int16_t>(std::lrintf(scaled));
}

float PcmToFloat(const unsigned char* data, int bitsPerSample)
{
  switch (bitsPerSample)
  {
    case 8:
      return (static_cast<int>(data[0]) - 128) / 128.f;
    case 16:
      return static_cast<int16_t>(ReadU16LE(data)) / 32768.f;
    case 24:
      return ReadS24LE(data) / 8388608.f;
    case 32:
      return ReadS32LE(data) / 2147483648.f;
    default:
      return 0.f;
  }
}

float Float32FromLE(const unsigned char* data)
{
  const uint32_t raw = ReadU32LE(data);
  float value = 0.f;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

double Float64FromLE(const unsigned char* data)
{
  uint64_t raw = 0;
  for (int i = 0; i < 8; ++i)
    raw |= static_cast<uint64_t>(data[i]) << (8 * i);

  double value = 0.0;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

bool WritePcm16Wav(const std::string& path, const RecordingSnapshot& snapshot, std::string& error)
{
  const int numChannels = static_cast<int>(snapshot.channels.size());
  if (numChannels <= 0 || snapshot.numSamples <= 0)
  {
    error = "no recorded samples to write";
    return false;
  }

  const int sampleRate = std::max(1, snapshot.sampleRate);
  const uint16_t bitsPerSample = 16;
  const uint16_t blockAlign = static_cast<uint16_t>(numChannels * (bitsPerSample / 8));
  const uint32_t byteRate = static_cast<uint32_t>(sampleRate * blockAlign);
  const uint64_t dataSize64 = static_cast<uint64_t>(snapshot.numSamples) * blockAlign;

  if (dataSize64 > 0xffffffffu)
  {
    error = "recording is too large for a classic WAV file";
    return false;
  }

  std::remove(path.c_str());

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
  {
    error = "could not open WAV for writing";
    return false;
  }

  const uint32_t dataSize = static_cast<uint32_t>(dataSize64);
  const uint32_t riffSize = 36u + dataSize;

  out.write("RIFF", 4);
  WriteU32LE(out, riffSize);
  out.write("WAVE", 4);

  out.write("fmt ", 4);
  WriteU32LE(out, 16);
  WriteU16LE(out, 1);
  WriteU16LE(out, static_cast<uint16_t>(numChannels));
  WriteU32LE(out, static_cast<uint32_t>(sampleRate));
  WriteU32LE(out, byteRate);
  WriteU16LE(out, blockAlign);
  WriteU16LE(out, bitsPerSample);

  out.write("data", 4);
  WriteU32LE(out, dataSize);

  for (int s = 0; s < snapshot.numSamples; ++s)
  {
    for (int c = 0; c < numChannels; ++c)
    {
      const auto& channel = snapshot.channels[static_cast<size_t>(c)];
      const float value = s < static_cast<int>(channel.size()) ? channel[static_cast<size_t>(s)] : 0.f;
      WriteU16LE(out, static_cast<uint16_t>(FloatToPcm16(value)));
    }
  }

  if (!out)
  {
    error = "failed while writing WAV data";
    return false;
  }

  return true;
}

std::string Base64Encode(const std::vector<unsigned char>& data)
{
  static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2u) / 3u) * 4u);

  size_t i = 0;
  while (i + 2u < data.size())
  {
    const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16)
                          | (static_cast<uint32_t>(data[i + 1u]) << 8)
                          | static_cast<uint32_t>(data[i + 2u]);
    out.push_back(table[(triple >> 18) & 0x3f]);
    out.push_back(table[(triple >> 12) & 0x3f]);
    out.push_back(table[(triple >> 6) & 0x3f]);
    out.push_back(table[triple & 0x3f]);
    i += 3u;
  }

  if (i < data.size())
  {
    uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
    const bool hasSecondByte = (i + 1u) < data.size();
    if (hasSecondByte)
      triple |= static_cast<uint32_t>(data[i + 1u]) << 8;

    out.push_back(table[(triple >> 18) & 0x3f]);
    out.push_back(table[(triple >> 12) & 0x3f]);
    out.push_back(hasSecondByte ? table[(triple >> 6) & 0x3f] : '=');
    out.push_back('=');
  }

  return out;
}

int Base64Value(unsigned char c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>& bytes, std::string& error)
{
  bytes.clear();
  error.clear();

  int accumulator = 0;
  int bits = -8;
  bool sawPadding = false;

  for (unsigned char c : text)
  {
    if (c == '=')
    {
      sawPadding = true;
      continue;
    }

    if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
      continue;

    if (sawPadding)
    {
      error = "invalid base64 padding";
      return false;
    }

    const int value = Base64Value(c);
    if (value < 0)
    {
      error = "invalid base64 character";
      return false;
    }

    accumulator = (accumulator << 6) | value;
    bits += 6;
    if (bits >= 0)
    {
      bytes.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xff));
      bits -= 8;
    }
  }

  if (bytes.empty())
  {
    error = "base64 decoded to empty audio";
    return false;
  }

  return true;
}

bool HasChunkId(const unsigned char* data, const char* id)
{
  return std::memcmp(data, id, 4) == 0;
}

std::string ExtensionLower(const std::string& path)
{
  const size_t slash = path.find_last_of("\\/");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};

  std::string ext = path.substr(dot + 1u);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

std::string FileNameOnly(const std::string& path)
{
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1u);
}

void FreeDrMp3Float(float* data)
{
  drmp3_free(data, nullptr);
}

AudioFileInfo LoadMp3File(const std::string& path, RecordingSnapshot& snapshot)
{
  snapshot = {};

  AudioFileInfo info;
  info.path = path;
  info.bytes = FileSizeBytes(path);

  drmp3_config config = {};
  drmp3_uint64 totalFrames = 0;
  std::unique_ptr<float, decltype(&FreeDrMp3Float)> interleaved(
    drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &config, &totalFrames, nullptr),
    FreeDrMp3Float);

  if (!interleaved)
  {
    info.error = "could not decode MP3 file";
    return info;
  }

  if (config.channels == 0 || config.sampleRate == 0 || totalFrames == 0)
  {
    info.error = "MP3 file decoded to empty audio";
    return info;
  }

  if (config.channels > 32)
  {
    info.error = "MP3 has too many channels";
    return info;
  }

  if (totalFrames > static_cast<drmp3_uint64>(std::numeric_limits<int>::max()))
  {
    info.error = "MP3 is too long for the current spike loader";
    return info;
  }

  const int numChannels = static_cast<int>(config.channels);
  const int numSamples = static_cast<int>(totalFrames);
  snapshot.sampleRate = static_cast<int>(config.sampleRate);
  snapshot.numSamples = numSamples;
  snapshot.channels.assign(static_cast<size_t>(numChannels), std::vector<float>(static_cast<size_t>(numSamples), 0.f));

  const float* samples = interleaved.get();
  for (int s = 0; s < numSamples; ++s)
  {
    for (int c = 0; c < numChannels; ++c)
    {
      const float value = samples[static_cast<size_t>(s) * static_cast<size_t>(numChannels) + static_cast<size_t>(c)];
      snapshot.channels[static_cast<size_t>(c)][static_cast<size_t>(s)] = std::max(-1.f, std::min(1.f, value));
    }
  }

  info.ok = true;
  info.numSamples = snapshot.numSamples;
  info.sampleRate = snapshot.sampleRate;
  info.numChannels = static_cast<int>(snapshot.channels.size());
  return info;
}

bool CopyFileBytes(const std::string& sourcePath, const std::string& destinationPath, std::string& error)
{
  error.clear();

  const uint64_t sourceBytes = FileSizeBytes(sourcePath);
  if (sourceBytes < 1)
  {
    error = "source file is missing or empty";
    return false;
  }

  std::ifstream in(sourcePath, std::ios::binary);
  if (!in)
  {
    error = "could not open source file";
    return false;
  }

  std::ofstream out(destinationPath, std::ios::binary | std::ios::trunc);
  if (!out)
  {
    error = "could not create destination file";
    return false;
  }

  out << in.rdbuf();
  if (!out)
  {
    error = "failed while copying file";
    return false;
  }

  out.flush();
  if (!out)
  {
    error = "failed to flush copied file";
    return false;
  }

  in.close();
  out.close();

  const uint64_t copiedBytes = FileSizeBytes(destinationPath);
  if (copiedBytes != sourceBytes || copiedBytes < 1)
  {
    char message[160] = {};
    std::snprintf(message,
                  sizeof(message),
                  "copy verification failed (%llu of %llu bytes)",
                  static_cast<unsigned long long>(copiedBytes),
                  static_cast<unsigned long long>(sourceBytes));
    error = message;
    return false;
  }

  return true;
}

bool PrepareOutputUndoSnapshot(std::string& error)
{
  error.clear();

  std::string pathError;
  const std::string outputPath = OutputWavPath(&pathError);
  if (outputPath.empty())
  {
    error = pathError.empty() ? "output path unavailable" : pathError;
    return false;
  }

  const std::string undoPath = OutputUndoWavPath(&pathError);
  if (undoPath.empty())
  {
    error = pathError.empty() ? "undo path unavailable" : pathError;
    return false;
  }

  if (FileSizeBytes(outputPath) < 1)
  {
    std::remove(undoPath.c_str());
    return true;
  }

  if (!CopyFileBytes(outputPath, undoPath, error))
    return false;

  return true;
}
} // namespace

std::string DocumentsDirectory(std::string* error)
{
  std::string localError;
  const std::string documents = UserDocumentsPath(&localError);
  if (documents.empty())
  {
    if (error != nullptr)
      *error = localError.empty() ? "Documents folder unavailable" : localError;
    return {};
  }

  const std::string appFolder = JoinPath(documents, kAppFolderName);
  if (!EnsureDirectoryExists(appFolder, error))
    return {};

  return appFolder;
}

std::string RecordingWavPath(std::string* error)
{
  const std::string documents = DocumentsDirectory(error);
  return documents.empty() ? std::string() : JoinPath(documents, kRecordingFileName);
}

std::string OutputWavPath(std::string* error)
{
  const std::string documents = DocumentsDirectory(error);
  return documents.empty() ? std::string() : JoinPath(documents, kOutputFileName);
}

std::string OutputUndoWavPath(std::string* error)
{
  const std::string documents = DocumentsDirectory(error);
  return documents.empty() ? std::string() : JoinPath(documents, kOutputUndoFileName);
}

std::string LoraDirectory(std::string* error)
{
  const std::string documents = DocumentsDirectory(error);
  if (documents.empty())
    return {};

  const std::string loras = JoinPath(documents, kLoraFolderName);
  if (!EnsureDirectoryExists(loras, error))
    return {};

  return loras;
}

std::string Sa3CppDirectory()
{
  if (const char* path = std::getenv("SA3_CPP_DIR"))
    if (*path)
      return path;
  return SA3_DEMO_SA3_CPP_DIR;
}

std::string DraggedAudioDirectory(std::string* error = nullptr)
{
  const std::string documents = DocumentsDirectory(error);
  if (documents.empty())
    return {};

  const std::string draggedAudio = JoinPath(documents, kDraggedAudioFolderName);
  if (!EnsureDirectoryExists(draggedAudio, error))
    return {};

  return draggedAudio;
}

AudioFileInfo SaveRecordingWav(const RecordingSnapshot& snapshot)
{
  AudioFileInfo info;
  info.numSamples = snapshot.numSamples;
  info.sampleRate = snapshot.sampleRate;
  info.numChannels = static_cast<int>(snapshot.channels.size());

  std::string error;
  info.path = RecordingWavPath(&error);
  if (info.path.empty())
  {
    info.error = error.empty() ? "recording path unavailable" : error;
    return info;
  }

  if (!WritePcm16Wav(info.path, snapshot, error))
  {
    info.error = error;
    return info;
  }

  info.bytes = FileSizeBytes(info.path);
  info.ok = info.bytes > 0;
  if (!info.ok)
    info.error = "WAV write completed but output file is empty";

  return info;
}

AudioFileInfo SaveOutputWav(const RecordingSnapshot& snapshot)
{
  AudioFileInfo info;
  info.numSamples = snapshot.numSamples;
  info.sampleRate = snapshot.sampleRate;
  info.numChannels = static_cast<int>(snapshot.channels.size());

  std::string error;
  info.path = OutputWavPath(&error);
  if (info.path.empty())
  {
    info.error = error.empty() ? "output path unavailable" : error;
    return info;
  }

  if (!PrepareOutputUndoSnapshot(error))
  {
    info.error = std::string("undo snapshot failed: ") + error;
    return info;
  }

  if (!WritePcm16Wav(info.path, snapshot, error))
  {
    info.error = error;
    return info;
  }

  info.bytes = FileSizeBytes(info.path);
  info.ok = info.bytes > 0;
  if (!info.ok)
    info.error = "WAV write completed but output file is empty";

  return info;
}

AudioFileInfo SaveOutputBase64Audio(const std::string& base64Audio)
{
  AudioFileInfo info;

  std::string error;
  info.path = OutputWavPath(&error);
  if (info.path.empty())
  {
    info.error = error.empty() ? "output path unavailable" : error;
    return info;
  }

  std::vector<unsigned char> bytes;
  if (!Base64Decode(base64Audio, bytes, error))
  {
    info.error = error;
    return info;
  }

  if (!PrepareOutputUndoSnapshot(error))
  {
    info.error = std::string("undo snapshot failed: ") + error;
    return info;
  }

  std::remove(info.path.c_str());

  std::ofstream out(info.path, std::ios::binary | std::ios::trunc);
  if (!out)
  {
    info.error = "could not open output audio for writing";
    return info;
  }

  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out)
  {
    info.error = "failed while writing output audio";
    return info;
  }

  out.flush();
  if (!out)
  {
    info.error = "failed to flush output audio";
    return info;
  }
  out.close();

  info.bytes = FileSizeBytes(info.path);
  info.ok = info.bytes > 0;
  if (!info.ok)
    info.error = "output audio write completed but file is empty";

  return info;
}

AudioFileInfo ImportLoraFile(const std::string& path)
{
  AudioFileInfo info;
  info.path = path;
  info.bytes = FileSizeBytes(path);

  const std::string ext = ExtensionLower(path);
  if (ext != "gguf" && ext != "safetensors" && ext != "ckpt")
  {
    info.error = "import a gguf, safetensors, or ckpt LoRA";
    return info;
  }
  if (info.bytes < 1)
  {
    info.error = "LoRA file is missing or empty";
    return info;
  }

  std::string error;
  const std::string loraDir = LoraDirectory(&error);
  if (loraDir.empty())
  {
    info.error = error.empty() ? "LoRA folder unavailable" : error;
    return info;
  }

  const std::string displayName = NormalizeLoraName(path);
  info.name = displayName;
  info.prompts = LoadPromptPoolForLoraSource(path, displayName);

  if (ext == "gguf")
  {
    const std::string destination = JoinPath(loraDir, FileNameOnly(path).c_str());
#if defined(_WIN32)
    if (_stricmp(path.c_str(), destination.c_str()) == 0)
#else
    if (path == destination)
#endif
    {
      info.path = destination;
      info.ok = true;
      return info;
    }

    if (!CopyFileBytes(path, destination, error))
    {
      info.error = std::string("LoRA copy failed: ") + error;
      return info;
    }

    info.path = destination;
    info.bytes = FileSizeBytes(destination);
    info.ok = info.bytes > 0;
    if (!info.ok)
      info.error = "LoRA import completed but destination file is empty";
    return info;
  }

  const std::string exportedBase = FindExportedLoraBase(path, error);
  if (exportedBase.empty())
  {
    info.error = error;
    return info;
  }

  const std::string destination = JoinPath(loraDir, (displayName + ".gguf").c_str());
  if (!ConvertLoraToGguf(exportedBase, destination, error))
  {
    info.error = std::string("LoRA conversion failed: ") + error;
    return info;
  }

  info.path = destination;
  info.bytes = FileSizeBytes(destination);
  info.ok = info.bytes > 0;
  if (!info.ok)
    info.error = "LoRA import completed but destination file is empty";
  return info;
}

std::vector<std::string> LoadDefaultPromptPool()
{
  std::vector<std::string> prompts;
  AppendJsonArrayStrings(JoinPath(JoinPath(Sa3CppDirectory(), "prompts"), "defaults.json"), prompts);
  if (prompts.empty())
  {
    AddUniquePrompt(prompts, "warm analog synthwave with a hypnotic arpeggio");
    AddUniquePrompt(prompts, "dusty boom-bap beat with a soulful sample chop");
    AddUniquePrompt(prompts, "cinematic post-rock build with shimmering guitars");
  }
  return prompts;
}

bool OutputUndoAvailable()
{
  std::string error;
  const std::string undoPath = OutputUndoWavPath(&error);
  return !undoPath.empty() && FileSizeBytes(undoPath) > 0;
}

AudioFileInfo RestoreOutputUndo()
{
  AudioFileInfo info;

  std::string error;
  const std::string undoPath = OutputUndoWavPath(&error);
  if (undoPath.empty())
  {
    info.error = error.empty() ? "undo path unavailable" : error;
    return info;
  }

  info.path = OutputWavPath(&error);
  if (info.path.empty())
  {
    info.error = error.empty() ? "output path unavailable" : error;
    return info;
  }

  if (FileSizeBytes(undoPath) < 1)
  {
    info.error = "no previous output to undo";
    return info;
  }

  if (!CopyFileBytes(undoPath, info.path, error))
  {
    info.error = std::string("restore copy failed: ") + error;
    return info;
  }

  std::remove(undoPath.c_str());

  info.bytes = FileSizeBytes(info.path);
  info.ok = info.bytes > 0;
  if (!info.ok)
    info.error = "undo restored an empty output file";

  return info;
}

AudioFileInfo CreateOutputDragCopy()
{
  AudioFileInfo info;

  std::string error;
  const std::string sourcePath = OutputWavPath(&error);
  if (sourcePath.empty())
  {
    info.error = error.empty() ? "output path unavailable" : error;
    return info;
  }

  const uint64_t sourceBytes = FileSizeBytes(sourcePath);
  if (sourceBytes < 1)
  {
    info.path = sourcePath;
    info.error = "myOutput.wav is missing or empty";
    return info;
  }

  const std::string draggedAudio = DraggedAudioDirectory(&error);
  if (draggedAudio.empty())
  {
    info.error = error.empty() ? "drag folder unavailable" : error;
    return info;
  }

  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  const std::string fileName = std::string("sa3-iplug2-demo_") + std::to_string(millis) + ".wav";
  info.path = JoinPath(draggedAudio, fileName.c_str());

  if (!CopyFileBytes(sourcePath, info.path, error))
  {
    char message[160] = {};
    std::snprintf(message,
                  sizeof(message),
                  "drag copy verification failed: %s",
                  error.c_str());
    info.error = message;
    info.bytes = FileSizeBytes(info.path);
    return info;
  }

  info.bytes = FileSizeBytes(info.path);
  info.ok = info.bytes == sourceBytes && info.bytes > 0;
  return info;
}

AudioFileInfo LoadAudioFile(const std::string& path, RecordingSnapshot& snapshot)
{
  const std::string ext = ExtensionLower(path);
  if (ext == "mp3")
    return LoadMp3File(path, snapshot);

  if (ext == "wav" || ext == "wave")
    return LoadWavFile(path, snapshot);

  snapshot = {};
  AudioFileInfo info;
  info.path = path;
  info.bytes = FileSizeBytes(path);
  info.error = "unsupported audio file type";
  return info;
}

AudioFileInfo LoadWavFile(const std::string& path, RecordingSnapshot& snapshot)
{
  snapshot = {};

  AudioFileInfo info;
  info.path = path;
  info.bytes = FileSizeBytes(path);

  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    info.error = "could not open WAV file";
    return info;
  }

  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < 44)
  {
    info.error = "file is too small to be a WAV";
    return info;
  }

  if (!HasChunkId(bytes.data(), "RIFF") || !HasChunkId(bytes.data() + 8, "WAVE"))
  {
    info.error = "only RIFF/WAVE files are supported in this spike";
    return info;
  }

  bool haveFormat = false;
  bool haveData = false;
  uint16_t formatTag = 0;
  uint16_t numChannels = 0;
  uint32_t sampleRate = 0;
  uint16_t blockAlign = 0;
  uint16_t bitsPerSample = 0;
  size_t dataOffset = 0;
  uint32_t dataBytes = 0;

  size_t offset = 12;
  while (offset + 8 <= bytes.size())
  {
    const unsigned char* chunk = bytes.data() + offset;
    const uint32_t chunkSize = ReadU32LE(chunk + 4);
    const size_t chunkDataOffset = offset + 8;
    const size_t nextOffset = chunkDataOffset + chunkSize + (chunkSize & 1u);

    if (chunkDataOffset + chunkSize > bytes.size())
      break;

    if (HasChunkId(chunk, "fmt "))
    {
      if (chunkSize < 16)
      {
        info.error = "WAV fmt chunk is too small";
        return info;
      }

      const unsigned char* fmt = bytes.data() + chunkDataOffset;
      formatTag = ReadU16LE(fmt);
      numChannels = ReadU16LE(fmt + 2);
      sampleRate = ReadU32LE(fmt + 4);
      blockAlign = ReadU16LE(fmt + 12);
      bitsPerSample = ReadU16LE(fmt + 14);

      if (formatTag == 0xfffe && chunkSize >= 40)
      {
        const uint16_t subFormatTag = ReadU16LE(fmt + 24);
        if (subFormatTag == 1 || subFormatTag == 3)
          formatTag = subFormatTag;
      }

      haveFormat = true;
    }
    else if (HasChunkId(chunk, "data"))
    {
      dataOffset = chunkDataOffset;
      dataBytes = chunkSize;
      haveData = true;
    }

    offset = nextOffset;
  }

  if (!haveFormat || !haveData)
  {
    info.error = "WAV file is missing fmt or data chunk";
    return info;
  }

  if (numChannels == 0 || sampleRate == 0 || blockAlign == 0 || bitsPerSample == 0)
  {
    info.error = "WAV format is invalid";
    return info;
  }

  if (formatTag != 1 && formatTag != 3)
  {
    info.error = "unsupported WAV encoding";
    return info;
  }

  const int bytesPerSample = bitsPerSample / 8;
  if (bytesPerSample <= 0 || (formatTag == 1 && bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32))
  {
    info.error = "unsupported PCM bit depth";
    return info;
  }

  if (formatTag == 3 && bitsPerSample != 32 && bitsPerSample != 64)
  {
    info.error = "unsupported float WAV bit depth";
    return info;
  }

  if (blockAlign < numChannels * bytesPerSample)
  {
    info.error = "WAV block alignment is invalid";
    return info;
  }

  const int numSamples = static_cast<int>(dataBytes / blockAlign);
  if (numSamples <= 0)
  {
    info.error = "WAV data chunk is empty";
    return info;
  }

  snapshot.sampleRate = static_cast<int>(sampleRate);
  snapshot.numSamples = numSamples;
  snapshot.channels.assign(numChannels, std::vector<float>(static_cast<size_t>(numSamples), 0.f));

  const unsigned char* data = bytes.data() + dataOffset;
  for (int s = 0; s < numSamples; ++s)
  {
    const unsigned char* frame = data + static_cast<size_t>(s) * blockAlign;
    for (int c = 0; c < numChannels; ++c)
    {
      const unsigned char* sample = frame + static_cast<size_t>(c) * bytesPerSample;
      float value = 0.f;
      if (formatTag == 1)
        value = PcmToFloat(sample, bitsPerSample);
      else if (bitsPerSample == 32)
        value = Float32FromLE(sample);
      else
        value = static_cast<float>(Float64FromLE(sample));

      snapshot.channels[static_cast<size_t>(c)][static_cast<size_t>(s)] = std::max(-1.f, std::min(1.f, value));
    }
  }

  info.ok = true;
  info.numSamples = snapshot.numSamples;
  info.sampleRate = snapshot.sampleRate;
  info.numChannels = static_cast<int>(snapshot.channels.size());
  return info;
}

RecordingSnapshot ResampleSnapshotLinear(const RecordingSnapshot& source, int targetSampleRate)
{
  if (targetSampleRate <= 0 || source.sampleRate <= 0 || source.numSamples <= 0 || source.channels.empty()
      || std::abs(source.sampleRate - targetSampleRate) <= 1)
  {
    return source;
  }

  RecordingSnapshot result;
  result.sampleRate = targetSampleRate;

  const double ratio = static_cast<double>(targetSampleRate) / static_cast<double>(source.sampleRate);
  result.numSamples = std::max(1, static_cast<int>(std::llround(static_cast<double>(source.numSamples) * ratio)));
  result.channels.assign(source.channels.size(), std::vector<float>(static_cast<size_t>(result.numSamples), 0.f));

  for (size_t c = 0; c < source.channels.size(); ++c)
  {
    const auto& input = source.channels[c];
    auto& output = result.channels[c];
    if (input.empty())
      continue;

    for (int s = 0; s < result.numSamples; ++s)
    {
      const double sourcePosition = static_cast<double>(s) / ratio;
      const int index = static_cast<int>(sourcePosition);
      const double fraction = sourcePosition - index;
      const float a = input[static_cast<size_t>(std::min(index, static_cast<int>(input.size()) - 1))];
      const float b = input[static_cast<size_t>(std::min(index + 1, static_cast<int>(input.size()) - 1))];
      output[static_cast<size_t>(s)] = static_cast<float>(a + (b - a) * fraction);
    }
  }

  return result;
}

bool ReadFileBase64(const std::string& path, std::string& base64, std::string& error)
{
  base64.clear();
  error.clear();

  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    error = "could not open file for base64 encoding";
    return false;
  }

  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.empty())
  {
    error = "file is empty";
    return false;
  }

  base64 = Base64Encode(bytes);
  return true;
}

uint64_t FileSizeBytes(const std::string& path)
{
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return 0;

  const std::streamoff size = in.tellg();
  return size > 0 ? static_cast<uint64_t>(size) : 0;
}

// =====================================================================================================
// v0.3.0 in-plugin model management: settings, model plan/validation, and curl-based downloading.
// =====================================================================================================

namespace fs = std::filesystem;

namespace
{
std::string SettingsPath()
{
  const std::string documents = DocumentsDirectory();
  return documents.empty() ? std::string() : JoinPath(documents, "settings.txt");
}

std::string GlobOneInDir(const std::string& dir, const std::string& prefix, const std::string& suffix)
{
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return {};
  for (const auto& entry : fs::directory_iterator(dir, ec))
  {
    const std::string n = entry.path().filename().string();
    if (n.size() >= prefix.size() + suffix.size()
        && n.compare(0, prefix.size(), prefix) == 0
        && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
      return entry.path().string();
  }
  return {};
}

bool VariantIsSmall(const std::string& variant)
{
  return variant == "small-music" || variant == "small-sfx";
}

#if defined(_WIN32)
std::string QuoteWinArg(const std::string& arg)
{
  if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos)
    return arg;   // no quoting needed
  std::string out = "\"";
  size_t backslashes = 0;
  for (char c : arg)
  {
    if (c == '\\')
    {
      ++backslashes;
      out.push_back(c);
    }
    else if (c == '"')
    {
      out.append(backslashes + 1, '\\');   // escape the run of backslashes + the quote
      out.push_back('"');
      backslashes = 0;
    }
    else
    {
      backslashes = 0;
      out.push_back(c);
    }
  }
  out.append(backslashes, '\\');   // double trailing backslashes before the closing quote
  out.push_back('"');
  return out;
}
#endif
} // namespace

std::string LoadSetting(const std::string& key)
{
  const std::string path = SettingsPath();
  if (path.empty())
    return {};
  std::ifstream in(path);
  if (!in)
    return {};
  std::string line;
  while (std::getline(in, line))
  {
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    if (line.compare(0, eq, key) == 0)
    {
      std::string value = line.substr(eq + 1);
      if (!value.empty() && value.back() == '\r')
        value.pop_back();
      return value;
    }
  }
  return {};
}

bool SaveSetting(const std::string& key, const std::string& value)
{
  const std::string path = SettingsPath();
  if (path.empty())
    return false;

  std::vector<std::pair<std::string, std::string>> entries;
  bool replaced = false;
  {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const size_t eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      const std::string k = line.substr(0, eq);
      if (k == key)
      {
        entries.emplace_back(k, value);
        replaced = true;
      }
      else
      {
        entries.emplace_back(k, line.substr(eq + 1));
      }
    }
  }
  if (!replaced)
    entries.emplace_back(key, value);

  std::ofstream out(path, std::ios::trunc);
  if (!out)
    return false;
  for (const auto& e : entries)
    out << e.first << '=' << e.second << '\n';
  return static_cast<bool>(out);
}

std::string DefaultModelsDirectory(std::string* error)
{
  const std::string documents = DocumentsDirectory(error);
  if (documents.empty())
    return {};
  const std::string models = JoinPath(documents, "models");
  if (!EnsureDirectoryExists(models, error))
    return {};
  return models;
}

std::vector<ModelDownloadItem> ModelPlan(const std::string& variant, const std::string& encoding)
{
  const std::string ENC = (encoding == "f32" || encoding == "F32") ? "F32" : "F16";
  const std::string ditSize = VariantIsSmall(variant) ? "0.5B" : "1.5B";
  const std::string same = VariantIsSmall(variant) ? "same-s" : "same-l";
  const std::string varRepo = "thepatch/stable-audio-3-" + variant + "-GGUF";
  const std::string shared = "thepatch/t5gemma-b-b-ul2-GGUF";
  const std::string base = "stable-audio-3-" + variant;

  std::vector<ModelDownloadItem> items;
  items.push_back({varRepo, base + "-dit-" + ditSize + "-v1.0-" + ENC + ".gguf",
                   "stable-audio-3-" + variant + "-dit-", "-" + ENC + ".gguf", "DiT"});
  items.push_back({varRepo, base + "-" + same + "-v1.0-" + ENC + ".gguf",
                   "stable-audio-3-" + variant + "-same-", "-" + ENC + ".gguf", "SAME"});
  items.push_back({varRepo, base + "-conditioner-v1.0-F32.gguf",
                   "stable-audio-3-" + variant + "-conditioner-", ".gguf", "conditioner"});
  items.push_back({shared, "t5gemma-b-b-ul2-encoder-0.3B-v1.0-F32.gguf",
                   "t5gemma-b-b-ul2-encoder-", ".gguf", "encoder"});
  items.push_back({shared, "t5gemma-b-b-ul2-v1.0-vocab.gguf",
                   "t5gemma-b-b-ul2-v1.0-vocab", ".gguf", "tokenizer"});
  return items;
}

bool ModelSetComplete(const std::string& dir, const std::string& variant, const std::string& encoding,
                      std::vector<std::string>& missing)
{
  missing.clear();
  if (dir.empty())
  {
    missing.push_back("all");
    return false;
  }
  for (const auto& item : ModelPlan(variant, encoding))
    if (GlobOneInDir(dir, item.globPrefix, item.globSuffix).empty())
      missing.push_back(item.what);
  return missing.empty();
}

std::string HuggingFaceResolveUrl(const std::string& repo, const std::string& filename)
{
  return "https://huggingface.co/" + repo + "/resolve/main/" + filename;
}

AsyncProcess StartProcess(const std::vector<std::string>& argv, std::string& error)
{
  AsyncProcess proc;
  if (argv.empty())
  {
    error = "empty command";
    return proc;
  }
#if defined(_WIN32)
  std::string cmdLine;
  for (size_t i = 0; i < argv.size(); ++i)
  {
    if (i)
      cmdLine.push_back(' ');
    cmdLine += QuoteWinArg(argv[i]);
  }
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION info = {};
  std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
  mutableCmd.push_back('\0');
  const BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
  if (!ok)
  {
    error = "failed to launch process, win32 error " + std::to_string(GetLastError());
    return proc;
  }
  CloseHandle(info.hThread);
  proc.native = reinterpret_cast<void*>(info.hProcess);
  proc.valid = true;
  return proc;
#else
  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& a : argv)
    cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), environ);
  if (rc != 0)
  {
    error = std::string("failed to launch process: ") + std::strerror(rc);
    return proc;
  }
  proc.native = reinterpret_cast<void*>(static_cast<intptr_t>(pid) + 1);
  proc.valid = true;
  return proc;
#endif
}

bool ProcessTryWait(AsyncProcess& proc, int* exitCode)
{
  if (!proc.valid)
  {
    if (exitCode)
      *exitCode = -1;
    return true;
  }
#if defined(_WIN32)
  HANDLE handle = reinterpret_cast<HANDLE>(proc.native);
  const DWORD wait = WaitForSingleObject(handle, 0);
  if (wait != WAIT_OBJECT_0)
    return false;
  DWORD code = 1;
  GetExitCodeProcess(handle, &code);
  if (exitCode)
    *exitCode = static_cast<int>(code);
  return true;
#else
  pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(proc.native) - 1);
  int status = 0;
  const pid_t r = waitpid(pid, &status, WNOHANG);
  if (r == 0)
    return false;
  if (exitCode)
    *exitCode = (r > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
  return true;
#endif
}

void ProcessTerminate(AsyncProcess& proc)
{
  if (!proc.valid)
    return;
#if defined(_WIN32)
  TerminateProcess(reinterpret_cast<HANDLE>(proc.native), 1);
#else
  pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(proc.native) - 1);
  kill(pid, SIGTERM);
#endif
}

void ProcessClose(AsyncProcess& proc)
{
  if (!proc.valid)
    return;
#if defined(_WIN32)
  CloseHandle(reinterpret_cast<HANDLE>(proc.native));
#else
  pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(proc.native) - 1);
  int status = 0;
  waitpid(pid, &status, 0);   // reap so it doesn't linger as a zombie
#endif
  proc.native = nullptr;
  proc.valid = false;
}

long long HttpContentLength(const std::string& url)
{
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec)
                     / ("sa3dl_head_" + std::to_string(std::hash<std::string>{}(url)) + ".txt");
  const std::string tmpPath = tmp.string();

  std::string error;
  AsyncProcess proc = StartProcess({"curl", "-sIL", "-o", tmpPath, url}, error);
  if (!proc.valid)
    return -1;
  int code = 1;
  while (!ProcessTryWait(proc, &code))
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  ProcessClose(proc);
  if (code != 0)
  {
    std::remove(tmpPath.c_str());
    return -1;
  }

  long long contentLength = -1;
  {
    std::ifstream in(tmpPath, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
      std::string lower = line;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const std::string tag = "content-length:";
      if (lower.compare(0, tag.size(), tag) == 0)
      {
        std::string digits;
        for (char c : line.substr(tag.size()))
          if (std::isdigit(static_cast<unsigned char>(c)))
            digits.push_back(c);
        if (!digits.empty())
        {
          const long long v = std::strtoll(digits.c_str(), nullptr, 10);
          if (v > 0)
            contentLength = v;   // keep the LAST positive value (final hop after -L redirects)
        }
      }
    }
  }
  std::remove(tmpPath.c_str());
  return contentLength;
}

} // namespace gary

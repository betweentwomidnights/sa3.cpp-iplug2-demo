#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace gary
{

struct RecordingSnapshot
{
  std::vector<std::vector<float>> channels;
  int numSamples = 0;
  int sampleRate = 44100;
};

struct AudioFileInfo
{
  bool ok = false;
  std::string path;
  std::string name;
  std::string error;
  std::vector<std::string> prompts;
  uint64_t bytes = 0;
  int numSamples = 0;
  int sampleRate = 44100;
  int numChannels = 0;
};

std::string DocumentsDirectory(std::string* error = nullptr);
std::string RecordingWavPath(std::string* error = nullptr);
std::string OutputWavPath(std::string* error = nullptr);
std::string OutputUndoWavPath(std::string* error = nullptr);
std::string LoraDirectory(std::string* error = nullptr);
std::string Sa3CppDirectory();

// --- Small persisted settings (Documents/sa3-iplug2-demo/settings.txt, "key=value" lines) -----------
std::string LoadSetting(const std::string& key);                       // "" when absent
bool SaveSetting(const std::string& key, const std::string& value);    // upsert one key

// Creative LoRAs are stored per generation-model family. Legacy creative_lora_* keys are
// treated as the medium registry so existing installs migrate without losing adapters.
struct PersistedCreativeLora
{
  std::string path;
  float strength = 0.f;   // 0 leaves the LoRA out of a render
};

std::vector<PersistedCreativeLora> LoadCreativeLoraRegistry(const std::string& variant);
bool SaveCreativeLoraRegistry(const std::string& variant,
                              const std::vector<PersistedCreativeLora>& loras);

std::string DefaultModelsDirectory(std::string* error = nullptr);      // Documents/sa3-iplug2-demo/models

// --- Model set: the five gguf files a generation needs, per variant/encoding ------------------------
// One member of the download plan. `filename` is the exact file to fetch; globPrefix/globSuffix let us
// validate an arbitrary user folder while tolerating size-label / minor naming differences.
struct ModelDownloadItem
{
  std::string repo;         // hf repo, e.g. "thepatch/stable-audio-3-medium-GGUF"
  std::string filename;     // exact file to download
  std::string globPrefix;   // for validating a folder
  std::string globSuffix;
  const char* what = "";    // short human label ("DiT", "SAME", ...)
};

// variant in {medium, small-music}; encoding in {f16, f32}. Returns the 5-file plan (dit, same,
// conditioner, shared encoder, shared vocab).
std::vector<ModelDownloadItem> ModelPlan(const std::string& variant, const std::string& encoding);

// True if `dir` holds a complete set for variant/encoding (globs by prefix+suffix). Fills `missing`
// with the `what` labels of any files not found.
bool ModelSetComplete(const std::string& dir, const std::string& variant, const std::string& encoding,
                      std::vector<std::string>& missing);

std::string HuggingFaceResolveUrl(const std::string& repo, const std::string& filename);

// --- curl-based download primitives (bundled curl on Win10+/macOS) ----------------------------------
long long HttpContentLength(const std::string& url);   // -1 on failure

// Async child-process control so the resumable curl download can report progress by polling the
// partial file size. `native` holds an OS handle/pid; treat as opaque.
struct AsyncProcess
{
  void* native = nullptr;
  bool  valid  = false;
};
AsyncProcess StartProcess(const std::vector<std::string>& argv, std::string& error);
bool ProcessTryWait(AsyncProcess& proc, int* exitCode);   // true once exited (sets exitCode)
void ProcessTerminate(AsyncProcess& proc);                // kill (cancel)
void ProcessClose(AsyncProcess& proc);                    // release handle

std::string HumanBytes(long long bytes);

struct ModelDownloadResult
{
  bool complete = false;   // every file of the set is in place
  std::string message;     // the final status to show, whatever the outcome
};

// Fetches the variant's F16 set into destDir, blocking until it ends, so call it off the UI and audio
// threads. Resumes partial files and skips finished ones. `report` receives a status line and 0..1 progress.
ModelDownloadResult DownloadModelSet(const std::string& destDir, const std::string& variant,
                                     const std::atomic<bool>& cancel,
                                     const std::function<void(const std::string&, float)>& report);

AudioFileInfo SaveRecordingWav(const RecordingSnapshot& snapshot);
AudioFileInfo SaveOutputWav(const RecordingSnapshot& snapshot);
AudioFileInfo SaveWavFile(const std::string& path, const RecordingSnapshot& snapshot);
AudioFileInfo SaveOutputBase64Audio(const std::string& base64Audio);
AudioFileInfo ImportLoraFile(const std::string& path);
std::vector<std::string> LoadPromptPoolForLora(const std::string& sourcePath,
                                               const std::string& displayName = {});
std::vector<std::string> LoadDefaultPromptPool();
bool OutputUndoAvailable();
AudioFileInfo RestoreOutputUndo();
AudioFileInfo CreateOutputDragCopy();
AudioFileInfo LoadAudioFile(const std::string& path, RecordingSnapshot& snapshot);
AudioFileInfo LoadWavFile(const std::string& path, RecordingSnapshot& snapshot);
RecordingSnapshot ResampleSnapshotLinear(const RecordingSnapshot& source, int targetSampleRate);
bool ReadFileBase64(const std::string& path, std::string& base64, std::string& error);
uint64_t FileSizeBytes(const std::string& path);

// Paths here are UTF-8 on every platform, as they are for iPlug2, REAPER and libsa3. On Windows a narrow
// stream, std::remove or std::filesystem::path reads them in the system code page instead, so anything
// outside ASCII opens the wrong file or none. Convert at the point of use with these.
std::filesystem::path PathFromUtf8(const std::string& path);
std::string PathToUtf8(const std::filesystem::path& path);
void RemoveFile(const std::string& path);
std::string EnvironmentPath(const char* name);   // empty when unset

} // namespace gary

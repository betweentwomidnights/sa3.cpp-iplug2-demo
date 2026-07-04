#pragma once

#include <cstdint>
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

AudioFileInfo SaveRecordingWav(const RecordingSnapshot& snapshot);
AudioFileInfo SaveOutputWav(const RecordingSnapshot& snapshot);
AudioFileInfo SaveOutputBase64Audio(const std::string& base64Audio);
AudioFileInfo ImportLoraFile(const std::string& path);
std::vector<std::string> LoadDefaultPromptPool();
bool OutputUndoAvailable();
AudioFileInfo RestoreOutputUndo();
AudioFileInfo CreateOutputDragCopy();
AudioFileInfo LoadAudioFile(const std::string& path, RecordingSnapshot& snapshot);
AudioFileInfo LoadWavFile(const std::string& path, RecordingSnapshot& snapshot);
RecordingSnapshot ResampleSnapshotLinear(const RecordingSnapshot& source, int targetSampleRate);
bool ReadFileBase64(const std::string& path, std::string& base64, std::string& error);
uint64_t FileSizeBytes(const std::string& path);

} // namespace gary

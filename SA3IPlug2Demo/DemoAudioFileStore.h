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
  std::string error;
  uint64_t bytes = 0;
  int numSamples = 0;
  int sampleRate = 44100;
  int numChannels = 0;
};

std::string DocumentsDirectory(std::string* error = nullptr);
std::string RecordingWavPath(std::string* error = nullptr);
std::string OutputWavPath(std::string* error = nullptr);
std::string OutputUndoWavPath(std::string* error = nullptr);

AudioFileInfo SaveRecordingWav(const RecordingSnapshot& snapshot);
AudioFileInfo SaveOutputWav(const RecordingSnapshot& snapshot);
AudioFileInfo SaveOutputBase64Audio(const std::string& base64Audio);
bool OutputUndoAvailable();
AudioFileInfo RestoreOutputUndo();
AudioFileInfo CreateOutputDragCopy();
AudioFileInfo LoadAudioFile(const std::string& path, RecordingSnapshot& snapshot);
AudioFileInfo LoadWavFile(const std::string& path, RecordingSnapshot& snapshot);
RecordingSnapshot ResampleSnapshotLinear(const RecordingSnapshot& source, int targetSampleRate);
bool ReadFileBase64(const std::string& path, std::string& base64, std::string& error);
uint64_t FileSizeBytes(const std::string& path);

} // namespace gary

#pragma once

#include <string>

namespace gary
{

// Seconds of schedule headroom past the end of the audio. With none, SA3 plans the ending where the
// audio stops, so you hear it land. With headroom it composes past the end and those seconds are
// thrown away, so the audio cuts off still in full swing, at the cost of rendering them.
inline constexpr float kSA3EndsHereTailSeconds = 0.f;
inline constexpr float kSA3KeepsGoingTailSeconds = 6.f;

inline constexpr const char* kSA3EndsHereHint = "Ends here: the audio lands on an ending.";
inline constexpr const char* kSA3KeepsGoingHint = "Keeps going: cuts off still in full swing.";

// 0 = none, 1..12 = C..B.
inline constexpr const char* kSA3KeyRootNames[13] = {"none", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// "" with no root chosen, else "C minor".
inline std::string KeyScaleText(int root, int mode)
{
  if (root < 1 || root > 12)
    return {};
  return std::string(kSA3KeyRootNames[root]) + (mode ? " minor" : " major");
}

inline std::string PromptWithKeyScale(std::string prompt, const std::string& keyScale)
{
  if (keyScale.empty())
    return prompt;
  if (!prompt.empty())
    prompt += ' ';
  return prompt + keyScale;
}

}

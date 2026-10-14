#pragma once

#include <functional>
#include <optional>
#include <span>
#include <stdint.h>

namespace ModernOS {
void registerMediaEngine();
// Result is zero or an errno. Only the broker can reach the PCM device.
using AudioSink = std::function<int(unsigned, std::span<const int16_t>)>;
void setAudioSink(AudioSink);
// The real device queue, in 48-kHz stereo frames. A missing device has no clock.
using AudioQueue = std::function<std::optional<uint64_t>()>;
void setAudioQueue(AudioQueue);
}

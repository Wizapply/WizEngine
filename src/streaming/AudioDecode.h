#pragma once

#include <string>

#include "audio/AudioClip.h"

// WAV 以外の音のファイル（.ogg .mp3 .flac .m4a .aac .opus .webm）を GStreamer の
// decodebin で復号する（audio::AudioClipCache::setFallbackDecoder に渡す）。
// filesrc ! decodebin ! audioconvert ! audioresample ! F32LE 2ch 48 kHz ! appsink を
// 最後まで回して集める。クリップの読み込みスレッドから呼ばれる（待ってよい）。
namespace wizengine {
namespace streaming {

bool decodeAudioFileGst(const std::string& path, audio::AudioClip& out, std::string& error);

}  // namespace streaming
}  // namespace wizengine

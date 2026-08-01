#pragma once

// FFmpeg-backed decoder for the game's Bink (.bik) intro videos.
//
// One instance decodes one file at a time. DecodeNextFrame pulls the next video
// frame as tightly packed RGBA and, along the way, drains any Bink audio
// packets it passes into an internal ring that the SDL audio callback consumes
// via ReadAudioSamples — so decode runs on the render thread while playback
// runs on the audio thread, with m_audioBuf guarded by m_audioMutex.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class BinkPlayer {
 public:
  ~BinkPlayer() { Close(); }

  bool Open(const std::string& path);
  void Close();

  // Decodes up to the next video frame, filling outRgba with
  // GetVideoWidth() * GetVideoHeight() * 4 bytes. Returns false at end of
  // stream or on a decode error.
  bool DecodeNextFrame(std::vector<uint8_t>& outRgba);

  // Drains up to maxSamples interleaved floats from the audio ring. Returns
  // how many were actually copied. Safe to call from the audio thread.
  size_t ReadAudioSamples(float* dst, size_t maxSamples);

  uint32_t GetVideoWidth() const { return m_videoWidth; }
  uint32_t GetVideoHeight() const { return m_videoHeight; }
  int GetFrameDurationMs() const { return m_frameDurationMs; }
  int64_t GetCurrentPtsMs() const { return m_currentPtsMs; }
  int GetAudioSampleRate() const { return m_audioSampleRate; }
  int GetAudioChannels() const { return m_audioChannels; }
  bool HasAudio() const { return m_hasAudio.load(); }

 private:
  AVFormatContext* m_fmtCtx = nullptr;
  AVCodecContext* m_videoCodecCtx = nullptr;
  AVFrame* m_videoFrame = nullptr;
  AVPacket* m_pkt = nullptr;
  SwsContext* m_sws = nullptr;
  int m_videoStream = -1;
  uint32_t m_videoWidth = 0;
  uint32_t m_videoHeight = 0;
  AVRational m_videoTimeBase = {1, 90000};
  int m_frameDurationMs = 33;
  int64_t m_currentPtsMs = 0;

  int m_audioStream = -1;
  AVCodecContext* m_audioCodecCtx = nullptr;
  AVFrame* m_audioFrame = nullptr;
  SwrContext* m_audioSwr = nullptr;
  int m_audioSampleRate = 0;
  int m_audioChannels = 0;
  std::atomic<bool> m_hasAudio{false};

  std::vector<float> m_audioBuf;
  std::mutex m_audioMutex;
};

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
}

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

class BinkPlayer {
 public:
  ~BinkPlayer() { Close(); }

  bool Open(const std::string& path) {
    Close();

    int ret = avformat_open_input(&m_fmtCtx, path.c_str(), nullptr, nullptr);
    if (ret < 0) return false;

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) { Close(); return false; }

    m_videoStream = -1;
    m_audioStream = -1;
    for (unsigned int i = 0; i < m_fmtCtx->nb_streams; ++i) {
      auto id = m_fmtCtx->streams[i]->codecpar->codec_id;
      if (id == AV_CODEC_ID_BINKVIDEO) {
        m_videoStream = static_cast<int>(i);
      } else if (id == AV_CODEC_ID_BINKAUDIO_DCT) {
        m_audioStream = static_cast<int>(i);
      }
    }
    if (m_videoStream < 0) { Close(); return false; }

    auto* vidPar = m_fmtCtx->streams[m_videoStream]->codecpar;
    const AVCodec* vidCodec = avcodec_find_decoder(vidPar->codec_id);
    if (!vidCodec) { Close(); return false; }

    m_videoCodecCtx = avcodec_alloc_context3(vidCodec);
    if (!m_videoCodecCtx) { Close(); return false; }

    ret = avcodec_parameters_to_context(m_videoCodecCtx, vidPar);
    if (ret < 0) { Close(); return false; }

    ret = avcodec_open2(m_videoCodecCtx, vidCodec, nullptr);
    if (ret < 0) { Close(); return false; }

    m_videoFrame = av_frame_alloc();
    if (!m_videoFrame) { Close(); return false; }

    m_videoWidth = static_cast<uint32_t>(m_videoCodecCtx->width);
    m_videoHeight = static_cast<uint32_t>(m_videoCodecCtx->height);
    m_videoTimeBase = m_fmtCtx->streams[m_videoStream]->time_base;

    auto frameRate = m_fmtCtx->streams[m_videoStream]->r_frame_rate;
    if (frameRate.num > 0 && frameRate.den > 0) {
      double fps = static_cast<double>(frameRate.num) / frameRate.den;
      if (fps > 31.0) fps = 30.0;
      m_frameDurationMs = static_cast<int>(1000.0 / fps);
      if (m_frameDurationMs < 1) m_frameDurationMs = 1;
    }

    if (m_audioStream >= 0) {
      auto* audPar = m_fmtCtx->streams[m_audioStream]->codecpar;
      const AVCodec* audCodec = avcodec_find_decoder(audPar->codec_id);
      if (audCodec) {
        m_audioCodecCtx = avcodec_alloc_context3(audCodec);
        if (m_audioCodecCtx) {
          ret = avcodec_parameters_to_context(m_audioCodecCtx, audPar);
          if (ret >= 0) {
            ret = avcodec_open2(m_audioCodecCtx, audCodec, nullptr);
            if (ret >= 0) {
              m_audioFrame = av_frame_alloc();
              m_audioSampleRate = m_audioCodecCtx->sample_rate;
              m_audioChannels = static_cast<int>(m_audioCodecCtx->ch_layout.nb_channels);

              AVChannelLayout chLayout;
              av_channel_layout_default(&chLayout, m_audioChannels);
              ret = swr_alloc_set_opts2(&m_audioSwr,
                  &chLayout, AV_SAMPLE_FMT_FLT, m_audioSampleRate,
                  &chLayout, AV_SAMPLE_FMT_FLTP, m_audioSampleRate,
                  0, nullptr);
              av_channel_layout_uninit(&chLayout);
              if (ret >= 0 && m_audioSwr) {
                swr_init(m_audioSwr);
              }

              m_hasAudio = true;
            }
          }
        }
      }
    }

    m_pkt = av_packet_alloc();
    if (!m_pkt) { Close(); return false; }
    return true;
  }

  void Close() {
    if (m_pkt) { av_packet_free(&m_pkt); }
    if (m_videoFrame) { av_frame_free(&m_videoFrame); }
    if (m_audioFrame) { av_frame_free(&m_audioFrame); }
    if (m_videoCodecCtx) { avcodec_free_context(&m_videoCodecCtx); }
    if (m_audioCodecCtx) { avcodec_free_context(&m_audioCodecCtx); }
    if (m_fmtCtx) { avformat_close_input(&m_fmtCtx); }
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_audioSwr) { swr_free(&m_audioSwr); }
    m_videoStream = -1;
    m_audioStream = -1;
    m_hasAudio = false;
    m_audioSampleRate = 0;
    m_audioChannels = 0;
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_audioBuf.clear();
  }

  bool DecodeNextFrame(std::vector<uint8_t>& outRgba) {
    if (!m_fmtCtx || !m_videoCodecCtx || !m_videoFrame || !m_pkt) return false;

    while (true) {
      int ret = av_read_frame(m_fmtCtx, m_pkt);
      if (ret < 0) return false;

      if (m_audioCodecCtx && m_audioFrame &&
          m_pkt->stream_index == m_audioStream) {
        ret = avcodec_send_packet(m_audioCodecCtx, m_pkt);
        av_packet_unref(m_pkt);
        if (ret >= 0) {
          while (true) {
            ret = avcodec_receive_frame(m_audioCodecCtx, m_audioFrame);
            if (ret == AVERROR(EAGAIN) || ret < 0) break;

            int samples = m_audioFrame->nb_samples;
            if (samples > 0) {
              if (m_audioSwr) {
                int outSamples = swr_get_out_samples(m_audioSwr, samples);
                std::vector<float> buf(outSamples * m_audioChannels);
                uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(buf.data())};
                int converted = swr_convert(m_audioSwr, dst, outSamples,
                    const_cast<const uint8_t**>(m_audioFrame->data), samples);
                if (converted > 0) {
                  std::lock_guard<std::mutex> lock(m_audioMutex);
                  m_audioBuf.insert(m_audioBuf.end(), buf.data(),
                                    buf.data() + converted * m_audioChannels);
                }
              } else {
                std::lock_guard<std::mutex> lock(m_audioMutex);
                const float* src = reinterpret_cast<const float*>(
                    m_audioFrame->data[0]);
                m_audioBuf.insert(m_audioBuf.end(), src,
                                  src + samples * m_audioChannels);
              }
            }
          }
        }
        continue;
      }

      if (m_pkt->stream_index == m_videoStream) {
        ret = avcodec_send_packet(m_videoCodecCtx, m_pkt);
        av_packet_unref(m_pkt);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(m_videoCodecCtx, m_videoFrame);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) return false;

        outRgba.resize(m_videoWidth * m_videoHeight * 4);

        if (!m_sws) {
          m_sws = sws_getContext(m_videoWidth, m_videoHeight,
                                  static_cast<AVPixelFormat>(m_videoFrame->format),
                                  m_videoWidth, m_videoHeight, AV_PIX_FMT_RGBA,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
        }

        if (m_sws) {
          uint8_t* dst[] = {outRgba.data(), nullptr, nullptr, nullptr};
          int dstStride[] = {static_cast<int>(m_videoWidth * 4), 0, 0, 0};
          sws_scale(m_sws, m_videoFrame->data, m_videoFrame->linesize, 0,
                    m_videoHeight, dst, dstStride);
        }
        m_currentPtsMs = m_videoFrame->pts >= 0
            ? static_cast<int64_t>(m_videoFrame->pts *
                av_q2d(m_videoTimeBase) * 1000.0)
            : m_currentPtsMs + m_frameDurationMs;
        return true;
      }

      av_packet_unref(m_pkt);
    }
  }

  size_t ReadAudioSamples(float* dst, size_t maxSamples) {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    size_t toCopy = std::min(maxSamples, m_audioBuf.size());
    if (toCopy > 0) {
      std::memcpy(dst, m_audioBuf.data(), toCopy * sizeof(float));
      m_audioBuf.erase(m_audioBuf.begin(), m_audioBuf.begin() + toCopy);
    }
    return toCopy;
  }

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

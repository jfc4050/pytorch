#include "caffe2/video/video_decoder.h"
#include "caffe2/core/logging.h"

#include <stdio.h>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace caffe2 {

VideoDecoder::VideoDecoder() {
  static bool gInitialized = false;
  static std::mutex gMutex;
  std::unique_lock<std::mutex> lock(gMutex);
  if (!gInitialized) {
    av_register_all();
    avcodec_register_all();
    avformat_network_init();
    gInitialized = true;
  }
}

void VideoDecoder::decodeLoop(
    const string& videoName,
    VideoIOContext& ioctx,
    const Params& params,
    std::vector<std::unique_ptr<DecodedFrame>>& sampledFrames) {
  PixelFormat pixFormat = params.pixelFormat_;

  AVFormatContext* inputContext = avformat_alloc_context();
  AVStream* videoStream_ = nullptr;
  AVCodecContext* videoCodecContext_ = nullptr;
  AVFrame* videoStreamFrame_ = nullptr;
  AVPacket packet;
  av_init_packet(&packet); // init packet
  SwsContext* scaleContext_ = nullptr;

  inputContext->pb = ioctx.get_avio();
  inputContext->flags |= AVFMT_FLAG_CUSTOM_IO;
  int ret = 0;

  // Determining the input format:
  int probeSz = 32 * 1024 + AVPROBE_PADDING_SIZE;
  DecodedFrame::AvDataPtr probe((uint8_t*)av_malloc(probeSz));

  memset(probe.get(), 0, probeSz);
  int len = ioctx.read(probe.get(), probeSz - AVPROBE_PADDING_SIZE);
  if (len < probeSz - AVPROBE_PADDING_SIZE) {
    LOG(ERROR) << "Insufficient data to determine video format";
  }

  // seek back to start of stream
  ioctx.seek(0, SEEK_SET);

  unique_ptr<AVProbeData> probeData(new AVProbeData());
  probeData->buf = probe.get();
  probeData->buf_size = len;
  probeData->filename = "";
  // Determine the input-format:
  inputContext->iformat = av_probe_input_format(probeData.get(), 1);

  ret = avformat_open_input(&inputContext, "", nullptr, nullptr);
  if (ret < 0) {
    LOG(ERROR) << "Unable to open stream " << ffmpegErrorStr(ret);
  }

  ret = avformat_find_stream_info(inputContext, nullptr);
  if (ret < 0) {
    LOG(ERROR) << "Unable to find stream info in " << videoName << " "
               << ffmpegErrorStr(ret);
  }

  // Decode the first video stream
  int videoStreamIndex_ = params.streamIndex_;
  if (videoStreamIndex_ == -1) {
    for (int i = 0; i < inputContext->nb_streams; i++) {
      auto stream = inputContext->streams[i];
      if (stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        videoStreamIndex_ = i;
        videoStream_ = stream;
        break;
      }
    }
  }

  if (videoStream_ == nullptr) {
    LOG(ERROR) << "Unable to find video stream in " << videoName << " "
               << ffmpegErrorStr(ret);
  }

  // Initialize codec
  videoCodecContext_ = videoStream_->codec;

  ret = avcodec_open2(
      videoCodecContext_,
      avcodec_find_decoder(videoCodecContext_->codec_id),
      nullptr);
  if (ret < 0) {
    LOG(ERROR) << "Cannot open video codec : "
               << videoCodecContext_->codec->name;
  }

  // Calcuate if we need to rescale the frames
  int outWidth = videoCodecContext_->width;
  int outHeight = videoCodecContext_->height;

  if (params.maxOutputDimension_ != -1) {
    if (videoCodecContext_->width > videoCodecContext_->height) {
      // dominant width
      if (params.maxOutputDimension_ < videoCodecContext_->width) {
        float ratio =
            (float)params.maxOutputDimension_ / videoCodecContext_->width;
        outWidth = params.maxOutputDimension_;
        outHeight = (int)round(videoCodecContext_->height * ratio);
      }
    } else {
      // dominant height
      if (params.maxOutputDimension_ < videoCodecContext_->height) {
        float ratio =
            (float)params.maxOutputDimension_ / videoCodecContext_->height;
        outWidth = (int)round(videoCodecContext_->width * ratio);
        outHeight = params.maxOutputDimension_;
      }
    }
  } else {
    outWidth = params.outputWidth_ == -1 ? videoCodecContext_->width
                                         : params.outputWidth_;
    outHeight = params.outputHeight_ == -1 ? videoCodecContext_->height
                                           : params.outputHeight_;
  }

  // Make sure that we have a valid format
  CAFFE_ENFORCE_NE(videoCodecContext_->pix_fmt, AV_PIX_FMT_NONE);

  // Create a scale context
  scaleContext_ = sws_getContext(
      videoCodecContext_->width,
      videoCodecContext_->height,
      videoCodecContext_->pix_fmt,
      outWidth,
      outHeight,
      pixFormat,
      SWS_FAST_BILINEAR,
      nullptr,
      nullptr,
      nullptr);

  // Getting video meta data
  VideoMeta videoMeta;
  videoMeta.codec_type = videoCodecContext_->codec_type;
  videoMeta.width = outWidth;
  videoMeta.height = outHeight;
  videoMeta.pixFormat = pixFormat;
  videoMeta.fps = av_q2d(videoStream_->avg_frame_rate);

  // If sampledFrames is not empty, empty it
  if (sampledFrames.size() > 0) {
    sampledFrames.clear();
  }

  if (params.intervals_.size() == 0) {
    LOG(ERROR) << "Empty sampling intervals.";
  }

  std::vector<SampleInterval>::const_iterator itvlIter =
      params.intervals_.begin();
  if (itvlIter->timestamp != 0) {
    LOG(ERROR) << "Sampling interval starting timestamp is not zero.";
  }

  double currFps = itvlIter->fps;
  if (currFps < 0 && currFps != SpecialFps::SAMPLE_ALL_FRAMES &&
      currFps != SpecialFps::SAMPLE_TIMESTAMP_ONLY) {
    // fps must be 0, -1, -2 or > 0
    LOG(ERROR) << "Invalid sampling fps.";
  }

  double prevTimestamp = itvlIter->timestamp;
  itvlIter++;
  if (itvlIter != params.intervals_.end() &&
      prevTimestamp >= itvlIter->timestamp) {
    LOG(ERROR) << "Sampling interval timestamps must be strictly ascending.";
  }

  double lastFrameTimestamp = -1.0;
  double timestamp = -1.0;

  // Initialize frame and packet.
  // These will be reused across calls.
  videoStreamFrame_ = av_frame_alloc();

  // frame index in video stream
  int frameIndex = -1;
  // frame index of outputed frames
  int outputFrameIndex = -1;

  int gotPicture = 0;
  int eof = 0;

  // There is a delay between reading packets from the
  // transport and getting decoded frames back.
  // Therefore, after EOF, continue going while
  // the decoder is still giving us frames.
  while (!eof || gotPicture) {
    if (!eof) {
      ret = av_read_frame(inputContext, &packet);

      if (ret == AVERROR(EAGAIN)) {
        continue;
      }
      // Interpret any other error as EOF
      if (ret < 0) {
        eof = 1;
        continue;
      }

      // Ignore packets from other streams
      if (packet.stream_index != videoStreamIndex_) {
        continue;
      }
    }

    ret = avcodec_decode_video2(
        videoCodecContext_, videoStreamFrame_, &gotPicture, &packet);
    if (ret < 0) {
      LOG(ERROR) << "Error decoding video frame : " << ffmpegErrorStr(ret);
    }

    // Nothing to do without a picture
    if (!gotPicture) {
      continue;
    }

    frameIndex++;

    timestamp = av_frame_get_best_effort_timestamp(videoStreamFrame_) *
        av_q2d(videoStream_->time_base);

    // if reaching the next interval, update the current fps
    // and reset lastFrameTimestamp so the current frame could be sampled
    // (unless fps == SpecialFps::SAMPLE_NO_FRAME)
    if (itvlIter != params.intervals_.end() &&
        timestamp >= itvlIter->timestamp) {
      lastFrameTimestamp = -1.0;
      currFps = itvlIter->fps;
      prevTimestamp = itvlIter->timestamp;
      itvlIter++;
      if (itvlIter != params.intervals_.end() &&
          prevTimestamp >= itvlIter->timestamp) {
        LOG(ERROR)
            << "Sampling interval timestamps must be strictly ascending.";
      }
    }

    // keyFrame will bypass all checks on fps sampling settings
    bool keyFrame = params.keyFrames_ && videoStreamFrame_->key_frame;
    if (!keyFrame) {
      // if fps == SpecialFps::SAMPLE_NO_FRAME (0), don't sample at all
      if (currFps == SpecialFps::SAMPLE_NO_FRAME) {
        continue;
      }

      // fps is considered reached in the following cases:
      // 1. lastFrameTimestamp < 0 - start of a new interval (or first frame)
      // 2. currFps == SpecialFps::SAMPLE_ALL_FRAMES (-1) - sample every frame
      // 3. timestamp - lastFrameTimestamp has reached target fps and
      //    currFps > 0 (not special fps setting)
      // different modes for fps:
      // SpecialFps::SAMPLE_NO_FRAMES (0):
      //     disable fps sampling, no frame sampled at all
      // SpecialFps::SAMPLE_ALL_FRAMES (-1):
      //     unlimited fps sampling, will sample at native video fps
      // SpecialFps::SAMPLE_TIMESTAMP_ONLY (-2):
      //     disable fps sampling, but will get the frame at specific timestamp
      // others (> 0): decoding at the specified fps
      bool fpsReached = lastFrameTimestamp < 0 ||
          currFps == SpecialFps::SAMPLE_ALL_FRAMES ||
          (currFps > 0 && timestamp >= lastFrameTimestamp + (1 / currFps));

      if (!fpsReached) {
        continue;
      }
    }

    lastFrameTimestamp = timestamp;

    outputFrameIndex++;
    if (params.maximumOutputFrames_ != -1 &&
        outputFrameIndex >= params.maximumOutputFrames_) {
      // enough frames
      break;
    }

    AVFrame* rgbFrame = av_frame_alloc();
    if (!rgbFrame) {
      LOG(ERROR) << "Error allocating AVframe";
    }

    // Determine required buffer size and allocate buffer
    int numBytes = avpicture_get_size(pixFormat, outWidth, outHeight);
    DecodedFrame::AvDataPtr buffer(
        (uint8_t*)av_malloc(numBytes * sizeof(uint8_t)));

    int size = avpicture_fill(
        (AVPicture*)rgbFrame, buffer.get(), pixFormat, outWidth, outHeight);

    sws_scale(
        scaleContext_,
        videoStreamFrame_->data,
        videoStreamFrame_->linesize,
        0,
        videoCodecContext_->height,
        rgbFrame->data,
        rgbFrame->linesize);

    unique_ptr<DecodedFrame> frame = make_unique<DecodedFrame>();
    frame->width_ = outWidth;
    frame->height_ = outHeight;
    frame->data_ = move(buffer);
    frame->size_ = size;
    frame->index_ = frameIndex;
    frame->outputFrameIndex_ = outputFrameIndex;
    frame->timestamp_ = timestamp;
    frame->keyFrame_ = videoStreamFrame_->key_frame;

    sampledFrames.push_back(move(frame));
    av_frame_free(&rgbFrame);
  }

  av_free_packet(&packet);
  av_frame_unref(videoStreamFrame_);
  sws_freeContext(scaleContext_);
  av_packet_unref(&packet);
  av_frame_free(&videoStreamFrame_);
  avcodec_close(videoCodecContext_);
  avformat_close_input(&inputContext);
  avformat_free_context(inputContext);
}

void VideoDecoder::decodeMemory(
    const char* buffer,
    const int size,
    const Params& params,
    std::vector<std::unique_ptr<DecodedFrame>>& sampledFrames) {
  VideoIOContext ioctx(buffer, size);
  decodeLoop(string("Memory Buffer"), ioctx, params, sampledFrames);
}

void VideoDecoder::decodeFile(
    const string file,
    const Params& params,
    std::vector<std::unique_ptr<DecodedFrame>>& sampledFrames) {
  VideoIOContext ioctx(file);
  decodeLoop(file, ioctx, params, sampledFrames);
}

string VideoDecoder::ffmpegErrorStr(int result) {
  std::array<char, 128> buf;
  av_strerror(result, buf.data(), buf.size());
  return string(buf.data());
}

} // namespace caffe2

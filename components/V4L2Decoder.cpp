// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "V4L2Decoder"

#include <v4l2_codec2/components/V4L2Decoder.h>

#include <stdint.h>

#include <algorithm>
#include <vector>

#include <base/bind.h>
#include <base/files/scoped_file.h>
#include <base/memory/ptr_util.h>
#include <log/log.h>

#include <v4l2_codec2/common/Common.h>
#include <v4l2_codec2/common/Fourcc.h>

namespace android {
namespace {

constexpr size_t kNumInputBuffers = 16;
// Extra buffers for transmitting in the whole video pipeline.
constexpr size_t kNumExtraOutputBuffers = 4;

// Currently we only support flexible pixel 420 format YCBCR_420_888 in Android.
// Here is the list of flexible 420 format.
constexpr std::initializer_list<uint32_t> kSupportedOutputFourccs = {
        Fourcc::YU12, Fourcc::YV12, Fourcc::YM12, Fourcc::YM21,
        Fourcc::NV12, Fourcc::NV21, Fourcc::NM12, Fourcc::NM21,
};

uint32_t VideoCodecToV4L2PixFmt(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return V4L2_PIX_FMT_H264;
    case VideoCodec::VP8:
        return V4L2_PIX_FMT_VP8;
    case VideoCodec::VP9:
        return V4L2_PIX_FMT_VP9;
    case VideoCodec::HEVC:
        return V4L2_PIX_FMT_HEVC;
    }
}

}  // namespace

// static
std::unique_ptr<VideoDecoder> V4L2Decoder::Create(
        const VideoCodec& codec, const size_t inputBufferSize, const size_t minNumOutputBuffers,
        GetPoolCB getPoolCb, OutputCB outputCb, ErrorCB errorCb,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner) {
    std::unique_ptr<V4L2Decoder> decoder =
            ::base::WrapUnique<V4L2Decoder>(new V4L2Decoder(taskRunner));
    if (!decoder->start(codec, inputBufferSize, minNumOutputBuffers, std::move(getPoolCb),
                        std::move(outputCb), std::move(errorCb))) {
        return nullptr;
    }
    return decoder;
}

V4L2Decoder::V4L2Decoder(scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mTaskRunner(std::move(taskRunner)) {
    ALOGV("%s()", __func__);

    mWeakThis = mWeakThisFactory.GetWeakPtr();
}

V4L2Decoder::~V4L2Decoder() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    mWeakThisFactory.InvalidateWeakPtrs();

    // Streamoff input and output queue.
    if (mOutputQueue) {
        mOutputQueue->streamoff();
        mOutputQueue->deallocateBuffers();
        mOutputQueue = nullptr;
    }
    if (mInputQueue) {
        mInputQueue->streamoff();
        mInputQueue->deallocateBuffers();
        mInputQueue = nullptr;
    }
    if (mDevice) {
        mDevice->stopPolling();
        mDevice = nullptr;
    }
}

bool V4L2Decoder::start(const VideoCodec& codec, const size_t inputBufferSize,
                        const size_t minNumOutputBuffers, GetPoolCB getPoolCb, OutputCB outputCb,
                        ErrorCB errorCb) {
    ALOGV("%s(codec=%s, inputBufferSize=%zu, minNumOutputBuffers=%zu)", __func__,
          VideoCodecToString(codec), inputBufferSize, minNumOutputBuffers);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    mMinNumOutputBuffers = minNumOutputBuffers;
    mGetPoolCb = std::move(getPoolCb);
    mOutputCb = std::move(outputCb);
    mErrorCb = std::move(errorCb);

    if (mState == State::Error) {
        ALOGE("Ignore due to error state.");
        return false;
    }

    mDevice = V4L2Device::create();

    const uint32_t inputPixelFormat = VideoCodecToV4L2PixFmt(codec);
    if (!mDevice->open(V4L2Device::Type::kDecoder, inputPixelFormat)) {
        ALOGE("Failed to open device for %s", VideoCodecToString(codec));
        return false;
    }

    if (!mDevice->hasCapabilities(V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) {
        ALOGE("Device does not have VIDEO_M2M_MPLANE and STREAMING capabilities.");
        return false;
    }

    struct v4l2_decoder_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = V4L2_DEC_CMD_STOP;
    if (mDevice->ioctl(VIDIOC_TRY_DECODER_CMD, &cmd) != 0) {
        ALOGE("Device does not support flushing (V4L2_DEC_CMD_STOP)");
        return false;
    }

    // Subscribe to the resolution change event.
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (mDevice->ioctl(VIDIOC_SUBSCRIBE_EVENT, &sub) != 0) {
        ALOGE("ioctl() failed: VIDIOC_SUBSCRIBE_EVENT: V4L2_EVENT_SOURCE_CHANGE");
        return false;
    }

    // Create Input/Output V4L2Queue, and setup input queue.
    mInputQueue = mDevice->getQueue(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    mOutputQueue = mDevice->getQueue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (!mInputQueue || !mOutputQueue) {
        ALOGE("Failed to create V4L2 queue.");
        return false;
    }
    if (!setupInputFormat(inputPixelFormat, inputBufferSize)) {
        ALOGE("Failed to setup input format.");
        return false;
    }

    if (!mDevice->startPolling(::base::BindRepeating(&V4L2Decoder::serviceDeviceTask, mWeakThis),
                               ::base::BindRepeating(&V4L2Decoder::onError, mWeakThis))) {
        ALOGE("Failed to start polling V4L2 device.");
        return false;
    }

    setState(State::Idle);
    return true;
}

bool V4L2Decoder::setupInputFormat(const uint32_t inputPixelFormat, const size_t inputBufferSize) {
    ALOGV("%s(inputPixelFormat=%u, inputBufferSize=%zu)", __func__, inputPixelFormat,
          inputBufferSize);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    // Check if the format is supported.
    std::vector<uint32_t> formats =
            mDevice->enumerateSupportedPixelformats(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (std::find(formats.begin(), formats.end(), inputPixelFormat) == formats.end()) {
        ALOGE("Input codec s not supported by device.");
        return false;
    }

    // Setup the input format.
    auto format = mInputQueue->setFormat(inputPixelFormat, ui::Size(), inputBufferSize, 0);
    if (!format) {
        ALOGE("Failed to call IOCTL to set input format.");
        return false;
    }
    ALOG_ASSERT(format->fmt.pix_mp.pixelformat == inputPixelFormat);

    if (mInputQueue->allocateBuffers(kNumInputBuffers, V4L2_MEMORY_DMABUF) == 0) {
        ALOGE("Failed to allocate input buffer.");
        return false;
    }
    if (!mInputQueue->streamon()) {
        ALOGE("Failed to streamon input queue.");
        return false;
    }
    return true;
}

void V4L2Decoder::decode(std::unique_ptr<ConstBitstreamBuffer> buffer, DecodeCB decodeCb) {
    ALOGV("%s(id=%d)", __func__, buffer->id);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Error) {
        ALOGE("Ignore due to error state.");
        mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(std::move(decodeCb),
                                                          VideoDecoder::DecodeStatus::kError));
        return;
    }

    if (mState == State::Idle) {
        setState(State::Decoding);
    }

    mDecodeRequests.push(DecodeRequest(std::move(buffer), std::move(decodeCb)));
    pumpDecodeRequest();
}

void V4L2Decoder::drain(DecodeCB drainCb) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    switch (mState) {
    case State::Idle:
        ALOGV("Nothing need to drain, ignore.");
        mTaskRunner->PostTask(
                FROM_HERE, ::base::BindOnce(std::move(drainCb), VideoDecoder::DecodeStatus::kOk));
        return;

    case State::Decoding:
        mDecodeRequests.push(DecodeRequest(nullptr, std::move(drainCb)));
        pumpDecodeRequest();
        return;

    case State::Draining:
    case State::Error:
        ALOGE("Ignore due to wrong state: %s", StateToString(mState));
        mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(std::move(drainCb),
                                                          VideoDecoder::DecodeStatus::kError));
        return;
    }
}

void V4L2Decoder::pumpDecodeRequest() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState != State::Decoding) return;

    while (!mDecodeRequests.empty()) {
        // Drain the decoder.
        if (mDecodeRequests.front().buffer == nullptr) {
            ALOGV("Get drain request.");
            // Send the flush command after all input buffers are dequeued. This makes
            // sure all previous resolution changes have been handled because the
            // driver must hold the input buffer that triggers resolution change. The
            // driver cannot decode data in it without new output buffers. If we send
            // the flush now and a queued input buffer triggers resolution change
            // later, the driver will send an output buffer that has
            // V4L2_BUF_FLAG_LAST. But some queued input buffer have not been decoded
            // yet. Also, V4L2VDA calls STREAMOFF and STREAMON after resolution
            // change. They implicitly send a V4L2_DEC_CMD_STOP and V4L2_DEC_CMD_START
            // to the decoder.
            if (mInputQueue->queuedBuffersCount() > 0) {
                ALOGV("Wait for all input buffers dequeued.");
                return;
            }

            auto request = std::move(mDecodeRequests.front());
            mDecodeRequests.pop();

            if (!sendV4L2DecoderCmd(false)) {
                std::move(request.decodeCb).Run(VideoDecoder::DecodeStatus::kError);
                onError();
                return;
            }
            mDrainCb = std::move(request.decodeCb);
            setState(State::Draining);
            return;
        }

        // Pause if no free input buffer. We resume decoding after dequeueing input buffers.
        auto inputBuffer = mInputQueue->getFreeBuffer();
        if (!inputBuffer) {
            ALOGV("There is no free input buffer.");
            return;
        }

        auto request = std::move(mDecodeRequests.front());
        mDecodeRequests.pop();

        const int32_t bitstreamId = request.buffer->id;
        ALOGV("QBUF to input queue, bitstreadId=%d", bitstreamId);
        inputBuffer->setTimeStamp({.tv_sec = bitstreamId});
        size_t planeSize = inputBuffer->getPlaneSize(0);
        if (request.buffer->size > planeSize) {
            ALOGE("The input size (%zu) is not enough, we need %zu", planeSize,
                  request.buffer->size);
            onError();
            return;
        }

        ALOGV("Set bytes_used=%zu, offset=%zu", request.buffer->offset + request.buffer->size,
              request.buffer->offset);
        inputBuffer->setPlaneDataOffset(0, request.buffer->offset);
        inputBuffer->setPlaneBytesUsed(0, request.buffer->offset + request.buffer->size);
        std::vector<int> fds;
        fds.push_back(std::move(request.buffer->dmabuf.handle()->data[0]));
        if (!std::move(*inputBuffer).queueDMABuf(fds)) {
            ALOGE("%s(): Failed to QBUF to input queue, bitstreamId=%d", __func__, bitstreamId);
            onError();
            return;
        }

        mPendingDecodeCbs.insert(std::make_pair(bitstreamId, std::move(request.decodeCb)));
    }
}

void V4L2Decoder::flush() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Idle) {
        ALOGV("Nothing need to flush, ignore.");
        return;
    }
    if (mState == State::Error) {
        ALOGE("Ignore due to error state.");
        return;
    }

    // Call all pending callbacks.
    for (auto& item : mPendingDecodeCbs) {
        std::move(item.second).Run(VideoDecoder::DecodeStatus::kAborted);
    }
    mPendingDecodeCbs.clear();
    if (mDrainCb) {
        std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kAborted);
    }

    // Streamoff both V4L2 queues to drop input and output buffers.
    const bool isOutputStreaming = mOutputQueue->isStreaming();
    mDevice->stopPolling();
    mOutputQueue->streamoff();
    mFrameAtDevice.clear();
    mInputQueue->streamoff();

    // Streamon both V4L2 queues.
    mInputQueue->streamon();
    if (isOutputStreaming) {
        mOutputQueue->streamon();
    }

    // If there is no free buffer at mOutputQueue, tryFetchVideoFrame() should be triggerred after
    // a buffer is DQBUF from output queue. Now all the buffers are dropped at mOutputQueue, we
    // have to trigger tryFetchVideoFrame() here.
    if (mVideoFramePool) {
        tryFetchVideoFrame();
    }

    if (!mDevice->startPolling(::base::BindRepeating(&V4L2Decoder::serviceDeviceTask, mWeakThis),
                               ::base::BindRepeating(&V4L2Decoder::onError, mWeakThis))) {
        ALOGE("Failed to start polling V4L2 device.");
        onError();
        return;
    }

    setState(State::Idle);
}

void V4L2Decoder::serviceDeviceTask(bool event) {
    ALOGV("%s(event=%d) state=%s InputQueue(%s):%zu+%zu/%zu, OutputQueue(%s):%zu+%zu/%zu", __func__,
          event, StateToString(mState), (mInputQueue->isStreaming() ? "streamon" : "streamoff"),
          mInputQueue->freeBuffersCount(), mInputQueue->queuedBuffersCount(),
          mInputQueue->allocatedBuffersCount(),
          (mOutputQueue->isStreaming() ? "streamon" : "streamoff"),
          mOutputQueue->freeBuffersCount(), mOutputQueue->queuedBuffersCount(),
          mOutputQueue->allocatedBuffersCount());
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Error) return;

    // Dequeue output and input queue.
    bool inputDequeued = false;
    while (mInputQueue->queuedBuffersCount() > 0) {
        bool success;
        V4L2ReadableBufferRef dequeuedBuffer;
        std::tie(success, dequeuedBuffer) = mInputQueue->dequeueBuffer();
        if (!success) {
            ALOGE("Failed to dequeue buffer from input queue.");
            onError();
            return;
        }
        if (!dequeuedBuffer) break;

        inputDequeued = true;

        // Run the corresponding decode callback.
        int32_t id = dequeuedBuffer->getTimeStamp().tv_sec;
        ALOGV("DQBUF from input queue, bitstreamId=%d", id);
        auto it = mPendingDecodeCbs.find(id);
        if (it == mPendingDecodeCbs.end()) {
            ALOGW("Callback is already abandoned.");
            continue;
        }
        std::move(it->second).Run(VideoDecoder::DecodeStatus::kOk);
        mPendingDecodeCbs.erase(it);
    }

    bool outputDequeued = false;
    while (mOutputQueue->queuedBuffersCount() > 0) {
        bool success;
        V4L2ReadableBufferRef dequeuedBuffer;
        std::tie(success, dequeuedBuffer) = mOutputQueue->dequeueBuffer();
        if (!success) {
            ALOGE("Failed to dequeue buffer from output queue.");
            onError();
            return;
        }
        if (!dequeuedBuffer) break;

        outputDequeued = true;

        const size_t bufferId = dequeuedBuffer->bufferId();
        const int32_t bitstreamId = static_cast<int32_t>(dequeuedBuffer->getTimeStamp().tv_sec);
        const size_t bytesUsed = dequeuedBuffer->getPlaneBytesUsed(0);
        const bool isLast = dequeuedBuffer->isLast();
        ALOGV("DQBUF from output queue, bufferId=%zu, bitstreamId=%d, bytesused=%zu, isLast=%d",
              bufferId, bitstreamId, bytesUsed, isLast);

        // Get the corresponding VideoFrame of the dequeued buffer.
        auto it = mFrameAtDevice.find(bufferId);
        ALOG_ASSERT(it != mFrameAtDevice.end(), "buffer %zu is not found at mFrameAtDevice",
                    bufferId);
        auto frame = std::move(it->second);
        mFrameAtDevice.erase(it);

        if (bytesUsed > 0) {
            ALOGV("Send output frame(bitstreamId=%d) to client", bitstreamId);
            frame->setBitstreamId(bitstreamId);
            frame->setVisibleRect(mVisibleRect);
            mOutputCb.Run(std::move(frame));
        } else {
            // Workaround(b/168750131): If the buffer is not enqueued before the next drain is done,
            // then the driver will fail to notify EOS. So we recycle the buffer immediately.
            ALOGV("Recycle empty buffer %zu back to V4L2 output queue.", bufferId);
            dequeuedBuffer.reset();
            auto outputBuffer = mOutputQueue->getFreeBuffer(bufferId);
            ALOG_ASSERT(outputBuffer, "V4L2 output queue slot %zu is not freed.", bufferId);

            if (!std::move(*outputBuffer).queueDMABuf(frame->getFDs())) {
                ALOGE("%s(): Failed to recycle empty buffer to output queue.", __func__);
                onError();
                return;
            }
            mFrameAtDevice.insert(std::make_pair(bufferId, std::move(frame)));
        }

        if (mDrainCb && isLast) {
            ALOGV("All buffers are drained.");
            sendV4L2DecoderCmd(true);
            std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kOk);
            setState(State::Idle);
        }
    }

    // Handle resolution change event.
    if (event && dequeueResolutionChangeEvent()) {
        if (!changeResolution()) {
            onError();
            return;
        }
    }

    // We freed some input buffers, continue handling decode requests.
    if (inputDequeued) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::BindOnce(&V4L2Decoder::pumpDecodeRequest, mWeakThis));
    }
    // We free some output buffers, try to get VideoFrame.
    if (outputDequeued) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::BindOnce(&V4L2Decoder::tryFetchVideoFrame, mWeakThis));
    }
}

bool V4L2Decoder::dequeueResolutionChangeEvent() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_event ev;
    memset(&ev, 0, sizeof(ev));
    while (mDevice->ioctl(VIDIOC_DQEVENT, &ev) == 0) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE &&
            ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION) {
            return true;
        }
    }
    return false;
}

bool V4L2Decoder::changeResolution() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    const std::optional<struct v4l2_format> format = getFormatInfo();
    std::optional<size_t> numOutputBuffers = getNumOutputBuffers();
    if (!format || !numOutputBuffers) {
        return false;
    }
    *numOutputBuffers = std::max(*numOutputBuffers, mMinNumOutputBuffers);

    const ui::Size codedSize(format->fmt.pix_mp.width, format->fmt.pix_mp.height);
    if (!setupOutputFormat(codedSize)) {
        return false;
    }

    const std::optional<struct v4l2_format> adjustedFormat = getFormatInfo();
    if (!adjustedFormat) {
        return false;
    }
    mCodedSize.set(adjustedFormat->fmt.pix_mp.width, adjustedFormat->fmt.pix_mp.height);
    mVisibleRect = getVisibleRect(mCodedSize);

    ALOGI("Need %zu output buffers. coded size: %s, visible rect: %s", *numOutputBuffers,
          toString(mCodedSize).c_str(), toString(mVisibleRect).c_str());
    if (isEmpty(mCodedSize)) {
        ALOGE("Failed to get resolution from V4L2 driver.");
        return false;
    }

    mOutputQueue->streamoff();
    mOutputQueue->deallocateBuffers();
    mFrameAtDevice.clear();
    mBlockIdToV4L2Id.clear();

    const size_t adjustedNumOutputBuffers =
            mOutputQueue->allocateBuffers(*numOutputBuffers, V4L2_MEMORY_DMABUF);
    if (adjustedNumOutputBuffers == 0) {
        ALOGE("Failed to allocate output buffer.");
        return false;
    }
    ALOGV("Allocated %zu output buffers.", adjustedNumOutputBuffers);
    if (!mOutputQueue->streamon()) {
        ALOGE("Failed to streamon output queue.");
        return false;
    }

    // Release the previous VideoFramePool before getting a new one to guarantee only one pool
    // exists at the same time.
    mVideoFramePool.reset();
    // Always use flexible pixel 420 format YCBCR_420_888 in Android.
    mVideoFramePool =
            mGetPoolCb.Run(mCodedSize, HalPixelFormat::YCBCR_420_888, adjustedNumOutputBuffers);
    if (!mVideoFramePool) {
        ALOGE("Failed to get block pool with size: %s", toString(mCodedSize).c_str());
        return false;
    }

    tryFetchVideoFrame();
    return true;
}

bool V4L2Decoder::setupOutputFormat(const ui::Size& size) {
    for (const uint32_t& pixfmt :
         mDevice->enumerateSupportedPixelformats(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
        if (std::find(kSupportedOutputFourccs.begin(), kSupportedOutputFourccs.end(), pixfmt) ==
            kSupportedOutputFourccs.end()) {
            ALOGD("Pixel format %s is not supported, skipping...", fourccToString(pixfmt).c_str());
            continue;
        }

        if (mOutputQueue->setFormat(pixfmt, size, 0) != std::nullopt) {
            return true;
        }
    }

    ALOGE("Failed to find supported pixel format");
    return false;
}

void V4L2Decoder::tryFetchVideoFrame() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (!mVideoFramePool) {
        ALOGE("mVideoFramePool is null, failed to get the instance after resolution change?");
        onError();
        return;
    }

    if (mOutputQueue->freeBuffersCount() == 0) {
        ALOGV("No free V4L2 output buffers, ignore.");
        return;
    }

    if (!mVideoFramePool->getVideoFrame(
                ::base::BindOnce(&V4L2Decoder::onVideoFrameReady, mWeakThis))) {
        ALOGV("%s(): Previous callback is running, ignore.", __func__);
    }
}

void V4L2Decoder::onVideoFrameReady(
        std::optional<VideoFramePool::FrameWithBlockId> frameWithBlockId) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (!frameWithBlockId) {
        ALOGE("Got nullptr VideoFrame.");
        onError();
        return;
    }

    // Unwrap our arguments.
    std::unique_ptr<VideoFrame> frame;
    uint32_t blockId;
    std::tie(frame, blockId) = std::move(*frameWithBlockId);

    std::optional<V4L2WritableBufferRef> outputBuffer;
    // Find the V4L2 buffer that is associated with this block.
    auto iter = mBlockIdToV4L2Id.find(blockId);
    if (iter != mBlockIdToV4L2Id.end()) {
        // If we have met this block in the past, reuse the same V4L2 buffer.
        outputBuffer = mOutputQueue->getFreeBuffer(iter->second);
    } else if (mBlockIdToV4L2Id.size() < mOutputQueue->allocatedBuffersCount()) {
        // If this is the first time we see this block, give it the next
        // available V4L2 buffer.
        const size_t v4l2BufferId = mBlockIdToV4L2Id.size();
        mBlockIdToV4L2Id.emplace(blockId, v4l2BufferId);
        outputBuffer = mOutputQueue->getFreeBuffer(v4l2BufferId);
    } else {
        // If this happens, this is a bug in VideoFramePool. It should never
        // provide more blocks than we have V4L2 buffers.
        ALOGE("Got more different blocks than we have V4L2 buffers for.");
    }

    if (!outputBuffer) {
        ALOGE("V4L2 buffer not available. blockId=%u", blockId);
        onError();
        return;
    }

    uint32_t v4l2Id = outputBuffer->bufferId();
    ALOGV("QBUF to output queue, blockId=%u, V4L2Id=%u", blockId, v4l2Id);

    if (!std::move(*outputBuffer).queueDMABuf(frame->getFDs())) {
        ALOGE("%s(): Failed to QBUF to output queue, blockId=%u, V4L2Id=%u", __func__, blockId,
              v4l2Id);
        onError();
        return;
    }
    if (mFrameAtDevice.find(v4l2Id) != mFrameAtDevice.end()) {
        ALOGE("%s(): V4L2 buffer %d already enqueued.", __func__, v4l2Id);
        onError();
        return;
    }
    mFrameAtDevice.insert(std::make_pair(v4l2Id, std::move(frame)));

    tryFetchVideoFrame();
}

std::optional<size_t> V4L2Decoder::getNumOutputBuffers() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    if (mDevice->ioctl(VIDIOC_G_CTRL, &ctrl) != 0) {
        ALOGE("ioctl() failed: VIDIOC_G_CTRL");
        return std::nullopt;
    }
    ALOGV("%s() V4L2_CID_MIN_BUFFERS_FOR_CAPTURE returns %u", __func__, ctrl.value);

    return ctrl.value + kNumExtraOutputBuffers;
}

std::optional<struct v4l2_format> V4L2Decoder::getFormatInfo() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (mDevice->ioctl(VIDIOC_G_FMT, &format) != 0) {
        ALOGE("ioctl() failed: VIDIOC_G_FMT");
        return std::nullopt;
    }

    return format;
}

Rect V4L2Decoder::getVisibleRect(const ui::Size& codedSize) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_rect* visible_rect = nullptr;
    struct v4l2_selection selection_arg;
    memset(&selection_arg, 0, sizeof(selection_arg));
    selection_arg.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection_arg.target = V4L2_SEL_TGT_COMPOSE;

    if (mDevice->ioctl(VIDIOC_G_SELECTION, &selection_arg) == 0) {
        ALOGV("VIDIOC_G_SELECTION is supported");
        visible_rect = &selection_arg.r;
    } else {
        ALOGV("Fallback to VIDIOC_G_CROP");
        struct v4l2_crop crop_arg;
        memset(&crop_arg, 0, sizeof(crop_arg));
        crop_arg.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        if (mDevice->ioctl(VIDIOC_G_CROP, &crop_arg) != 0) {
            ALOGW("ioctl() VIDIOC_G_CROP failed");
            return Rect(codedSize.width, codedSize.height);
        }
        visible_rect = &crop_arg.c;
    }

    Rect rect(visible_rect->left, visible_rect->top, visible_rect->left + visible_rect->width,
              visible_rect->top + visible_rect->height);
    ALOGV("visible rectangle is %s", toString(rect).c_str());
    if (!contains(Rect(codedSize.width, codedSize.height), rect)) {
        ALOGW("visible rectangle %s is not inside coded size %s", toString(rect).c_str(),
              toString(codedSize).c_str());
        return Rect(codedSize.width, codedSize.height);
    }
    if (rect.isEmpty()) {
        ALOGW("visible size is empty");
        return Rect(codedSize.width, codedSize.height);
    }

    return rect;
}

bool V4L2Decoder::sendV4L2DecoderCmd(bool start) {
    ALOGV("%s(start=%d)", __func__, start);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_decoder_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = start ? V4L2_DEC_CMD_START : V4L2_DEC_CMD_STOP;
    if (mDevice->ioctl(VIDIOC_DECODER_CMD, &cmd) != 0) {
        ALOGE("ioctl() VIDIOC_DECODER_CMD failed: start=%d", start);
        return false;
    }

    return true;
}

void V4L2Decoder::onError() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    setState(State::Error);
    mErrorCb.Run();
}

void V4L2Decoder::setState(State newState) {
    ALOGV("%s(%s)", __func__, StateToString(newState));
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == newState) return;
    if (mState == State::Error) {
        ALOGV("Already in Error state.");
        return;
    }

    switch (newState) {
    case State::Idle:
        break;
    case State::Decoding:
        break;
    case State::Draining:
        if (mState != State::Decoding) newState = State::Error;
        break;
    case State::Error:
        break;
    }

    ALOGI("Set state %s => %s", StateToString(mState), StateToString(newState));
    mState = newState;
}

// static
const char* V4L2Decoder::StateToString(State state) {
    switch (state) {
    case State::Idle:
        return "Idle";
    case State::Decoding:
        return "Decoding";
    case State::Draining:
        return "Draining";
    case State::Error:
        return "Error";
    }
}

}  // namespace android

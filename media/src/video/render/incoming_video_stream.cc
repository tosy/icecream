/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "video/render/incoming_video_stream.h"

#include <memory>
#include <utility>

#include "absl/types/optional.h"
#include "api/units/time_delta.h"
#include "rtc_base/checks.h"
#include "rtc_base/trace_event.h"
#include "video/render/video_render_frames.h"

#include "rtc_base/time_utils.h"
#include "rtc_base/logging.h"

namespace webrtc {

IncomingVideoStream::IncomingVideoStream(
    TaskQueueFactory* task_queue_factory,
    int32_t delay_ms,
    rtc::VideoSinkInterface<VideoFrame>* callback)
    : render_buffers_(delay_ms),
      callback_(callback),
      incoming_render_queue_(task_queue_factory->CreateTaskQueue(
          "IncomingVideoStream",
          TaskQueueFactory::Priority::HIGH)) {}

IncomingVideoStream::~IncomingVideoStream() {
  RTC_DCHECK(main_thread_checker_.IsCurrent());
}

void IncomingVideoStream::OnFrame(const VideoFrame& video_frame) {
  TRACE_EVENT0("webrtc", "IncomingVideoStream::OnFrame");
  RTC_CHECK_RUNS_SERIALIZED(&decoder_race_checker_);
  RTC_DCHECK(!incoming_render_queue_.IsCurrent());


  // Local time in webrtc time base.
  auto post_ms = rtc::TimeMillis();
  //tosy test render too slow when 2k 4k resolution with bmp render
  int nProcessTasks =
      frames_scheduled_for_processing_.fetch_add(1, std::memory_order_relaxed);
  if (nProcessTasks >= 1) {
    frames_scheduled_for_processing_.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
  // TODO(srte): Using video_frame = std::move(video_frame) would move the frame
  // into the lambda instead of copying it, but it doesn't work unless we change
  // OnFrame to take its frame argument by value instead of const reference.
  incoming_render_queue_.PostTask([this, video_frame = video_frame,post_ms]() mutable {
    RTC_DCHECK_RUN_ON(&incoming_render_queue_);
    //tosy test too much cache
    if (render_buffers_.AddFrame(std::move(video_frame)) == 1){
      Dequeue();
      frames_scheduled_for_processing_.fetch_sub(1, std::memory_order_relaxed);
      static int nTestCount = 0;
      static int64_t nTestSumMs = 0;
      static int64_t nTestMax = 0;

      nTestCount++;
      auto diff = rtc::TimeMillis() - post_ms;
      nTestSumMs += diff;
      if (nTestMax < diff) {
        nTestMax = diff;
      }
      if (nTestCount >= 60) {
        RTC_LOG(LS_INFO) << "TOSY test IncomingVideoStream::OnFrame::cost "
                         << nTestSumMs / nTestCount << " MAX:" << nTestMax;
          nTestCount = 0;
          nTestSumMs = 0;
          nTestMax = 0;
      }
    }
  });
}

void IncomingVideoStream::Dequeue() {
  TRACE_EVENT0("webrtc", "IncomingVideoStream::Dequeue");
  RTC_DCHECK_RUN_ON(&incoming_render_queue_);

//  int64_t cur = rtc::TimeMillis();

  absl::optional<VideoFrame> frame_to_render = render_buffers_.FrameToRender();
  if (frame_to_render)
    callback_->OnFrame(*frame_to_render);

  if (render_buffers_.HasPendingFrames()) {
    uint32_t wait_time = render_buffers_.TimeToNextFrameRelease();
    incoming_render_queue_.PostDelayedHighPrecisionTask(
        [this]() { Dequeue(); }, TimeDelta::Millis(wait_time));
  }

  //tosy test show
  /*
  RTC_LOG(LS_INFO) << "TOSY IncomingVideoStream::Dequeue cost :: " << rtc::TimeMillis() - cur;
  */
}

}  // namespace webrtc

/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/conductor.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "absl/types/optional.h"
#include "api/audio/audio_mixer.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_peerconnection_factory.h"
#include "api/rtp_sender_interface.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "examples/peerconnection/client/defaults.h"
#include "modules/audio_device/include/audio_device.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h" 
#include "p2p/base/port_allocator.h"
#include "pc/video_track_source.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/strings/json.h"
#include "test/vcm_capturer.h"

#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_capture_options.h"

#include "api/task_queue/default_task_queue_factory.h"
#include "third_party/libyuv/include/libyuv.h"
#include "api/video/i420_buffer.h"

namespace {
// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
//TOSY TEST
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static rtc::scoped_refptr<DummySetSessionDescriptionObserver> Create() {
    return rtc::make_ref_counted<DummySetSessionDescriptionObserver>();
  }
  virtual void OnSuccess() { RTC_LOG(LS_INFO) << __FUNCTION__; }
  virtual void OnFailure(webrtc::RTCError error) {
    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << ToString(error.type()) << ": "
                     << error.message();
  }
};



class DesktopVideoSource : public webrtc::test::TestVideoCapturer,
                           webrtc::DesktopCapturer::Callback {
 public:
  static DesktopVideoSource* Create() {
      RTC_LOG(LS_INFO) << "TOSY DesktopVideoSource Create";
      
      webrtc::DesktopCaptureOptions options = webrtc::DesktopCaptureOptions::CreateDefault();
      //wgc not work
      //options.set_allow_wgc_capturer(true);
      options.set_allow_directx_capturer(true);

      auto dcapture = webrtc::DesktopCapturer::CreateScreenCapturer(options);
      // init dcapture
      webrtc::DesktopCapturer::SourceList sources;
      dcapture->GetSourceList(&sources);
      if (0 == sources.size()) {
        RTC_LOG(LS_INFO) << "TOSY DesktopVideoSource sources size 0";
        //TODO
        return nullptr;
      }
      RTC_LOG(LS_INFO) << "TOSY DesktopVideoSource sources size"
                       << sources.size();
      for (auto s : sources) {
        RTC_LOG(LS_INFO) << "TOSY DesktopVideoSource sourceid " << s.id;
      }
      dcapture->SelectSource(sources[0].id);
      return new DesktopVideoSource(std::move(dcapture));
    }

    int GetFrameWidth() const override { return static_cast<int>(width_); }
    int GetFrameHeight() const override { return static_cast<int>(height_); }
    ~DesktopVideoSource() { 
        webrtc::MutexLock lock(&lock_);
        worker_queue_.reset(nullptr);
    }

    DesktopVideoSource(const DesktopVideoSource&) = delete;
    DesktopVideoSource() = delete;

   protected:
    explicit DesktopVideoSource(std::unique_ptr<webrtc::DesktopCapturer> dcapture)
        : dcapture_(std::move(dcapture)) {
        last_ = rtc::TimeMillis();
        counts_second_ = 0;
        pastms_sum_ = 0;
        dcapture_->Start(this);
      
        // make thread for loop catpture desktop
        static std::unique_ptr<webrtc::TaskQueueFactory> factory =
            webrtc::CreateDefaultTaskQueueFactory(nullptr);
        worker_queue_.reset(new rtc::TaskQueue(factory->CreateTaskQueue(
            "dvs-capture-thread", webrtc::TaskQueueFactory::Priority::NORMAL)));
        //start loop task
        worker_queue_->PostTask([&] {
          LoopCaptureInThread();
        });
    }
    void LoopCaptureInThread() {
        webrtc::MutexLock lock(&lock_);
        if (worker_queue_.get()) {
            int64_t cur = rtc::TimeMillis();
            int64_t pastms = cur - last_;
            if (pastms >= duration_) {
                last_ = cur;
                worker_queue_->PostTask([&] {
                    dcapture_->CaptureFrame();
                    counts_second_++;
                    LoopCaptureInThread();
                });
                //counts
                pastms_sum_ += pastms;
                if (pastms_sum_ > 1000) {
                  pastms_sum_ = 0;
                  RTC_LOG(LS_INFO)
                      << "TOSY Desktop capture fps::" << counts_second_;
                  counts_second_ = 0;
                }
            } else {
                //RTC_LOG(LS_INFO) << "TOSY Loop duration_-pastms::" << duration_ - pastms;
                worker_queue_->PostDelayedTask([&]() {
                    LoopCaptureInThread();
                },webrtc::TimeDelta::Millis(duration_-pastms));
            }
        }
    }
    
    void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                         std::unique_ptr<webrtc::DesktopFrame> frame) override {
        if (result != webrtc::DesktopCapturer::Result::SUCCESS) {
            RTC_LOG(LS_INFO) << "TOSY OnCaptureResult Result::ERROR" ;
            return;
        }

//        int64_t cur = rtc::TimeMillis();

        // convert to video_frame
        int width = frame->size().width();
        int height = frame->size().height();
//        width_ = width;
//        height_ = height;

        rtc::scoped_refptr<webrtc::I420Buffer> i420_buffer =
            webrtc::I420Buffer::Create(width, height);
        libyuv::ConvertToI420(
            frame->data(), 0, i420_buffer->MutableDataY(),
            i420_buffer->StrideY(), i420_buffer->MutableDataU(),
            i420_buffer->StrideU(), i420_buffer->MutableDataV(),
            i420_buffer->StrideV(), 0, 0, width, height, width, height,
            libyuv::kRotate0, libyuv::FOURCC_ARGB);

        rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer = i420_buffer->Scale(width_, height_);
  
        webrtc::VideoFrame videoFrame =
            webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(buffer)
                .set_timestamp_rtp(0)
                .set_timestamp_ms(rtc::TimeMillis())
                .set_rotation(webrtc::kVideoRotation_0)
                .build();
        videoFrame.set_ntp_time_ms(0);
        //for test
        absl::optional<webrtc::Timestamp> cti(webrtc::Timestamp::Millis(rtc::TimeMillis()));
        videoFrame.set_capture_time_identifier(cti);

        // covert 2ms. capture 0~2ms. raw data capture cost 2~4ms.
        // codec 10ms average.
        //TODO 2ms cost !! no convert go hwcodec ??!!
        // best way  make host resolution as same as you want send.
//        RTC_LOG(LS_INFO) << "TOSY convert cost :: " << rtc::TimeMillis() - cur;
//        RTC_LOG(LS_INFO) << "TOSY capture cost :: " << frame->capture_time_ms();

        TestVideoCapturer::OnFrame(videoFrame);
    }
    webrtc::Mutex lock_;
    std::unique_ptr<webrtc::DesktopCapturer> dcapture_;
    std::unique_ptr<rtc::TaskQueue> worker_queue_;
    //var for multi-threads
    volatile size_t width_ = 1920;
    volatile size_t height_ = 1080;
    volatile int64_t duration_ = 1000/60;   //1000ms / 60fps
    //var for thread task
    int64_t last_;
    int64_t counts_second_;
    int64_t pastms_sum_;
};

class DesktopCapturerTrackSource : public webrtc::VideoTrackSource {
public:
  static rtc::scoped_refptr<DesktopCapturerTrackSource> Create(
      rtc::Thread* thread) {
    
      std::unique_ptr<DesktopVideoSource> dsource =
      thread->BlockingCall([&]() { 
          std::unique_ptr<DesktopVideoSource> dvs(DesktopVideoSource::Create());
          return dvs;
      });
    
      return rtc::make_ref_counted<DesktopCapturerTrackSource>(std::move(dsource));
  }

protected:
  explicit DesktopCapturerTrackSource(
      std::unique_ptr<DesktopVideoSource> dsource)
     : VideoTrackSource(/*remote*/ false), dsource_(std::move(dsource)) {}

private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
      return dsource_.get();
  }
  std::unique_ptr<DesktopVideoSource> dsource_;
};


class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<CapturerTrackSource> Create(rtc::Thread * thread) {
    const size_t kWidth = 640;
    const size_t kHeight = 480;
    const size_t kFps = 30;
    std::unique_ptr<webrtc::test::VcmCapturer> capturer;
    std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
        webrtc::VideoCaptureFactory::CreateDeviceInfo());
    if (!info) {
      return nullptr;
    }
    int num_devices = info->NumberOfDevices();

    // TOSY TEST
    for (int i = 0; i < num_devices; ++i) {
        char name[128] = {0};
        info->GetDeviceName(i, name, sizeof(name), nullptr, 0);
        RTC_LOG(LS_INFO) << " TOSY TEST deivce " << name;
    }
    for (int i = 0; i < num_devices; ++i) {
        //TOSY TEST
        /*
        capturer = absl::WrapUnique(
            webrtc::test::VcmCapturer::Create(kWidth, kHeight, kFps, i));
       */
      capturer = thread->BlockingCall([&]() {
        return absl::WrapUnique(
            webrtc::test::VcmCapturer::Create(kWidth, kHeight, kFps, i));
      }); 
      
      if (capturer) {
        return rtc::make_ref_counted<CapturerTrackSource>(std::move(capturer));
      }
    }

    return nullptr;
  }

 protected:
  explicit CapturerTrackSource(
      std::unique_ptr<webrtc::test::VcmCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}

 private:
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::unique_ptr<webrtc::test::VcmCapturer> capturer_;
};

}  // namespace

Conductor::Conductor(PeerConnectionClient* client, MainWindow* main_wnd)
    : peer_id_(-1), loopback_(false), client_(client), main_wnd_(main_wnd) {
  client_->RegisterObserver(this);
  main_wnd->RegisterObserver(this);
}

Conductor::~Conductor() {
  RTC_DCHECK(!peer_connection_);
}

bool Conductor::connection_active() const {
  return peer_connection_ != nullptr;
}

void Conductor::Close() {
  client_->SignOut();
  DeletePeerConnection();
}

bool Conductor::InitializePeerConnection() {
  RTC_DCHECK(!peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  if (!signaling_thread_.get()) {
    signaling_thread_ = rtc::Thread::CreateWithSocketServer();
    signaling_thread_->Start();
  }
  //rtc_include_builtin_audio_codecs=false
  peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
      nullptr /* network_thread */, nullptr /* worker_thread */,
      signaling_thread_.get(), nullptr /* default_adm */,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          //webrtc::LibvpxVp8EncoderTemplateAdapter
          // ,
          // webrtc::LibvpxVp9EncoderTemplateAdapter,
          // webrtc::OpenH264EncoderTemplateAdapter,
           webrtc::LibaomAv1EncoderTemplateAdapter
          >>(),
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          //webrtc::LibvpxVp8DecoderTemplateAdapter
          // ,
          // webrtc::LibvpxVp9DecoderTemplateAdapter,
          // webrtc::OpenH264DecoderTemplateAdapter,
           webrtc::Dav1dDecoderTemplateAdapter
          >>(),
      nullptr /* audio_mixer */, nullptr /* audio_processing */);

  if (!peer_connection_factory_) {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnectionFactory",
                          true);
    DeletePeerConnection();
    return false;
  }

  if (!CreatePeerConnection()) {
    main_wnd_->MessageBox("Error", "CreatePeerConnection failed", true);
    DeletePeerConnection();
  }

  AddTracks();

  return peer_connection_ != nullptr;
}

bool Conductor::ReinitializePeerConnectionForLoopback() {
  loopback_ = true;
  std::vector<rtc::scoped_refptr<webrtc::RtpSenderInterface>> senders =
      peer_connection_->GetSenders();
  peer_connection_ = nullptr;
  // Loopback is only possible if encryption is disabled.
  webrtc::PeerConnectionFactoryInterface::Options options;
  options.disable_encryption = true;
  peer_connection_factory_->SetOptions(options);
  if (CreatePeerConnection()) {
    for (const auto& sender : senders) {
      peer_connection_->AddTrack(sender->track(), sender->stream_ids());
    }
    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  }
  options.disable_encryption = false;
  peer_connection_factory_->SetOptions(options);
  return peer_connection_ != nullptr;
}

bool Conductor::CreatePeerConnection() {
  RTC_DCHECK(peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  webrtc::PeerConnectionFactoryInterface::Options options;
  options.disable_encryption = true;
  peer_connection_factory_->SetOptions(options);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  config.tcp_candidate_policy = 
      webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
  //  webrtc::PeerConnectionInterface::IceServer server;
//  server.uri = GetPeerConnectionString();
//  config.servers.push_back(server);

  webrtc::PeerConnectionDependencies pc_dependencies(this);
  auto error_or_peer_connection =
      peer_connection_factory_->CreatePeerConnectionOrError(
          config, std::move(pc_dependencies));
  if (error_or_peer_connection.ok()) {
    peer_connection_ = std::move(error_or_peer_connection.value());
    //tosy test set bitrate 10m
    webrtc::BitrateSettings bit_setting;
    bit_setting.min_bitrate_bps = 80,000,000;
    bit_setting.max_bitrate_bps = 80,000,000;
    bit_setting.start_bitrate_bps = 80,000,000;
    peer_connection_->SetBitrate(bit_setting);
  }
  return peer_connection_ != nullptr;
}

void Conductor::DeletePeerConnection() {
    main_wnd_->StopLocalRenderer();
    main_wnd_->StopRemoteRenderer();
    peer_connection_ = nullptr;
    peer_connection_factory_ = nullptr;
    peer_id_ = -1;
    loopback_ = false;
}

void Conductor::EnsureStreamingUI() {
  RTC_DCHECK(peer_connection_);
  if (main_wnd_->IsWindow()) {
    if (main_wnd_->current_ui() != MainWindow::STREAMING)
      main_wnd_->SwitchToStreamingUI();
  }
}

//
// PeerConnectionObserver implementation.
//

void Conductor::OnAddTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " PeerConnectionObserver " << receiver->id() << " "
                   << rtc::Thread::Current();
  main_wnd_->QueueUIThreadCallback(NEW_TRACK_ADDED,
                                   receiver->track().release());
}

void Conductor::OnRemoveTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " PeerConnectionObserver " << receiver->id() << " "
                   << rtc::Thread::Current();
  main_wnd_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
}

void Conductor::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " PeerConnectionObserver " << candidate->sdp_mline_index() << " "
                   << rtc::Thread::Current();
  // For loopback test. To save some connecting delay.
  if (loopback_) {
    if (!peer_connection_->AddIceCandidate(candidate)) {
      RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
    }
    return;
  }

  Json::Value jmessage;
  jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
  jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
  std::string sdp;
  if (!candidate->ToString(&sdp)) {
    RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
    return;
  }
  jmessage[kCandidateSdpName] = sdp;

  //TOSY TEST
  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void Conductor::OnSignedIn() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnDisconnected() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  //TOSY TEST
//  DeletePeerConnection();

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToConnectUI();
}

void Conductor::OnPeerConnected(int id, const std::string& name) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  // Refresh the list if we're showing it.
  if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnPeerDisconnected(int id) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (id == peer_id_) {
    RTC_LOG(LS_INFO) << "Our peer disconnected";
    main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_CLOSED, NULL);
  } else {
    // Refresh the list if we're showing it.
    if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
      main_wnd_->SwitchToPeerList(client_->peers());
  }
}

void Conductor::OnMessageFromPeer(int peer_id, const std::string& message) {
  //TOSY TEST
  RTC_DCHECK(peer_id_ == peer_id || peer_id_ == -1);
  RTC_DCHECK(!message.empty());

  if (!peer_connection_.get()) {
    RTC_DCHECK(peer_id_ == -1);
    peer_id_ = peer_id;

    if (!InitializePeerConnection()) {
      RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
      client_->SignOut();
      return;
    }
  } else if (peer_id != peer_id_) {
    RTC_DCHECK(peer_id_ != -1);
    RTC_LOG(LS_WARNING)
        << "Received a message from unknown peer while already in a "
           "conversation with a different peer.";
    return;
  }

  Json::CharReaderBuilder factory;
  std::unique_ptr<Json::CharReader> reader =
      absl::WrapUnique(factory.newCharReader());
  Json::Value jmessage;
  if (!reader->parse(message.data(), message.data() + message.length(),
                     &jmessage, nullptr)) {
    RTC_LOG(LS_WARNING) << "Received unknown message. " << message;
    return;
  }
  std::string type_str;
  std::string json_object;

  rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName,
                               &type_str);
  if (!type_str.empty()) {
    if (type_str == "offer-loopback") {
      // This is a loopback call.
      // Recreate the peerconnection with DTLS disabled.
      if (!ReinitializePeerConnectionForLoopback()) {
        RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
        DeletePeerConnection();
        client_->SignOut();
      }
      return;
    }
    absl::optional<webrtc::SdpType> type_maybe =
        webrtc::SdpTypeFromString(type_str);
    if (!type_maybe) {
      RTC_LOG(LS_ERROR) << "Unknown SDP type: " << type_str;
      return;
    }
    webrtc::SdpType type = *type_maybe;
    std::string sdp;
    if (!rtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName,
                                      &sdp)) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(type, sdp, &error);
    if (!session_description) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message. "
             "SdpParseError was: "
          << error.description;
      return;
    }
    RTC_LOG(LS_INFO) << " Received session description :" << message;
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create().get(),
        session_description.release());
    if (type == webrtc::SdpType::kOffer) {
      peer_connection_->CreateAnswer(
          this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    }
  } else {
    std::string sdp_mid;
    int sdp_mlineindex = 0;
    std::string sdp;
    if (!rtc::GetStringFromJsonObject(jmessage, kCandidateSdpMidName,
                                      &sdp_mid) ||
        !rtc::GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
                                   &sdp_mlineindex) ||
        !rtc::GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
      RTC_LOG(LS_WARNING) << "Can't parse received message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, &error));
    if (!candidate.get()) {
      RTC_LOG(LS_WARNING) << "Can't parse received candidate message. "
                             "SdpParseError was: "
                          << error.description;
      return;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
      RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
      return;
    }
    RTC_LOG(LS_INFO) << " Received candidate :" << message;
  }
}

void Conductor::OnMessageSent(int err) {
  // Process the next pending message if any.
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, NULL);
}

void Conductor::OnServerConnectionFailure() {
  main_wnd_->MessageBox("Error", ("Failed to connect to " + server_).c_str(),
                        true);
}

//
// MainWndCallback implementation.
//

void Conductor::StartLogin(const std::string& server, int port) {
  if (client_->is_connected())
    return;
  server_ = server;
  client_->Connect(server, port, GetPeerName());
}

void Conductor::DisconnectFromServer() {
  if (client_->is_connected())
    client_->SignOut();
}

void Conductor::ConnectToPeer(int peer_id) {
  RTC_DCHECK(peer_id_ == -1);
  RTC_DCHECK(peer_id != -1);

  if (peer_connection_.get()) {
    main_wnd_->MessageBox(
        "Error", "We only support connecting to one peer at a time", true);
    return;
  }

  if (InitializePeerConnection()) {
    peer_id_ = peer_id;
    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    RTC_LOG(LS_INFO) << "CreateOffer over";
  } else {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
  }
}

void Conductor::AddTracks() {
  if (!peer_connection_->GetSenders().empty()) {
    return;  // Already added tracks.
  }

  /*
  rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
      peer_connection_factory_->CreateAudioTrack(
          kAudioLabel,
          peer_connection_factory_->CreateAudioSource(cricket::AudioOptions())
              .get()));
*/
  //tosy test no audio
  /*
  rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track = 
      peer_connection_factory_->CreateAudioTrack(kAudioLabel,
          peer_connection_factory_->CreateAudioSource(cricket::AudioOptions()).get());

  auto result_or_error = peer_connection_->AddTrack(audio_track, {kStreamId});
  if (!result_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                      << result_or_error.error().message();
  }
  */

//  rtc::scoped_refptr<CapturerTrackSource> video_device =
//     CapturerTrackSource::Create(signaling_thread_.get());

  rtc::scoped_refptr<DesktopCapturerTrackSource> video_device =
      DesktopCapturerTrackSource::Create(signaling_thread_.get());

  if (video_device) {
    rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_ = 
        peer_connection_factory_->CreateVideoTrack(video_device, kVideoLabel);

    //TOSY TEST 
//    main_wnd_->StartLocalRenderer(video_track_.get());

    auto result_or_error = peer_connection_->AddTrack(video_track_, {kStreamId});
    if (!result_or_error.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
                        << result_or_error.error().message();
    }
  } else {
    RTC_LOG(LS_ERROR) << "OpenVideoCaptureDevice failed";
  }
  
  main_wnd_->SwitchToStreamingUI();
}

void Conductor::DisconnectFromCurrentPeer() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (peer_connection_.get()) {
    client_->SendHangUp(peer_id_);
    DeletePeerConnection();
  }

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::UIThreadCallback(int msg_id, void* data) {
  switch (msg_id) {
    case PEER_CONNECTION_CLOSED:
      RTC_LOG(LS_INFO) << "PEER_CONNECTION_CLOSED";
      DeletePeerConnection();

      if (main_wnd_->IsWindow()) {
        if (client_->is_connected()) {
          main_wnd_->SwitchToPeerList(client_->peers());
        } else {
          main_wnd_->SwitchToConnectUI();
        }
      } else {
        DisconnectFromServer();
      }
      break;

    case SEND_MESSAGE_TO_PEER: {
      RTC_LOG(LS_INFO) << "SEND_MESSAGE_TO_PEER";
      std::string* msg = reinterpret_cast<std::string*>(data);
      if (msg) {
        // For convenience, we always run the message through the queue.
        // This way we can be sure that messages are sent to the server
        // in the same order they were signaled without much hassle.
        pending_messages_.push_back(msg);
      }

      if (!pending_messages_.empty() && !client_->IsSendingMessage()) {
        msg = pending_messages_.front();
        pending_messages_.pop_front();

        if (!client_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
          RTC_LOG(LS_ERROR) << "SendToPeer failed";
          DisconnectFromServer();
        }
        delete msg;
      }

      if (!peer_connection_.get())
        peer_id_ = -1;

      break;
    }

    case NEW_TRACK_ADDED: {
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
        main_wnd_->StartRemoteRenderer(video_track);
      }
      track->Release();
      break;
    }

    case TRACK_REMOVED: {
      // Remote peer stopped sending a track.
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      track->Release();
      break;
    }

    default:
      RTC_DCHECK_NOTREACHED();
      break;
  }
}

void Conductor::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << rtc::Thread::Current();
  //TOSY TEST
  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create().get(), desc);

  std::string sdp;
  desc->ToString(&sdp);
  RTC_LOG(LS_INFO) << "Local sdp " << sdp;

  // For loopback test. To save some connecting delay.
  if (loopback_) {
    // Replace message type from "offer" to "answer"
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp);
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create().get(),
        session_description.release());
    return;
  }

  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] =
      webrtc::SdpTypeToString(desc->GetType());
  jmessage[kSessionDescriptionSdpName] = sdp;
  //TOSY TEST
  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

void Conductor::OnFailure(webrtc::RTCError error) {
  RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
}

void Conductor::SendMessage(const std::string& json_object) {
  std::string* msg = new std::string(json_object);
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);
}

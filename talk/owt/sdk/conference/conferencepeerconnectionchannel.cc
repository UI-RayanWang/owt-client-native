// Copyright (C) <2018> Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0
#include "talk/owt/sdk/conference/conferencepeerconnectionchannel.h"
#include <future>
#include <thread>
#include <vector>
#include "talk/owt/sdk/base/functionalobserver.h"
#include "talk/owt/sdk/base/mediautils.h"
#include "talk/owt/sdk/base/peerconnectiondependencyfactory.h"
#include "talk/owt/sdk/base/sdputils.h"
#include "talk/owt/sdk/include/cpp/owt/conference/remotemixedstream.h"
#include "webrtc/rtc_base/logging.h"
#include "webrtc/rtc_base/task_queue.h"
#include "webrtc/system_wrappers/include/field_trial.h"
using namespace rtc;
namespace owt {
namespace conference {
using std::string;
enum ConferencePeerConnectionChannel::SessionState : int {
  kSessionStateReady =
      1,  // Indicate the channel is ready. This is the initial state.
  kSessionStateOffered,     // Indicates local client has sent an invitation and
                            // waiting for an acceptance.
  kSessionStatePending,     // Indicates local client received an invitation and
                            // waiting for user's response.
  kSessionStateMatched,     // Indicates both sides agreed to start a WebRTC
                            // session. One of them will send an offer soon.
  kSessionStateConnecting,  // Indicates both sides are trying to connect to the
                            // other side.
  kSessionStateConnected,   // Indicates PeerConnection has been established.
};
enum ConferencePeerConnectionChannel::NegotiationState : int {
  kNegotiationStateNone = 1,  // Indicates not in renegotiation.
  kNegotiationStateSent,  // Indicates a negotiation request has been sent to
                          // remote user.
  kNegotiationStateReceived,  // Indicates local side has received a negotiation
                              // request from remote user.
  kNegotiationStateAccepted,  // Indicates local side has accepted remote user's
                              // negotiation request.
};
// Stream option member key
const string kStreamOptionStreamIdKey = "streamId";
const string kStreamOptionStateKey = "state";
const string kStreamOptionDataKey = "type";
const string kStreamOptionAudioKey = "audio";
const string kStreamOptionVideoKey = "video";
const string kStreamOptionScreenKey = "screen";
const string kStreamOptionAttributesKey = "attributes";
// Session description member key
const string kSessionDescriptionMessageTypeKey = "messageType";
const string kSessionDescriptionSdpKey = "sdp";
const string kSessionDescriptionOfferSessionIdKey = "offererSessionId";
const string kSessionDescriptionAnswerSessionIdKey = "answerSessionId";
const string kSessionDescriptionSeqKey = "seq";
const string kSessionDescriptionTiebreakerKey = "tiebreaker";
// ICE candidate member key
const string kIceCandidateSdpMidKey = "sdpMid";
const string kIceCandidateSdpMLineIndexKey = "sdpMLineIndex";
const string kIceCandidateSdpNameKey = "candidate";
ConferencePeerConnectionChannel::ConferencePeerConnectionChannel(
    PeerConnectionChannelConfiguration& configuration,
    std::shared_ptr<ConferenceSocketSignalingChannel> signaling_channel,
    std::shared_ptr<rtc::TaskQueue> event_queue)
    : PeerConnectionChannel(configuration),
      signaling_channel_(signaling_channel),
      session_id_(""),
      ice_restart_needed_(false),
      connected_(false),
      sub_stream_added_(false),
      sub_server_ready_(false),
      event_queue_(event_queue) {
  InitializePeerConnection();
  RTC_CHECK(signaling_channel_);
}
ConferencePeerConnectionChannel::~ConferencePeerConnectionChannel() {
  RTC_LOG(LS_INFO) << "Deconstruct conference peer connection channel";
  if (published_stream_)
    Unpublish(GetSessionId(), nullptr, nullptr);
  if (subscribed_stream_)
    Unsubscribe(GetSessionId(), nullptr, nullptr);
}
void ConferencePeerConnectionChannel::AddObserver(
    ConferencePeerConnectionChannelObserver& observer) {
  const std::lock_guard<std::mutex> lock(observers_mutex_);
  std::vector<std::reference_wrapper<ConferencePeerConnectionChannelObserver>>::
      iterator it = std::find_if(
          observers_.begin(), observers_.end(),
          [&](std::reference_wrapper<ConferencePeerConnectionChannelObserver> o)
              -> bool { return &observer == &(o.get()); });
  if (it != observers_.end()) {
    RTC_LOG(LS_WARNING) << "Adding duplicate observer.";
    return;
  }
  observers_.push_back(observer);
}
void ConferencePeerConnectionChannel::RemoveObserver(
    ConferencePeerConnectionChannelObserver& observer) {
  const std::lock_guard<std::mutex> lock(observers_mutex_);
  observers_.erase(std::find_if(
      observers_.begin(), observers_.end(),
      [&](std::reference_wrapper<ConferencePeerConnectionChannelObserver> o)
          -> bool { return &observer == &(o.get()); }));
}

void ConferencePeerConnectionChannel::CreateOffer() {
  RTC_LOG(LS_INFO) << "Create offer.";
  scoped_refptr<FunctionalCreateSessionDescriptionObserver> observer =
      FunctionalCreateSessionDescriptionObserver::Create(
          std::bind(&ConferencePeerConnectionChannel::
                        OnCreateSessionDescriptionSuccess,
                    this, std::placeholders::_1),
          std::bind(&ConferencePeerConnectionChannel::
                        OnCreateSessionDescriptionFailure,
                    this, std::placeholders::_1));
  bool rtp_no_mux = webrtc::field_trial::IsEnabled("OWT-IceUnbundle");
  auto offer_answer_options =
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions();
  offer_answer_options.use_rtp_mux = !rtp_no_mux;
  peer_connection_->CreateOffer(observer, offer_answer_options);
}

void ConferencePeerConnectionChannel::IceRestart() {
  if (SignalingState() == PeerConnectionInterface::SignalingState::kStable) {
    DoIceRestart();
  } else {
    ice_restart_needed_ = true;
  }
}
void ConferencePeerConnectionChannel::DoIceRestart() {
  RTC_LOG(LS_INFO) << "ICE restart";
  RTC_DCHECK(SignalingState() ==
             PeerConnectionInterface::SignalingState::kStable);
  this->CreateOffer();
}

void ConferencePeerConnectionChannel::CreateAnswer() {
  RTC_LOG(LS_INFO) << "Create answer.";
  scoped_refptr<FunctionalCreateSessionDescriptionObserver> observer =
      FunctionalCreateSessionDescriptionObserver::Create(
          std::bind(&ConferencePeerConnectionChannel::
                        OnCreateSessionDescriptionSuccess,
                    this, std::placeholders::_1),
          std::bind(&ConferencePeerConnectionChannel::
                        OnCreateSessionDescriptionFailure,
                    this, std::placeholders::_1));
  bool rtp_no_mux = webrtc::field_trial::IsEnabled("OWT-IceUnbundle");
  auto offer_answer_options =
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions();
  offer_answer_options.use_rtp_mux = !rtp_no_mux;
  peer_connection_->CreateAnswer(observer, offer_answer_options);
}

void ConferencePeerConnectionChannel::OnSignalingChange(
    PeerConnectionInterface::SignalingState new_state) {
  RTC_LOG(LS_INFO) << "Signaling state changed: " << new_state;
  signaling_state_ = new_state;
  if (new_state == webrtc::PeerConnectionInterface::SignalingState::kStable) {
    if (ice_restart_needed_) {
      ice_restart_needed_ = false;
      {
        std::lock_guard<std::mutex> lock(candidates_mutex_);
        ice_candidates_.clear();
      }
      DoIceRestart();
    } else {
      DrainIceCandidates();
    }
  }
}

void ConferencePeerConnectionChannel::OnAddStream(
    rtc::scoped_refptr<MediaStreamInterface> stream) {
  RTC_LOG(LS_INFO) << "On add stream.";
  if (subscribed_stream_ != nullptr)
    subscribed_stream_->MediaStream(stream);
  std::weak_ptr<ConferencePeerConnectionChannel> weak_this = shared_from_this();
  if (subscribe_success_callback_) {
    bool server_ready = false;
    {
      std::lock_guard<std::mutex> lock(sub_stream_added_mutex_);
      server_ready = sub_server_ready_;
      sub_stream_added_ = true;
      if (server_ready) {
        event_queue_->PostTask([weak_this] {
          auto that = weak_this.lock();
          std::lock_guard<std::mutex> lock(that->callback_mutex_);
          if (!that || !that->subscribe_success_callback_)
            return;
          that->subscribe_success_callback_(that->GetSessionId());
          that->ResetCallbacks();
        });
        sub_server_ready_ = false;
        sub_stream_added_ = false;
      }
    }
  }
}
void ConferencePeerConnectionChannel::OnRemoveStream(
    rtc::scoped_refptr<MediaStreamInterface> stream) {}
void ConferencePeerConnectionChannel::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {}
void ConferencePeerConnectionChannel::OnRenegotiationNeeded() {}
void ConferencePeerConnectionChannel::OnIceConnectionChange(
    PeerConnectionInterface::IceConnectionState new_state) {
  RTC_LOG(LS_INFO) << "Ice connection state changed: " << new_state;
  if (new_state == PeerConnectionInterface::kIceConnectionConnected ||
      new_state == PeerConnectionInterface::kIceConnectionCompleted) {
    connected_ = true;
  } else if (new_state == PeerConnectionInterface::kIceConnectionFailed) {
    // TODO(jianlin): Change trigger condition back to kIceConnectionClosed
    // once conference server re-enables IceRestart and client supports it as well.
    if (connected_) {
      OnStreamError(std::string("Stream ICE connection failed."));
    }
    connected_ = false;
  } else {
    return;
  }
  // It's better to clean all callbacks to avoid fire them again. But callbacks
  // are run in task queue, so we cannot clean it here. Also, PostTaskAndReply
  // requires a reply queue which is not available at this time.
}
void ConferencePeerConnectionChannel::OnIceGatheringChange(
    PeerConnectionInterface::IceGatheringState new_state) {
  RTC_LOG(LS_INFO) << "Ice gathering state changed: " << new_state;
}
// TODO(jianlin): New signaling protocol defines candidate as
// a string instead of object. Need to double check with server
// side implementation before we switch to it.
void ConferencePeerConnectionChannel::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  RTC_LOG(LS_INFO) << "On ice candidate";
  string candidate_string;
  candidate->ToString(&candidate_string);
  candidate_string.insert(0, "a=");
  sio::message::ptr message = sio::object_message::create();
  message->get_map()["id"] = sio::string_message::create(session_id_);
  sio::message::ptr sdp_message = sio::object_message::create();
  sdp_message->get_map()["type"] = sio::string_message::create("candidate");
  sio::message::ptr candidate_message = sio::object_message::create();
  candidate_message->get_map()["sdpMLineIndex"] =
      sio::int_message::create(candidate->sdp_mline_index());
  candidate_message->get_map()["sdpMid"] =
      sio::string_message::create(candidate->sdp_mid());
  candidate_message->get_map()["candidate"] =
      sio::string_message::create(candidate_string);
  sdp_message->get_map()["candidate"] = candidate_message;
  message->get_map()["signaling"] = sdp_message;
  if (signaling_state_ ==
      webrtc::PeerConnectionInterface::SignalingState::kStable) {
    signaling_channel_->SendSdp(message, nullptr, nullptr);
  } else {
    ice_candidates_.push_back(message);
  }
}
void ConferencePeerConnectionChannel::OnIceCandidatesRemoved(
    const std::vector<cricket::Candidate>& candidates) {
  RTC_LOG(LS_INFO) << "On ice candidate removed";
  if (candidates.empty())
    return;
  sio::message::ptr message = sio::object_message::create();
  message->get_map()["id"] = sio::string_message::create(session_id_);
  sio::message::ptr remove_candidates_msg = sio::object_message::create();
  remove_candidates_msg->get_map()["type"] =
      sio::string_message::create("removed-candidates");
  sio::message::ptr removed_candidates = sio::array_message::create();
  for (auto candidate : candidates) {
    std::string candidate_string = candidate.ToString();
    candidate_string.insert(0, "a=");
    sio::message::ptr current_candidate = sio::object_message::create();
    current_candidate->get_map()["candidate"] =
        sio::string_message::create(candidate_string);
    // jianlin: Native stack does not pop sdpMid & sdpMLineIndex to observer.
    // Maybe need to create a hash table to map candidate id to
    // sdpMid/sdpMlineIndex in OnIceCandidate().
    removed_candidates->get_vector().push_back(current_candidate);
  }
  remove_candidates_msg->get_map()["candidates"] = removed_candidates;
  message->get_map()["signaling"] = remove_candidates_msg;
  if (signaling_channel_) {
    signaling_channel_->SendSdp(message, nullptr, nullptr);
  }
}
void ConferencePeerConnectionChannel::OnCreateSessionDescriptionSuccess(
    webrtc::SessionDescriptionInterface* desc) {
  RTC_LOG(LS_INFO) << "Create sdp success.";
  scoped_refptr<FunctionalSetSessionDescriptionObserver> observer =
      FunctionalSetSessionDescriptionObserver::Create(
          std::bind(&ConferencePeerConnectionChannel::
                        OnSetLocalSessionDescriptionSuccess,
                    this),
          std::bind(&ConferencePeerConnectionChannel::
                        OnSetLocalSessionDescriptionFailure,
                    this, std::placeholders::_1));
  std::string sdp_string;
  if (!desc->ToString(&sdp_string)) {
    RTC_LOG(LS_ERROR) << "Error parsing local description.";
    RTC_DCHECK(false);
  }
  std::vector<AudioCodec> audio_codecs;
  for (auto& audio_enc_param : configuration_.audio) {
    audio_codecs.push_back(audio_enc_param.codec.name);
  }
  sdp_string = SdpUtils::SetPreferAudioCodecs(sdp_string, audio_codecs);
  std::vector<VideoCodec> video_codecs;
  for (auto& video_enc_param : configuration_.video) {
    video_codecs.push_back(video_enc_param.codec.name);
  }
  bool is_screen = published_stream_.get() ? (published_stream_->Source().video ==
                          owt::base::VideoSourceInfo::kScreenCast)
                       : (subscribed_stream_.get()
                          ? (subscribed_stream_->Source().video ==
                                    owt::base::VideoSourceInfo::kScreenCast)
                          : false);
  sdp_string = SdpUtils::SetPreferVideoCodecs(sdp_string, video_codecs, is_screen);
  webrtc::SessionDescriptionInterface* new_desc(
      webrtc::CreateSessionDescription(desc->type(), sdp_string, nullptr));
  peer_connection_->SetLocalDescription(observer, new_desc);
}
void ConferencePeerConnectionChannel::OnCreateSessionDescriptionFailure(
    const std::string& error) {
  RTC_LOG(LS_INFO) << "Create sdp failed.";
}
void ConferencePeerConnectionChannel::OnSetLocalSessionDescriptionSuccess() {
  RTC_LOG(LS_INFO) << "Set local sdp success.";
  // For conference, it's now OK to set bandwidth
  ApplyBitrateSettings();
  auto desc = LocalDescription();
  string sdp;
  desc->ToString(&sdp);
  sio::message::ptr message = sio::object_message::create();
  message->get_map()["id"] = sio::string_message::create(session_id_);
  sio::message::ptr sdp_message = sio::object_message::create();
  sdp_message->get_map()["type"] = sio::string_message::create(desc->type());
  sdp_message->get_map()["sdp"] = sio::string_message::create(sdp);
  message->get_map()["signaling"] = sdp_message;
  signaling_channel_->SendSdp(message, nullptr, nullptr);
}
void ConferencePeerConnectionChannel::OnSetLocalSessionDescriptionFailure(
    const std::string& error) {
  RTC_LOG(LS_INFO) << "Set local sdp failed.";
  if (failure_callback_) {
    std::unique_ptr<Exception> e(new Exception(
        ExceptionType::kConferenceUnknown, "Failed to set local description."));
    failure_callback_(std::move(e));
    ResetCallbacks();
  }
  OnStreamError(std::string("Failed to set local description."));
}
void ConferencePeerConnectionChannel::OnSetRemoteSessionDescriptionSuccess() {
  PeerConnectionChannel::OnSetRemoteSessionDescriptionSuccess();
}
void ConferencePeerConnectionChannel::OnSetRemoteSessionDescriptionFailure(
    const std::string& error) {
  RTC_LOG(LS_INFO) << "Set remote sdp failed.";
  if (failure_callback_) {
    std::unique_ptr<Exception> e(new Exception(
        ExceptionType::kConferenceUnknown, "Fail to set remote description."));
    failure_callback_(std::move(e));
    ResetCallbacks();
  }
  OnStreamError(std::string("Failed to set remote description."));
}
void ConferencePeerConnectionChannel::SetRemoteDescription(
    const std::string& type,
    const std::string& sdp) {
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc(
      webrtc::CreateSessionDescription(
      "answer", sdp,
      nullptr));  // TODO(jianjun): change answer to type.toLowerCase.
  if (!desc) {
    RTC_LOG(LS_ERROR) << "Failed to create session description.";
    return;
  }
  scoped_refptr<FunctionalSetRemoteDescriptionObserver> observer =
      FunctionalSetRemoteDescriptionObserver::Create(std::bind(
          &ConferencePeerConnectionChannel::OnSetRemoteDescriptionComplete,
                    this, std::placeholders::_1));
  peer_connection_->SetRemoteDescription(std::move(desc), observer);
}

bool ConferencePeerConnectionChannel::CheckNullPointer(
    uintptr_t pointer,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (pointer)
    return true;
  if (on_failure != nullptr) {
    event_queue_->PostTask([on_failure]() {
      std::unique_ptr<Exception> e(new Exception(
          ExceptionType::kConferenceUnknown, "Nullptr is not allowed."));
      on_failure(std::move(e));
    });
  }
  return false;
}
// Failure of publish will be handled here directly; while success needs
// conference client to construct the ConferencePublication instance,
// So we're not passing success callback here.
void ConferencePeerConnectionChannel::Publish(
    std::shared_ptr<LocalStream> stream,
    std::function<void(std::string)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  RTC_LOG(LS_INFO) << "Publish a local stream.";
  published_stream_ = stream;
  if ((!CheckNullPointer((uintptr_t)stream.get(), on_failure)) ||
      (!CheckNullPointer((uintptr_t)stream->MediaStream(), on_failure))) {
    RTC_LOG(LS_INFO) << "Local stream cannot be nullptr.";
  }
  if (IsMediaStreamEnded(stream->MediaStream())) {
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(new Exception(
            ExceptionType::kConferenceUnknown, "Cannot publish ended stream."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  int audio_track_count = 0, video_track_count = 0;
  audio_track_count = stream->MediaStream()->GetAudioTracks().size();
  video_track_count = stream->MediaStream()->GetVideoTracks().size();
  if (audio_track_count == 0 && video_track_count == 0) {
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(new Exception(
            ExceptionType::kConferenceUnknown,
            "Cannot publish media stream without any tracks."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  publish_success_callback_ = on_success;
  failure_callback_ = on_failure;
  audio_transceiver_direction_=webrtc::RtpTransceiverDirection::kSendOnly;
  video_transceiver_direction_=webrtc::RtpTransceiverDirection::kSendOnly;
  sio::message::ptr options = sio::object_message::create();
  // attributes
  sio::message::ptr attributes_ptr = sio::object_message::create();
  for (auto const& attr : stream->Attributes()) {
    attributes_ptr->get_map()[attr.first] =
        sio::string_message::create(attr.second);
  }
  options->get_map()[kStreamOptionAttributesKey] = attributes_ptr;
  // TODO(jianlin): Currently we fix mid to 0/1. Need
  // to update the flow to set local desc for retrieving the mid.
  // See https://github.com/open-webrtc-toolkit/owt-client-native/issues/459
  // for more details.
  sio::message::ptr media_ptr = sio::object_message::create();
  sio::message::ptr tracks_ptr = sio::array_message::create();
  if (audio_track_count != 0) {
    RTC_LOG(LS_INFO) << "Adding audio tracks for publish.";
    sio::message::ptr audio_options = sio::object_message::create();
    audio_options->get_map()["type"] = sio::string_message::create("audio");
    audio_options->get_map()["mid"] = sio::string_message::create("0");
    if (stream->Source().audio == owt::base::AudioSourceInfo::kScreenCast) {
      audio_options->get_map()["source"] =
          sio::string_message::create("screen-cast");
    } else {
      audio_options->get_map()["source"] = sio::string_message::create("mic");
    }
    tracks_ptr->get_vector().push_back(audio_options);
  }
  if (video_track_count != 0) {
    RTC_LOG(LS_INFO) << "Adding video tracks for publish.";
    sio::message::ptr video_options = sio::object_message::create();
    video_options->get_map()["type"] = sio::string_message::create("video");
    if (audio_track_count == 0) {
      video_options->get_map()["mid"] = sio::string_message::create("0");
    } else {
      video_options->get_map()["mid"] = sio::string_message::create("1");
    }
    if (stream->Source().video == owt::base::VideoSourceInfo::kScreenCast) {
      video_options->get_map()["source"] =
          sio::string_message::create("screen-cast");
    } else {
      video_options->get_map()["source"] =
          sio::string_message::create("camera");
    }
    tracks_ptr->get_vector().push_back(video_options);
  }
  media_ptr->get_map()["tracks"] = tracks_ptr;
  options->get_map()["media"] = media_ptr;
  sio::message::ptr transport_ptr = sio::object_message::create();
  transport_ptr->get_map()["type"] = sio::string_message::create("webrtc");
  options->get_map()["transport"] = transport_ptr;
  SendPublishMessage(options, stream, on_failure);
}

static bool SubOptionAllowed(
    const SubscribeOptions& subscribe_options,
    const PublicationSettings& publication_settings,
    const SubscriptionCapabilities& subscription_caps) {
  // TODO: Audio subscribe constraints are currently not checked as spec only
  // specifies codec, though signaling allows specifying sample rate and channel
  // number.

  // If rid is specified, search in publication_settings for rid;
  if (subscribe_options.video.rid != "") {
    for (auto video_setting : publication_settings.video) {
      if (video_setting.rid == subscribe_options.video.rid)
        return true;
    }
    return false;
  }

  bool resolution_supported = (subscribe_options.video.resolution.width == 0 &&
                               subscribe_options.video.resolution.height == 0);
  bool frame_rate_supported = (subscribe_options.video.frameRate == 0);
  bool keyframe_interval_supported =
      (subscribe_options.video.keyFrameInterval == 0);
  bool bitrate_multiplier_supported =
      (subscribe_options.video.bitrateMultiplier == 0);

  // If rid is not used, check in publication_settings and capabilities.
  for (auto video_setting : publication_settings.video) {
    if (subscribe_options.video.resolution.width != 0 &&
        subscribe_options.video.resolution.height != 0 &&
        video_setting.resolution.width ==
            subscribe_options.video.resolution.width &&
        video_setting.resolution.height ==
            subscribe_options.video.resolution.height) {
      resolution_supported = true;
    }

    if (subscribe_options.video.frameRate != 0 &&
            video_setting.frame_rate == subscribe_options.video.frameRate) {
      frame_rate_supported = true;
    }

    if (subscribe_options.video.keyFrameInterval != 0 &&
        video_setting.keyframe_interval ==
            subscribe_options.video.keyFrameInterval)
          keyframe_interval_supported = true;
  }

  if (subscribe_options.video.resolution.width != 0 &&
      subscribe_options.video.resolution.height != 0) {
    if (std::find_if(subscription_caps.video.resolutions.begin(),
                     subscription_caps.video.resolutions.end(),
                     [&](const Resolution& format) {
                       return format == subscribe_options.video.resolution;
                     }) != subscription_caps.video.resolutions.end()) {
      resolution_supported = true;
    }
  }
  if (subscribe_options.video.frameRate != 0) {
    if (std::find_if(subscription_caps.video.frame_rates.begin(),
                     subscription_caps.video.frame_rates.end(),
                     [&](const double& format) {
                       return format == subscribe_options.video.frameRate;
                     }) != subscription_caps.video.frame_rates.end()) {
      frame_rate_supported = true;
    }
  }
  if (subscribe_options.video.keyFrameInterval != 0) {
    if (std::find_if(subscription_caps.video.keyframe_intervals.begin(),
                     subscription_caps.video.keyframe_intervals.end(),
                     [&](const unsigned long& format) {
                       return format ==
                              subscribe_options.video.keyFrameInterval;
                     }) != subscription_caps.video.keyframe_intervals.end()) {
      keyframe_interval_supported = true;
    }
  }
  if (subscribe_options.video.bitrateMultiplier != 0) {
    if (std::find_if(subscription_caps.video.bitrate_multipliers.begin(),
                     subscription_caps.video.bitrate_multipliers.end(),
                     [&](const double& format) {
                       return format ==
                              subscribe_options.video.bitrateMultiplier;
                     }) != subscription_caps.video.bitrate_multipliers.end()) {
      bitrate_multiplier_supported = true;
    }
  }
  return (resolution_supported && frame_rate_supported &&
      keyframe_interval_supported && bitrate_multiplier_supported);
}

void ConferencePeerConnectionChannel::Subscribe(
    std::shared_ptr<RemoteStream> stream,
    const SubscribeOptions& subscribe_options,
    std::function<void(std::string)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  RTC_LOG(LS_INFO) << "Subscribe a remote stream. It has audio? "
                   << stream->has_audio_ << ", has video? "
                   << stream->has_video_;
  if (!SubOptionAllowed(subscribe_options, stream->Settings(), stream->Capabilities())) {
    RTC_LOG(LS_ERROR)
        << "Subscribe option mismatch with stream subcription capabilities.";
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kConferenceUnknown,
                          "Unsupported subscribe option."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  if (!CheckNullPointer((uintptr_t)stream.get(), on_failure)) {
    RTC_LOG(LS_ERROR) << "Remote stream cannot be nullptr.";
    return;
  }
  if (subscribe_success_callback_) {
    if (on_failure) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(new Exception(
            ExceptionType::kConferenceUnknown, "Subscribing this stream."));
        on_failure(std::move(e));
      });
    }
  }
  subscribe_success_callback_ = on_success;
  failure_callback_ = on_failure;
  int audio_track_count = 0, video_track_count = 0;
  if (stream->has_audio_ && !subscribe_options.audio.disabled) {
    webrtc::RtpTransceiverInit transceiver_init;
    transceiver_init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
    AddTransceiver(cricket::MediaType::MEDIA_TYPE_AUDIO, transceiver_init);
    audio_track_count = 1;
  }
  if (stream->has_video_ && !subscribe_options.video.disabled) {
    webrtc::RtpTransceiverInit transceiver_init;
    transceiver_init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
    AddTransceiver(cricket::MediaType::MEDIA_TYPE_VIDEO, transceiver_init);
    video_track_count = 1;
  }
  sio::message::ptr sio_options = sio::object_message::create();
  sio::message::ptr media_options = sio::object_message::create();
  sio::message::ptr tracks_options = sio::array_message::create();
  if (audio_track_count > 0) {
    sio::message::ptr audio_options = sio::object_message::create();
    audio_options->get_map()["type"] = sio::string_message::create("audio");
    audio_options->get_map()["mid"] = sio::string_message::create("0");
    audio_options->get_map()["from"] =
        sio::string_message::create(stream->Id());
    tracks_options->get_vector().push_back(audio_options);
  }
  if (video_track_count > 0) {
    sio::message::ptr video_options = sio::object_message::create();
    video_options->get_map()["type"] = sio::string_message::create("video");
    if (audio_track_count == 0) {
      video_options->get_map()["mid"] = sio::string_message::create("0");
    } else {
      video_options->get_map()["mid"] = sio::string_message::create("1");
    }
    auto publication_settings = stream->Settings();
    if (subscribe_options.video.rid != "") {
      for (auto video_setting : publication_settings.video) {
        if (video_setting.rid == subscribe_options.video.rid) {
          std::string track_id = video_setting.track_id;
          video_options->get_map()["from"] =
              sio::string_message::create(track_id);
          break;
        }
      }
    } else {
      video_options->get_map()["from"] =
          sio::string_message::create(stream->Id());
    }
    sio::message::ptr video_spec = sio::object_message::create();
    sio::message::ptr resolution_options = sio::object_message::create();
    if (subscribe_options.video.resolution.width != 0 &&
        subscribe_options.video.resolution.height != 0) {
      resolution_options->get_map()["width"] =
          sio::int_message::create(subscribe_options.video.resolution.width);
      resolution_options->get_map()["height"] =
          sio::int_message::create(subscribe_options.video.resolution.height);
      video_spec->get_map()["resolution"] = resolution_options;
    }
    // If bitrateMultiplier is not specified, do not include it in video spec.
    std::string quality_level("x1.0");
    if (subscribe_options.video.bitrateMultiplier != 0) {
      quality_level =
          "x" + std::to_string(subscribe_options.video.bitrateMultiplier)
                    .substr(0, 3);
    }
    if (quality_level != "x1.0") {
      sio::message::ptr quality_options =
          sio::string_message::create(quality_level);
      video_spec->get_map()["bitrate"] = quality_options;
    }
    if (subscribe_options.video.keyFrameInterval != 0) {
      video_spec->get_map()["keyFrameInterval"] =
          sio::int_message::create(subscribe_options.video.keyFrameInterval);
    }
    if (subscribe_options.video.frameRate != 0) {
      video_spec->get_map()["framerate"] =
          sio::int_message::create(subscribe_options.video.frameRate);
    }
    video_options->get_map()["parameters"] = video_spec;
    if (subscribe_options.video.rid != "") {
      video_options->get_map()["simulcastRid"] =
          sio::string_message::create(subscribe_options.video.rid);
    }
    tracks_options->get_vector().push_back(video_options);
  }

  media_options->get_map()["tracks"] = tracks_options;
  sio_options->get_map()["media"] = media_options;
  sio::message::ptr transport_ptr = sio::object_message::create();
  transport_ptr->get_map()["type"] = sio::string_message::create("webrtc");
  sio_options->get_map()["transport"] = transport_ptr;

  signaling_channel_->SendInitializationMessage(
      sio_options, "", stream->Id(),
      [this](std::string session_id, std::string transport_id) {
        // Pre-set the session's ID.
        SetSessionId(session_id);
        CreateOffer();
      },
      on_failure);  // TODO: on_failure
  subscribed_stream_ = stream;
}
void ConferencePeerConnectionChannel::Unpublish(
    const std::string& session_id,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (session_id != GetSessionId()) {
    RTC_LOG(LS_ERROR) << "Publication ID mismatch.";
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kConferenceUnknown,
                          "Invalid stream to be unpublished."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  connected_ = false;
  signaling_channel_->SendStreamEvent("unpublish", session_id,
                                      RunInEventQueue(on_success), on_failure);
  this->ClosePeerConnection();
}
void ConferencePeerConnectionChannel::Unsubscribe(
    const std::string& session_id,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (session_id != GetSessionId()) {
    RTC_LOG(LS_ERROR) << "Subscription ID mismatch.";
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kConferenceUnknown,
                          "Invalid stream to be unsubscribed."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  if (subscribe_success_callback_ != nullptr) {  // Subscribing
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kConferenceUnknown,
                          "Cannot unsubscribe a stream during subscribing."));
        on_failure(std::move(e));
      });
    }
    return;
  }
  connected_ = false;
  signaling_channel_->SendStreamEvent("unsubscribe", session_id,
                                      RunInEventQueue(on_success), on_failure);
  this->ClosePeerConnection();
}
void ConferencePeerConnectionChannel::SendStreamControlMessage(
    const std::string& in_action,
    const std::string& out_action,
    const std::string& operation,
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) const {
  std::string action = "";
  if (published_stream_) {
    action = out_action;
    signaling_channel_->SendStreamControlMessage(session_id_, action, operation,
                                                 on_success, on_failure);
  } else if (subscribed_stream_) {
    action = in_action;
    signaling_channel_->SendSubscriptionControlMessage(
        session_id_, action, operation, on_success, on_failure);
  } else
    RTC_DCHECK(false);
}
void ConferencePeerConnectionChannel::PlayAudioVideo(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  SendStreamControlMessage("av", "av", "play", on_success, on_failure);
}
void ConferencePeerConnectionChannel::PauseAudioVideo(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  SendStreamControlMessage("av", "av", "pause", on_success, on_failure);
}
void ConferencePeerConnectionChannel::PlayAudio(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  SendStreamControlMessage("audio", "audio", "play", on_success, on_failure);
}
void ConferencePeerConnectionChannel::PauseAudio(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  SendStreamControlMessage("audio", "audio", "pause", on_success, on_failure);
}
void ConferencePeerConnectionChannel::PlayVideo(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  SendStreamControlMessage("video", "video", "play", on_success, on_failure);
}
void ConferencePeerConnectionChannel::PauseVideo(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  SendStreamControlMessage("video", "video", "pause", on_success, on_failure);
}
void ConferencePeerConnectionChannel::Stop(
    std::function<void()> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  RTC_LOG(LS_INFO) << "Stop session.";
}
void ConferencePeerConnectionChannel::GetConnectionStats(
    std::function<void(std::shared_ptr<ConnectionStats>)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (!published_stream_ && !subscribed_stream_) {
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kConferenceUnknown,
                          "No stream associated with the session"));
        on_failure(std::move(e));
      });
    }
    return;
  }
  if (subscribed_stream_ || published_stream_) {
    scoped_refptr<FunctionalStatsObserver> observer =
        FunctionalStatsObserver::Create(on_success);
    peer_connection_->GetStats(
        observer, nullptr,
        webrtc::PeerConnectionInterface::kStatsOutputLevelStandard);
  }
}

void ConferencePeerConnectionChannel::GetConnectionStats(
    std::function<void(std::shared_ptr<RTCStatsReport>)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (!published_stream_ && !subscribed_stream_) {
    if (on_failure != nullptr) {
      event_queue_->PostTask([on_failure]() {
        std::unique_ptr<Exception> e(
            new Exception(ExceptionType::kConferenceUnknown,
                          "No stream associated with the session"));
        on_failure(std::move(e));
      });
    }
    return;
  }
  if (subscribed_stream_ || published_stream_) {
    rtc::scoped_refptr<FunctionalStandardRTCStatsCollectorCallback> observer =
        FunctionalStandardRTCStatsCollectorCallback::Create(
            std::move(on_success));
    peer_connection_->GetStats(observer);
  }
}

void ConferencePeerConnectionChannel::GetStats(
    std::function<void(const webrtc::StatsReports& reports)> on_success,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  if (!on_success) {
    return;
  }
  scoped_refptr<FunctionalNativeStatsObserver> observer =
      FunctionalNativeStatsObserver::Create(on_success);
  peer_connection_->GetStats(
      observer, nullptr,
      webrtc::PeerConnectionInterface::kStatsOutputLevelStandard);
}

void ConferencePeerConnectionChannel::OnSignalingMessage(
    sio::message::ptr message) {
  if (message == nullptr) {
    RTC_LOG(LS_INFO) << "Ignore empty signaling message";
    return;
  }
  if (message->get_flag() == sio::message::flag_string) {
    if (message->get_string() == "success") {
      std::weak_ptr<ConferencePeerConnectionChannel> weak_this =
          shared_from_this();
      if (publish_success_callback_) {
        event_queue_->PostTask([weak_this] {
          auto that = weak_this.lock();
          std::lock_guard<std::mutex> lock(that->callback_mutex_);
          if (!that || !that->publish_success_callback_)
            return;
          that->publish_success_callback_(that->GetSessionId());
          that->ResetCallbacks();
        });
      } else if (subscribe_success_callback_) {
        bool stream_added = false;
        {
          std::lock_guard<std::mutex> lock(sub_stream_added_mutex_);
          stream_added = sub_stream_added_;
          sub_server_ready_ = true;
          if (stream_added) {
            event_queue_->PostTask([weak_this] {
              auto that = weak_this.lock();
              std::lock_guard<std::mutex> lock(that->callback_mutex_);
              if (!that || !that->subscribe_success_callback_)
                return;
              that->subscribe_success_callback_(that->GetSessionId());
              that->ResetCallbacks();
            });
            sub_server_ready_ = false;
            sub_stream_added_ = false;
          }
        }
      }
      return;
    } else if (message->get_string() == "failure") {
      if (!connected_ && failure_callback_) {
        std::weak_ptr<ConferencePeerConnectionChannel> weak_this =
            shared_from_this();
        event_queue_->PostTask([weak_this] {
          auto that = weak_this.lock();
          std::lock_guard<std::mutex> lock(that->callback_mutex_);
          if (!that || !that->failure_callback_)
            return;
          std::unique_ptr<Exception> e(new Exception(
              ExceptionType::kConferenceUnknown,
              "Server internal error during connection establishment."));
          that->failure_callback_(std::move(e));
          that->ResetCallbacks();
        });
      }
    }
    return;
  } else if (message->get_flag() != sio::message::flag_object) {
    RTC_LOG(LS_WARNING) << "Ignore invalid signaling message from server.";
    return;
  }
  // Since trickle ICE from server is not supported, we parse the message as
  // SOAC message, not Canddiate message.
  if (message->get_map().find("type") == message->get_map().end()) {
    RTC_LOG(LS_INFO) << "Ignore message without type from server.";
    return;
  }
  if (message->get_map()["type"]->get_flag() != sio::message::flag_string ||
      message->get_map()["sdp"] == nullptr ||
      message->get_map()["sdp"]->get_flag() != sio::message::flag_string) {
    RTC_LOG(LS_ERROR) << "Invalid signaling message";
    return;
  }
  const std::string type = message->get_map()["type"]->get_string();
  RTC_LOG(LS_INFO) << "On signaling message: " << type;
  if (type == "answer") {
    const std::string sdp = message->get_map()["sdp"]->get_string();
    SetRemoteDescription(type, sdp);
  } else {
    RTC_LOG(LS_ERROR)
        << "Ignoring signaling message from server other than answer.";
  }
}
void ConferencePeerConnectionChannel::DrainIceCandidates() {
  std::lock_guard<std::mutex> lock(candidates_mutex_);
  for (auto it = ice_candidates_.begin(); it != ice_candidates_.end(); ++it) {
    signaling_channel_->SendSdp(*it, nullptr, nullptr);
  }
  ice_candidates_.clear();
}
std::string ConferencePeerConnectionChannel::GetSubStreamId() {
  if (subscribed_stream_) {
    return subscribed_stream_->Id();
  } else {
    return "";
  }
}
void ConferencePeerConnectionChannel::SetSessionId(const std::string& id) {
  RTC_LOG(LS_INFO) << "Setting session ID for current channel";
  session_id_ = id;
}
std::string ConferencePeerConnectionChannel::GetSessionId() const {
  return session_id_;
}
void ConferencePeerConnectionChannel::SendPublishMessage(
    sio::message::ptr options,
    std::shared_ptr<LocalStream> stream,
    std::function<void(std::unique_ptr<Exception>)> on_failure) {
  signaling_channel_->SendInitializationMessage(
      options, stream->MediaStream()->id(), "",
      [stream, this](std::string session_id, std::string transport_id) {
        SetSessionId(session_id);
        for (const auto& track : stream->MediaStream()->GetAudioTracks()) {
          webrtc::RtpTransceiverInit transceiver_init;
          transceiver_init.stream_ids.push_back(stream->MediaStream()->id());
          transceiver_init.direction = webrtc::RtpTransceiverDirection::kSendOnly;
          AddTransceiver(track, transceiver_init);
        }
        for (const auto& track : stream->MediaStream()->GetVideoTracks()) {
          webrtc::RtpTransceiverInit transceiver_init;
          transceiver_init.stream_ids.push_back(stream->MediaStream()->id());
          transceiver_init.direction =
              webrtc::RtpTransceiverDirection::kSendOnly;
          if (configuration_.video.size() > 0 &&
              configuration_.video[0].rtp_encoding_parameters.size() != 0) {
            for (auto encoding :
                 configuration_.video[0].rtp_encoding_parameters) {
              webrtc::RtpEncodingParameters param;
              if (encoding.rid != "")
                param.rid = encoding.rid;
              if (encoding.max_bitrate_bps != 0)
                param.max_bitrate_bps = encoding.max_bitrate_bps;
              if (encoding.max_framerate != 0)
                param.max_framerate = encoding.max_framerate;
              if (encoding.scale_resolution_down_by > 0)
                param.scale_resolution_down_by =
                    encoding.scale_resolution_down_by;
              if (encoding.num_temporal_layers > 0 &&
                  encoding.num_temporal_layers <= 4) {
                param.num_temporal_layers = encoding.num_temporal_layers;
              }
              if (encoding.priority != owt::base::NetworkPriority::kDefault) {
                switch (encoding.priority) {
                  case owt::base::NetworkPriority::kVeryLow:
                    param.network_priority = webrtc::Priority::kVeryLow;
                    break;
                  case owt::base::NetworkPriority::kLow:
                    param.network_priority = webrtc::Priority::kLow;
                    break;
                  case owt::base::NetworkPriority::kMedium:
                    param.network_priority = webrtc::Priority::kMedium;
                    break;
                  case owt::base::NetworkPriority::kHigh:
                    param.network_priority = webrtc::Priority::kHigh;
                    break;
                  default:
                    break;
                }
              }
              param.active = encoding.active;
              transceiver_init.send_encodings.push_back(param);
            }
          }
          AddTransceiver(track, transceiver_init);
        }
        CreateOffer();
      },
      on_failure);
}
void ConferencePeerConnectionChannel::OnNetworksChanged() {
  RTC_LOG(LS_INFO) << "ConferencePeerConnectionChannel::OnNetworksChanged";
}
void ConferencePeerConnectionChannel::OnStreamError(
    const std::string& error_message) {
  std::shared_ptr<const Exception> e(
      new Exception(ExceptionType::kConferenceUnknown, error_message));
  std::shared_ptr<Stream> error_stream;
  for (auto its = observers_.begin(); its != observers_.end(); ++its) {
    RTC_LOG(LS_INFO) << "On stream error.";
    (*its).get().OnStreamError(error_stream, e);
  }
  if (published_stream_) {
    Unpublish(GetSessionId(), nullptr, nullptr);
    error_stream = published_stream_;
  }
  if (subscribed_stream_) {
    Unsubscribe(GetSessionId(), nullptr, nullptr);
    error_stream = subscribed_stream_;
  }
  if (error_stream == nullptr) {
    RTC_DCHECK(false);
    return;
  }
}
std::function<void()> ConferencePeerConnectionChannel::RunInEventQueue(
    std::function<void()> func) {
  if (!func)
    return nullptr;
  std::weak_ptr<ConferencePeerConnectionChannel> weak_this = shared_from_this();
  return [func, weak_this] {
    auto that = weak_this.lock();
    if (!that)
      return;
    that->event_queue_->PostTask([func] { func(); });
  };
}
void ConferencePeerConnectionChannel::ResetCallbacks() {
  publish_success_callback_ = nullptr;
  subscribe_success_callback_ = nullptr;
  failure_callback_ = nullptr;
}
void ConferencePeerConnectionChannel::ClosePeerConnection() {
  RTC_LOG(LS_INFO) << "Close peer connection.";
  std::lock_guard<std::mutex> locker(release_mutex_);
  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
}
bool ConferencePeerConnectionChannel::IsMediaStreamEnded(
    MediaStreamInterface* stream) const {
  RTC_CHECK(stream);
  for (auto track : stream->GetAudioTracks()) {
    if (track->state() == webrtc::AudioTrackInterface::TrackState::kLive) {
      return false;
    }
  }
  for (auto track : stream->GetVideoTracks()) {
    if (track->state() == webrtc::VideoTrackInterface::TrackState::kLive) {
      return false;
    }
  }
  return true;
}
}  // namespace conference
}  // namespace owt

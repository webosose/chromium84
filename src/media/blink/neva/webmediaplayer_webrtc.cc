// Copyright 2020 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "media/blink/neva/webmediaplayer_webrtc.h"

#include "cc/layers/layer.h"
#include "cc/layers/video_layer.h"
#include "cc/trees/layer_tree_host.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/media_util.h"
#include "media/blink/neva/stream_texture_interface.h"
#include "media/blink/neva/video_frame_provider_impl.h"
#include "media/blink/neva/video_util_neva.h"
#include "media/neva/media_platform_api.h"
#include "media/neva/media_preferences.h"
#include "media/webrtc/neva/webrtc_pass_through_video_decoder.h"
#include "third_party/blink/public/web/modules/media/webmediaplayer_util.h"
#include "third_party/blink/public/web/modules/mediastream/web_media_stream_renderer_factory.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/modules/mediastream/webmediaplayer_ms_compositor.h"

namespace media {

#define BIND_TO_RENDER_LOOP_VIDEO_FRAME_PROVIDER(function) \
  (DCHECK(main_task_runner_->BelongsToCurrentThread()),    \
   BindToCurrentLoop(                                      \
       base::Bind(function, (this->video_frame_provider_impl_->AsWeakPtr()))))

#define BIND_TO_RENDER_LOOP(function)                   \
  (DCHECK(main_task_runner_->BelongsToCurrentThread()), \
   media::BindToCurrentLoop(base::Bind(function, weak_ptr_this_)))

WebMediaPlayerWebRTC::WebMediaPlayerWebRTC(
    blink::WebLocalFrame* frame,
    blink::WebMediaPlayerClient* client,
    blink::WebMediaPlayerDelegate* delegate,
    std::unique_ptr<media::MediaLog> media_log,
    std::unique_ptr<blink::WebMediaStreamRendererFactory> renderer_factory,
    scoped_refptr<base::SingleThreadTaskRunner> main_render_task_runner,
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
    scoped_refptr<base::SingleThreadTaskRunner> compositor_task_runner,
    scoped_refptr<base::SingleThreadTaskRunner> media_task_runner,
    scoped_refptr<base::TaskRunner> worker_task_runner,
    media::GpuVideoAcceleratorFactories* gpu_factories,
    const blink::WebString& sink_id,
    blink::CreateSurfaceLayerBridgeCB create_bridge_callback,
    std::unique_ptr<blink::WebVideoFrameSubmitter> submitter,
    blink::WebMediaPlayer::SurfaceLayerMode surface_layer_mode,
    const StreamTextureFactoryCreateCB& stream_texture_factory_create_cb,
    std::unique_ptr<WebMediaPlayerParamsNeva> params_neva)
    : blink::WebMediaPlayerMS(frame,
                              client,
                              delegate,
                              std::move(media_log),
                              std::move(renderer_factory),
                              main_render_task_runner,
                              io_task_runner,
                              compositor_task_runner,
                              media_task_runner,
                              worker_task_runner,
                              gpu_factories,
                              sink_id,
                              std::move(create_bridge_callback),
                              std::move(submitter),
                              surface_layer_mode),
      additional_contents_scale_(params_neva->additional_contents_scale()),
      main_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      app_id_(params_neva->application_id().Utf8()),
      create_media_platform_api_cb_(
          params_neva->override_create_media_platform_api()),
      create_video_window_cb_(params_neva->get_create_video_window_callback()),
      weak_factory_this_(this) {
  LOG(INFO) << __func__ << " delegate_id_: " << delegate_id_;

  weak_ptr_this_ = weak_factory_this_.GetWeakPtr();

  video_frame_provider_impl_ = std::make_unique<media::VideoFrameProviderImpl>(
      stream_texture_factory_create_cb, compositor_task_runner);
  video_frame_provider_impl_->SetWebLocalFrame(frame);
  video_frame_provider_impl_->SetWebMediaPlayerClient(client);

  delegate_->DidMediaCreated(delegate_id_,
                             !params_neva->use_unlimited_media_policy());
}

WebMediaPlayerWebRTC::~WebMediaPlayerWebRTC() {
  LOG(INFO) << __func__ << " delegate_id_: " << delegate_id_;

  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  compositor_task_runner_->DeleteSoon(FROM_HERE,
                                      std::move(video_frame_provider_impl_));

  if (media_platform_api_)
    media_platform_api_->Finalize();
}

blink::WebMediaPlayer::LoadTiming WebMediaPlayerWebRTC::Load(
    LoadType load_type,
    const blink::WebMediaPlayerSource& source,
    CorsMode cors_mode) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  DCHECK(source.IsMediaStream());

  is_loading_ = true;
  pending_load_type_ = load_type;
  pending_stream_ = source.GetAsMediaStream();
  pending_cors_mode_ = cors_mode;

  delegate_->DidMediaActivationNeeded(delegate_id_);

  return LoadTiming::kDeferred;
}

void WebMediaPlayerWebRTC::Play() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (!has_activation_permit_) {
    status_on_suspended_ = StatusOnSuspended::PlayingStatus;
    if (!client_->IsSuppressedMediaPlay())
      delegate_->DidMediaActivationNeeded(delegate_id_);
    return;
  }
  blink::WebMediaPlayerMS::Play();
}

void WebMediaPlayerWebRTC::Pause() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (is_suspended_) {
    status_on_suspended_ = StatusOnSuspended::PausedStatus;
    return;
  }

  // call base-class implementation
  blink::WebMediaPlayerMS::Pause();

  paused_time_ = base::TimeDelta::FromMillisecondsD(CurrentTime());
}

void WebMediaPlayerWebRTC::SetRate(double rate) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (!has_activation_permit_) {
    if (!client_->IsSuppressedMediaPlay())
      delegate_->DidMediaActivationNeeded(delegate_id_);
    return;
  }

  blink::WebMediaPlayerMS::SetRate(rate);
}

void WebMediaPlayerWebRTC::SetVolume(double volume) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  blink::WebMediaPlayerMS::SetVolume(volume);
}

void WebMediaPlayerWebRTC::EnteredFullscreen() {
  geometry_update_helper_->SetFullscreenMode(true);
}

void WebMediaPlayerWebRTC::ExitedFullscreen() {
  geometry_update_helper_->SetFullscreenMode(false);
}

void WebMediaPlayerWebRTC::OnFrameHidden() {
  LOG(INFO) << __func__ << " : delegate_id_: " << delegate_id_;

  blink::WebMediaPlayerMS::OnFrameHidden();

  SuspendInternal();
}

void WebMediaPlayerWebRTC::OnFrameShown() {
  LOG(INFO) << __func__ << " : delegate_id_: " << delegate_id_;

  blink::WebMediaPlayerMS::OnFrameShown();

  ResumeInternal();
}

void WebMediaPlayerWebRTC::OnMediaActivationPermitted() {
  // If we already have activation permit, just skip.
  if (has_activation_permit_) {
    delegate_->DidMediaActivated(delegate_id_);
    return;
  }

  has_activation_permit_ = true;

  if (is_loading_) {
    OnLoadPermitted();
    return;
  }

  Play();

  client_->RequestPlay();
  delegate_->DidMediaActivated(delegate_id_);
}

void WebMediaPlayerWebRTC::OnVideoWindowCreated(
    const ui::VideoWindowInfo& info) {
  VLOG(1) << __func__;
  video_window_info_ = info;
  video_frame_provider_impl_->SetOverlayPlaneId(info.window_id);
  if (media_platform_api_)
    media_platform_api_->SetMediaLayerId(info.native_window_id);

  ContinuePlayerWithWindowId();
}

void WebMediaPlayerWebRTC::OnVideoWindowDestroyed() {
  VLOG(1) << __func__;
  video_window_info_ = base::nullopt;
  video_window_client_receiver_.reset();
}

void WebMediaPlayerWebRTC::OnVideoWindowGeometryChanged(const gfx::Rect& rect) {
#if defined(NEVA_VIDEO_HOLE)
  geometry_update_helper_->SetMediaLayerGeometry(rect);
#endif
}

void WebMediaPlayerWebRTC::OnVideoWindowVisibilityChanged(bool visibility) {
  VLOG(1) << __func__;
#if defined(NEVA_VIDEO_HOLE)
  geometry_update_helper_->SetMediaLayerVisibility(visibility);
#endif
}

void WebMediaPlayerWebRTC::SetRenderMode(
    blink::WebMediaPlayer::RenderMode mode) {
  if (render_mode_ == mode)
    return;

  render_mode_ = mode;

  if (is_render_mode_texture()) {
    video_frame_provider_impl_->SetFrameType(VideoFrameProviderImpl::kHole);
#if defined(USE_VIDEO_TEXTURE)
    if (media_platform_api_ && gfx::VideoTexture::IsSupported())
      media_platform_api_->SwitchToAutoLayout();
#endif
  } else {
#if defined(NEVA_VIDEO_HOLE)
    video_frame_provider_impl_->SetFrameType(VideoFrameProviderImpl::kHole);
#endif
  }
}

void WebMediaPlayerWebRTC::SetDisableAudio(bool disable) {
  if (media_platform_api_)
    media_platform_api_->SetDisableAudio(disable);
}

bool WebMediaPlayerWebRTC::HandleVideoFrame(
    const scoped_refptr<media::VideoFrame>& video_frame) {
  // For local stream video frames contains raw data in I420 format.
  // So decoding is not needed hence we return the same to parent
  // WebMediaPlayerMS class for rendering using chromium video layer.
  // For remote streams we pass the buffer to platform media pipeline
  // for decoding and rendering.
  int video_codec = 0;
  bool is_encoded = video_frame->metadata()->GetInteger(
      VideoFrameMetadata::CODEC_ID, &video_codec);
  CompositorType compositor_type = (is_encoded ?
      CompositorType::kVideoFrameProviderImpl :
      CompositorType::kWebMediaPlayerMSCompositor);
  if (compositor_type_ != compositor_type)
    compositor_type_ = compositor_type;

  if (!is_encoded)
    return false;

  if (media_player_status_cb_.is_null() &&
      !video_frame->metadata()->media_player_status_cb.is_null()) {
    media_player_status_cb_ =
        video_frame->metadata()->media_player_status_cb;
  }

  if (is_suspended_) {
    pending_encoded_frames_.clear();
    return true;
  }

  main_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&WebMediaPlayerWebRTC::HandleEncodedFrame,
                                weak_ptr_this_, video_frame));
  return true;
}

void WebMediaPlayerWebRTC::TriggerResize() {
  if (compositor_type_ == CompositorType::kVideoFrameProviderImpl) {
    blink::WebSize natural_size = NaturalSize();
    gfx::Size gfx_size(natural_size.width, natural_size.height);

    video_frame_provider_impl_->SetNaturalVideoSize(gfx_size);
    if (video_window_remote_)
      video_window_remote_->SetNaturalVideoSize(gfx_size);

    video_frame_provider_impl_->UpdateVideoFrame();
  }

  blink::WebMediaPlayerMS::TriggerResize();
}

void WebMediaPlayerWebRTC::OnFirstFrameReceived(
    media::VideoRotation video_rotation,
    bool is_opaque) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (is_loading_) {
    is_loading_ = false;
    delegate_->DidMediaActivated(delegate_id_);
  }

  has_first_frame_ = true;

  OnRotationChanged(video_rotation);
  OnOpacityChanged(is_opaque);

  SetReadyState(WebMediaPlayer::kReadyStateHaveMetadata);
  SetReadyState(WebMediaPlayer::kReadyStateHaveEnoughData);

  TriggerResize();
  ResetCanvasCache();
}

void WebMediaPlayerWebRTC::OnRotationChanged(VideoRotation video_rotation) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  VLOG(1) << __func__;

  video_rotation_ = video_rotation;
  video_transformation_ = {video_rotation_, 0};

  CreateVideoLayerInternal();

  // Request first frame for video
  if (!media_player_status_cb_.is_null())
    media_player_status_cb_.Run(
        VideoFrameMetadata::StatusType::kKeyFrameRequest);
}

void WebMediaPlayerWebRTC::HandleEncodedFrame(
    const scoped_refptr<media::VideoFrame>& encoded_frame) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  // The pipeline operation here might be incorrect, but encoded frame
  // couldn't be handled somewhere else.
  if (pipeline_status_ != media::PIPELINE_OK) {
    LOG(ERROR) << __func__ << " : pipeline_status error";
    return;
  }

  StartMediaPipeline(encoded_frame);

  {
    base::AutoLock auto_lock(frame_lock_);

    // While pipeline is initializing all pending encoded frames will be
    // removed after receiving a new key frame. So, set correct timestamp
    const base::TimeDelta incoming_timestamp = encoded_frame->timestamp();
    if (!pipeline_running_ &&
        encoded_frame->metadata()->IsTrue(VideoFrameMetadata::KEY_FRAME)) {
      start_timestamp_ = incoming_timestamp;
      encoded_frame->set_timestamp(base::TimeDelta());
      pending_encoded_frames_.clear();
    } else {
      if (start_timestamp_ == media::kNoTimestamp)
        start_timestamp_ = incoming_timestamp;
      const base::TimeDelta elapsed_timestamp =
          incoming_timestamp - start_timestamp_;
      encoded_frame->set_timestamp(elapsed_timestamp);
    }
    pending_encoded_frames_.push_back(encoded_frame);
  }

  if (pipeline_running_) {
    media_task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&WebMediaPlayerWebRTC::OnPipelineFeed, weak_ptr_this_));
  }
}

void WebMediaPlayerWebRTC::CreateMediaPlatformAPI() {
  LOG(INFO) << __func__;

  DestroyMediaPlatformAPI();

  // Create MediaPlatformAPI
  if (create_media_platform_api_cb_) {
    media_platform_api_ = create_media_platform_api_cb_.Run(
        media_task_runner_, client_->IsVideo(), app_id_,
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnNaturalVideoSizeChanged),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnResumed),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnSuspended),
        BIND_TO_RENDER_LOOP_VIDEO_FRAME_PROVIDER(
            &VideoFrameProviderImpl::ActiveRegionChanged),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnPipelineError));
  } else {
    media_platform_api_ = media::MediaPlatformAPI::Create(
        media_task_runner_, client_->IsVideo(), app_id_,
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnNaturalVideoSizeChanged),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnResumed),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnSuspended),
        BIND_TO_RENDER_LOOP_VIDEO_FRAME_PROVIDER(
            &VideoFrameProviderImpl::ActiveRegionChanged),
        BIND_TO_RENDER_LOOP(&WebMediaPlayerWebRTC::OnPipelineError));
  }

  if (!media_platform_api_)
    return;

  media_platform_api_->SetMediaPreferences(
      MediaPreferences::Get()->GetRawMediaPreferences());
  media_platform_api_->SetMediaCodecCapabilities(
      MediaPreferences::Get()->GetMediaCodecCapabilities());

  if (video_window_info_)
    media_platform_api_->SetMediaLayerId(video_window_info_->native_window_id);

  geometry_update_helper_.reset(new VideoHoleGeometryUpdateHelper(
      client_, additional_contents_scale_,
      base::BindRepeating(&media::MediaPlatformAPI::SetDisplayWindow,
                          media_platform_api_),
      base::BindRepeating(&media::MediaPlatformAPI::SetVisibility,
                          media_platform_api_)));

  SetDisableAudio(true);

  SetRenderMode(get_client()->RenderMode());
}

void WebMediaPlayerWebRTC::DestroyMediaPlatformAPI() {
  LOG(INFO) << __func__;

  if (!media_platform_api_)
    return;

  media_platform_api_->Finalize();

  // Make sure to stop the pipeline so there's no more media threads running.
  base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                            base::WaitableEvent::InitialState::NOT_SIGNALED);
  media_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&base::WaitableEvent::Signal, base::Unretained(&event)));
  event.Wait();

  media_platform_api_ = nullptr;
}

void WebMediaPlayerWebRTC::StartMediaPipeline(
    const scoped_refptr<media::VideoFrame>& input_frame) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  VideoCodec video_codec = media::kUnknownVideoCodec;
  input_frame->metadata()->GetInteger(VideoFrameMetadata::CODEC_ID,
                                      reinterpret_cast<int*>(&video_codec));
  if (media_platform_api_ && video_codec_ == video_codec)
    return;

  if (!input_frame->metadata()->IsTrue(VideoFrameMetadata::KEY_FRAME)) {
    if (!media_player_status_cb_.is_null())
      media_player_status_cb_.Run(
          VideoFrameMetadata::StatusType::kKeyFrameRequest);
    return;
  }

  video_codec_ = video_codec;

  CreateMediaPlatformAPI();

  media_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&WebMediaPlayerWebRTC::InitMediaPlatformAPI,
                                weak_ptr_this_, input_frame));
}

void WebMediaPlayerWebRTC::InitMediaPlatformAPI(
    const scoped_refptr<media::VideoFrame>& input_frame) {
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  // audio data handling and rendering path is separate. We are leaving
  // it to be taken care by Chromium now. So, we dont need any audio config
  AudioDecoderConfig audio_config;
  VideoDecoderConfig video_config = GetVideoConfig(input_frame);

  LOG(INFO) << __func__
            << " : natural_size: " << video_config.natural_size().ToString();

  media_platform_api_->Initialize(
      audio_config, video_config,
      base::Bind(&WebMediaPlayerWebRTC::OnMediaPlatformAPIInitialized,
                 weak_factory_this_.GetWeakPtr()));
}

void WebMediaPlayerWebRTC::ReleaseMediaPlatformAPI() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  VLOG(1) << __func__;

  is_destroying_ = true;

  {
    base::AutoLock auto_lock(frame_lock_);
    pending_encoded_frames_.clear();
  }

  compositor_->ReplaceCurrentFrameWithACopy();

  DestroyMediaPlatformAPI();

  pipeline_running_ = false;
  pipeline_status_ = media::PIPELINE_OK;
}

void WebMediaPlayerWebRTC::OnPipelineFeed() {
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  if (is_destroying_)
    return;

  std::vector<scoped_refptr<media::VideoFrame>> encoded_frames;
  {
    base::AutoLock auto_lock(frame_lock_);
    encoded_frames.swap(pending_encoded_frames_);
  }

  for (const auto& encoded_frame : encoded_frames) {
    scoped_refptr<media::DecoderBuffer> buffer = media::DecoderBuffer::CopyFrom(
        encoded_frame->data(0), encoded_frame->data_size(0));
    buffer->set_timestamp(encoded_frame->timestamp());
    buffer->set_is_key_frame(
        encoded_frame->metadata()->IsTrue(VideoFrameMetadata::KEY_FRAME));
    media_platform_api_->Feed(buffer, FeedType::kVideo);
  }
}

void WebMediaPlayerWebRTC::SuspendInternal() {
  LOG(INFO) << __func__ << " : delegate_id_: " << delegate_id_;

  if (is_suspended_)
    return;

  status_on_suspended_ = Paused() ? StatusOnSuspended::PausedStatus
                                  : StatusOnSuspended::PlayingStatus;
  if (media_platform_api_) {
    SuspendReason reason = client_->IsSuppressedMediaPlay()
                               ? SuspendReason::kBackgrounded
                               : SuspendReason::kSuspendedByPolicy;
    media_platform_api_->Suspend(reason);
  }

  is_suspended_ = true;
  has_activation_permit_ = false;

  // TODO(neva): also need to set STORAGE_BLACK for NEVA_VIDEO_HOLE ?
  if (HasVideo() && is_render_mode_texture())
    video_frame_provider_impl_->SetFrameType(VideoFrameProviderImpl::kBlack);

  // Usually we wait until OnSuspended(), but send DidMediaSuspended()
  // immediately when media_platform_api_ is null.
  if (!media_platform_api_)
    delegate_->DidMediaSuspended(delegate_id_);
}

void WebMediaPlayerWebRTC::ResumeInternal() {
  LOG(INFO) << __func__ << " : delegate_id_: " << delegate_id_;

  if (!is_suspended_)
    return;

  is_suspended_ = false;

  if (!media_player_status_cb_.is_null())
    media_player_status_cb_.Run(
        VideoFrameMetadata::StatusType::kKeyFrameRequest);

  media::RestorePlaybackMode restore_playback_mode =
      (status_on_suspended_ == StatusOnSuspended::PausedStatus)
          ? media::RestorePlaybackMode::kPaused
          : media::RestorePlaybackMode::kPlaying;

  if (media_platform_api_) {
    media_platform_api_->Resume(paused_time_, restore_playback_mode);
  } else {
    // Usually we wait until OnResumed(), but send DidMediaActivated()
    // immediately when media_platform_api_ is null.
    delegate_->DidMediaActivated(delegate_id_);
  }
}

void WebMediaPlayerWebRTC::CreateVideoLayerInternal() {
  VLOG(1) << __func__;

  get_client()->SetCcLayer(nullptr);

  if (video_layer_)
    video_layer_->StopUsingProvider();

  if (compositor_type_ == CompositorType::kWebMediaPlayerMSCompositor) {
    ReleaseMediaPlatformAPI();
    video_layer_ = cc::VideoLayer::Create(compositor_.get(), video_rotation_);
  } else {
    video_layer_ = cc::VideoLayer::Create(video_frame_provider_impl_.get(),
                                          video_rotation_);
  }

  video_layer_->SetContentsOpaque(opaque_);
  get_client()->SetCcLayer(video_layer_.get());
}

void WebMediaPlayerWebRTC::OnLoadPermitted() {
  // call base-class implementation
  if (!EnsureVideoWindowCreated()) {
    pending_load_media_ = true;
    return;
  }

  ContinuePlayerWithWindowId();
}

void WebMediaPlayerWebRTC::OnNaturalVideoSizeChanged(
    const gfx::Size& natural_video_size) {
  VLOG(1) << __func__
          << " natural_video_size: " << natural_video_size.ToString();

  if (natural_video_size_ == natural_video_size)
    return;

  natural_video_size_ = natural_video_size;
  video_frame_provider_impl_->SetNaturalVideoSize(natural_video_size_);

  geometry_update_helper_->SetNaturalVideoSize(natural_video_size_);

  if (video_window_remote_)
    video_window_remote_->SetNaturalVideoSize(natural_video_size_);
}

void WebMediaPlayerWebRTC::OnResumed() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

#if defined(NEVA_VIDEO_HOLE)
  geometry_update_helper_->UpdateVideoHoleBoundary();
#endif

  client_->RequestSeek(paused_time_.InSecondsF());

  if (status_on_suspended_ == StatusOnSuspended::PausedStatus) {
    Pause();
    status_on_suspended_ = StatusOnSuspended::UnknownStatus;
  } else {
    Play();
    client_->RequestPlay();
  }

  if (HasVideo() && is_render_mode_texture())
    video_frame_provider_impl_->SetFrameType(VideoFrameProviderImpl::kTexture);

  delegate_->DidMediaActivated(delegate_id_);
}

void WebMediaPlayerWebRTC::OnSuspended() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  delegate_->DidMediaSuspended(delegate_id_);
}

// It returns true if video window is already created and can be continued
// to next step.
bool WebMediaPlayerWebRTC::EnsureVideoWindowCreated() {
  VLOG(1) << __func__;

  if (video_window_info_)
    return true;

  // |is_bound()| would be true if we already requested so we need to just wait
  // for response
  if (video_window_client_receiver_.is_bound())
    return false;

  mojo::PendingRemote<ui::mojom::VideoWindowClient> pending_client;
  video_window_client_receiver_.Bind(
      pending_client.InitWithNewPipeAndPassReceiver());

  mojo::PendingRemote<ui::mojom::VideoWindow> pending_window_remote;
  create_video_window_cb_.Run(
      std::move(pending_client),
      pending_window_remote.InitWithNewPipeAndPassReceiver(),
      ui::VideoWindowParams());
  video_window_remote_.Bind(std::move(pending_window_remote));
  return false;
}

void WebMediaPlayerWebRTC::ContinuePlayerWithWindowId() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  VLOG(1) << __func__;

  if (pending_load_media_) {
    // call base-class implementation
    blink::WebMediaPlayerSource pending_source(pending_stream_);
    blink::WebMediaPlayerMS::Load(pending_load_type_, pending_source,
                                  pending_cors_mode_);
    pending_load_media_ = false;
  }
}

void WebMediaPlayerWebRTC::OnMediaPlatformAPIInitialized(
    PipelineStatus status) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  LOG(INFO) << __func__ << " status : " << status;

  if (is_destroying_ || !media_platform_api_) {
    LOG(ERROR) << __func__ << " Is destroying";
    return;
  }

  pipeline_running_ = true;
  pipeline_status_ = status;

  media_platform_api_->SetPlaybackRate(1.0f);

  media_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&WebMediaPlayerWebRTC::OnPipelineFeed, weak_ptr_this_));
}

void WebMediaPlayerWebRTC::OnPipelineError(PipelineStatus status) {
  VLOG(1) << __func__ << " : delegate_id_: " << delegate_id_
          << " status : " << status;

  if (is_loading_) {
    is_loading_ = false;
    delegate_->DidMediaActivated(delegate_id_);
  }

  if (is_destroying_)
    return;

  if (!media_player_status_cb_.is_null() &&
      (status == PIPELINE_ERROR_ABORT ||
       status == DECODER_ERROR_RESOURCE_IS_RELEASED))
    media_player_status_cb_.Run(VideoFrameMetadata::StatusType::kPipelineError);

  {
    base::AutoLock auto_lock(frame_lock_);
    pending_encoded_frames_.clear();
  }

  compositor_->ReplaceCurrentFrameWithACopy();

  pipeline_running_ = false;
  pipeline_status_ = status;

  SetNetworkState(blink::PipelineErrorToNetworkState(status));

  RepaintInternal();
}

VideoDecoderConfig WebMediaPlayerWebRTC::GetVideoConfig(
    const scoped_refptr<media::VideoFrame>& video_frame) {
  VideoCodecProfile profile = media::VIDEO_CODEC_PROFILE_UNKNOWN;
  switch (video_codec_) {
    case media::kCodecH264:
      profile = media::H264PROFILE_MIN;
      break;
    case media::kCodecVP8:
      profile = media::VP8PROFILE_ANY;
      break;
    case media::kCodecVP9:
      profile = media::VP9PROFILE_MIN;
      break;
    default:
      // forgot to handle new encoded video format?
      NOTREACHED();
      break;
  }
  LOG(INFO) << __func__ << ", codec: " << video_codec_
            << ", name: " << media::GetCodecName(video_codec_);

  media::VideoDecoderConfig video_config(
      video_codec_, profile, media::VideoDecoderConfig::AlphaMode::kIsOpaque,
      media::VideoColorSpace(), media::kNoTransformation,
      video_frame->coded_size(), video_frame->visible_rect(),
      video_frame->natural_size(), media::EmptyExtraData(),
      media::EncryptionScheme::kUnencrypted);
  video_config.set_live_stream(true);
  return video_config;
}

}  // namespace media

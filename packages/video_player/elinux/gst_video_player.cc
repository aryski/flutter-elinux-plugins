// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gst_video_player.h"

#include <iostream>
#include <gst/app/gstappsink.h>  // appsink support

// ---------------------------------------------------------------------------
//  Fixed raw pipeline: shmsrc → videoconvert → RGBA → appsink
// ---------------------------------------------------------------------------
namespace {
constexpr char kCustomPipeline[] =
    "shmsrc socket-path=/tmp/shmsock "
    "! video/x-raw,format=I420,width=1280,height=720,framerate=30/1 "
    "! videoconvert ! video/x-raw,format=RGBA "
    "! appsink name=sink emit-signals=true sync=false";
}  // namespace

// ---------------------------------------------------------------------------
//  Construction / destruction
// ---------------------------------------------------------------------------
GstVideoPlayer::GstVideoPlayer(
    const std::string& uri, std::unique_ptr<VideoPlayerStreamHandler> handler)
    : stream_handler_(std::move(handler)) {
  gst_.pipeline      = nullptr;
  gst_.playbin       = nullptr;
  gst_.video_convert = nullptr;
  gst_.video_sink    = nullptr;
  gst_.output        = nullptr;
  gst_.bus           = nullptr;
  gst_.buffer        = nullptr;

  uri_ = ParseUri(uri);  // parsed but unused by raw pipeline

  if (!CreatePipeline()) {
    std::cerr << "Failed to create a pipeline\n";
    DestroyPipeline();
  }
}

GstVideoPlayer::~GstVideoPlayer() {
#ifdef USE_EGL_IMAGE_DMABUF
  UnrefEGLImage();
#endif
  Stop();
  DestroyPipeline();
}

// static ---------------------------------------------------------------------
void GstVideoPlayer::GstLibraryLoad()   { gst_init(nullptr, nullptr); }
void GstVideoPlayer::GstLibraryUnload() { gst_deinit(); }

// ---------------------------------------------------------------------------
//  appsink callback (now a class static → full access to privates)
// ---------------------------------------------------------------------------
GstFlowReturn GstVideoPlayer::NewSampleHandler(GstAppSink* sink,
                                               gpointer      user_data) {
  auto* self = static_cast<GstVideoPlayer*>(user_data);

  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_ERROR;

  GstBuffer* buffer = gst_sample_get_buffer(sample);
  if (!buffer) { gst_sample_unref(sample); return GST_FLOW_ERROR; }

  // Update resolution if it changed
  if (GstCaps* caps = gst_sample_get_caps(sample)) {
    GstStructure* s = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    gst_structure_get_int(s, "width",  &w);
    gst_structure_get_int(s, "height", &h);
    if (w != self->width_ || h != self->height_) {
      self->width_  = w;
      self->height_ = h;
      self->pixels_.reset(new uint32_t[w * h]);
      std::cout << "Pixel buffer resized: " << w << "×" << h << '\n';
    }
  }

  { // store the latest buffer thread-safely
    std::lock_guard<std::shared_mutex> lock(self->mutex_buffer_);
    if (self->gst_.buffer) gst_buffer_unref(self->gst_.buffer);
    self->gst_.buffer = gst_buffer_ref(buffer);
  }

  self->stream_handler_->OnNotifyFrameDecoded();
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// ---------------------------------------------------------------------------
//  Pipeline creation
// ---------------------------------------------------------------------------
bool GstVideoPlayer::CreatePipeline() {
  GError* err = nullptr;
  gst_.pipeline = gst_parse_launch(kCustomPipeline, &err);
  if (!gst_.pipeline) {
    std::cerr << "gst_parse_launch failed: "
              << (err ? err->message : "unknown") << '\n';
    if (err) g_error_free(err);
    return false;
  }

  // Bus for EOS / errors
  gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
  gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, nullptr);

  // appsink
  gst_.video_sink = gst_bin_get_by_name(GST_BIN(gst_.pipeline), "sink");
  if (!gst_.video_sink) {
    std::cerr << "appsink element not found (name=sink)\n";
    return false;
  }
  g_signal_connect(gst_.video_sink, "new-sample",
                   G_CALLBACK(GstVideoPlayer::NewSampleHandler), this);

  return true;
}

// ---------------------------------------------------------------------------
//  Preroll helper (original logic)
// ---------------------------------------------------------------------------
bool GstVideoPlayer::Preroll() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change state to PAUSED\n";
    return false;
  }
  GstState state;
  if (gst_element_get_state(gst_.pipeline, &state, nullptr,
                            GST_CLOCK_TIME_NONE) == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed waiting for PAUSED\n";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  Init / Play / Pause / Stop
// ---------------------------------------------------------------------------
bool GstVideoPlayer::Init() {
  if (!gst_.pipeline) return false;
  if (!Preroll()) {
    DestroyPipeline();
    return false;
  }
  GetVideoSize(width_, height_);
  pixels_.reset(new uint32_t[width_ * height_]);
  stream_handler_->OnNotifyInitialized();
  return true;
}

bool GstVideoPlayer::Play() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change state to PLAYING\n";
    return false;
  }
  stream_handler_->OnNotifyPlaying(true);
  return true;
}

bool GstVideoPlayer::Pause() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change state to PAUSED\n";
    return false;
  }
  stream_handler_->OnNotifyPlaying(false);
  return true;
}

bool GstVideoPlayer::Stop() {
  if (gst_element_set_state(gst_.pipeline, GST_STATE_READY) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change state to READY\n";
    return false;
  }
  stream_handler_->OnNotifyPlaying(false);
  return true;
}

// ---------------------------------------------------------------------------
//  Volume / rate / seek  (rate/seek logic identical to original)
// ---------------------------------------------------------------------------
bool GstVideoPlayer::SetVolume(double volume) { volume_ = volume; return false; }

bool GstVideoPlayer::SetPlaybackRate(double rate) {
  if (rate <= 0) { std::cerr << "Bad rate\n"; return false; }
  gint64 pos_ns = 0;
  if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &pos_ns))
    return false;
  if (!gst_element_seek(gst_.pipeline, rate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                        GST_SEEK_TYPE_SET, pos_ns, GST_SEEK_TYPE_END,
                        GST_CLOCK_TIME_NONE)) {
    std::cerr << "gst_element_seek failed\n";
    return false;
  }
  playback_rate_ = rate;
  return true;
}

bool GstVideoPlayer::SetSeek(int64_t position_ms) {
  const gint64 ns = position_ms * 1000 * 1000;
  if (!gst_element_seek(gst_.pipeline, playback_rate_, GST_FORMAT_TIME,
                        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                                  GST_SEEK_FLAG_KEY_UNIT),
                        GST_SEEK_TYPE_SET, ns,
                        GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to seek\n";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  Duration / position
// ---------------------------------------------------------------------------
int64_t GstVideoPlayer::GetDuration() {
  gint64 dur_ns = 0;
  return gst_element_query_duration(gst_.pipeline, GST_FORMAT_TIME, &dur_ns)
             ? dur_ns / GST_MSECOND
             : -1;
}

int64_t GstVideoPlayer::GetCurrentPosition() {
  gint64 pos_ns = 0;
  if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &pos_ns))
    return -1;

  // EOS handling
  {
    std::unique_lock<std::mutex> lock(mutex_event_completed_);
    if (is_completed_) {
      is_completed_ = false;
      lock.unlock();
      if (auto_repeat_)
        SetSeek(0);
      else
        stream_handler_->OnNotifyCompleted();
    }
  }
  return pos_ns / GST_MSECOND;
}

// ---------------------------------------------------------------------------
//  Frame extraction (works via appsink)
// ---------------------------------------------------------------------------
const uint8_t* GstVideoPlayer::GetFrameBuffer() {
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) return nullptr;
  const uint32_t bytes = width_ * height_ * 4;
  gst_buffer_extract(gst_.buffer, 0, pixels_.get(), bytes);
  return reinterpret_cast<const uint8_t*>(pixels_.get());
}

#ifdef USE_EGL_IMAGE_DMABUF
void* GstVideoPlayer::GetEGLImage(void*, void*) { return nullptr; }
void  GstVideoPlayer::UnrefEGLImage()            {}
#endif

// ---------------------------------------------------------------------------
//  DestroyPipeline
// ---------------------------------------------------------------------------
void GstVideoPlayer::DestroyPipeline() {
  if (gst_.pipeline) gst_element_set_state(gst_.pipeline, GST_STATE_NULL);

  if (gst_.buffer) { gst_buffer_unref(gst_.buffer); gst_.buffer = nullptr; }
  if (gst_.bus)    { gst_object_unref(gst_.bus);    gst_.bus    = nullptr; }
  if (gst_.pipeline) { gst_object_unref(gst_.pipeline); gst_.pipeline = nullptr; }

  gst_.video_sink    = nullptr;
  gst_.video_convert = nullptr;
  gst_.playbin       = nullptr;
  gst_.output        = nullptr;
}

// ---------------------------------------------------------------------------
//  Helpers (ParseUri, GetVideoSize)  – unchanged
// ---------------------------------------------------------------------------
std::string GstVideoPlayer::ParseUri(const std::string& uri) {
  if (gst_uri_is_valid(uri.c_str())) return uri;
  gchar* u = gst_filename_to_uri(uri.c_str(), nullptr);
  if (!u) { std::cerr << "Failed to open " << uri << '\n'; return uri; }
  std::string ret(u); g_free(u); return ret;
}

void GstVideoPlayer::GetVideoSize(int32_t& w, int32_t& h) {
  if (!gst_.video_sink) return;
  GstPad* pad = gst_element_get_static_pad(gst_.video_sink, "sink");
  if (!pad) return;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  if (!caps) { gst_object_unref(pad); return; }
  GstStructure* s = gst_caps_get_structure(caps, 0);
  gst_structure_get_int(s, "width",  &w);
  gst_structure_get_int(s, "height", &h);
  gst_caps_unref(caps);
  gst_object_unref(pad);
}

// ---------------------------------------------------------------------------
//  Bus handler (same as original)
// ---------------------------------------------------------------------------
GstBusSyncReply GstVideoPlayer::HandleGstMessage(GstBus*, GstMessage* msg,
                                                 gpointer data) {
  auto* self = static_cast<GstVideoPlayer*>(data);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      { std::lock_guard<std::mutex> lock(self->mutex_event_completed_);
        self->is_completed_ = true; }
      break;
    case GST_MESSAGE_WARNING:
    case GST_MESSAGE_ERROR: {
      gchar* dbg = nullptr; GError* err = nullptr;
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING)
        gst_message_parse_warning(msg, &err, &dbg);
      else
        gst_message_parse_error(msg, &err, &dbg);
      g_printerr("%s from %s: %s\n",
                 GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING ? "WARNING" : "ERROR",
                 GST_OBJECT_NAME(msg->src), err->message);
      g_printerr("Details: %s\n", dbg);
      g_free(dbg); g_error_free(err);
      break;
    }
    default: break;
  }
  gst_message_unref(msg);
  return GST_BUS_DROP;
}

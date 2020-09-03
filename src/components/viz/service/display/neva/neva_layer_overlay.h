// Copyright 2019-2020 LG Electronics, Inc.
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
//
// Logic is partially copied from DCLayerOverlayProcessor in dc_layer_overlay.h

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_NEVA_NEVA_LAYER_OVERLAY_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_NEVA_NEVA_LAYER_OVERLAY_H_

#include "base/containers/flat_map.h"
#include "components/viz/common/quads/render_pass.h"
#include "ui/gfx/geometry/rect_f.h"
#include "gpu/ipc/common/surface_handle.h"

#if defined(USE_NEVA_MEDIA)
namespace ui {
class VideoWindowController;
}  // namespace ui
#endif  // defined(USE_NEVA_MEDIA)

namespace viz {
class DisplayResourceProvider;

class NevaLayerOverlayProcessor {
 public:
  NevaLayerOverlayProcessor();
  NevaLayerOverlayProcessor(gpu::SurfaceHandle surface_handle);
  ~NevaLayerOverlayProcessor();

  void Process(DisplayResourceProvider* resource_provider,
               const gfx::RectF& display_rect,
               RenderPassList* render_passes,
               gfx::Rect* overlay_damage_rect,
               gfx::Rect* damage_rect);

  void ClearOverlayState() {
    previous_frame_underlay_rect_ = gfx::Rect();
    previous_frame_underlay_occlusion_ = gfx::Rect();
  }

 private:
  bool IsVideoHoleDrawQuad(DisplayResourceProvider* resource_provider,
                           const gfx::RectF& display_rect,
                           QuadList::ConstIterator quad_list_begin,
                           QuadList::ConstIterator quad);
  void AddPunchThroughRectIfNeeded(RenderPassId id, const gfx::Rect& rect);

  // Returns an iterator to the element after |it|.
  QuadList::Iterator ProcessRenderPassDrawQuad(RenderPass* render_pass,
                                               gfx::Rect* damage_rect,
                                               QuadList::Iterator it);

  void ProcessRenderPass(DisplayResourceProvider* resource_provider,
                         const gfx::RectF& display_rect,
                         RenderPass* render_pass,
                         bool is_root,
                         gfx::Rect* overlay_damage_rect,
                         gfx::Rect* damage_rect);
  bool ProcessForUnderlay(const gfx::RectF& display_rect,
                          RenderPass* render_pass,
                          const gfx::Rect& quad_rectangle,
                          const gfx::RectF& occlusion_bounding_box,
                          const QuadList::Iterator& it,
                          bool is_root,
                          gfx::Rect* damage_rect,
                          gfx::Rect* this_frame_underlay_rect,
                          gfx::Rect* this_frame_underlay_occlusion);

  gpu::SurfaceHandle surface_handle_;
#if defined(USE_NEVA_MEDIA)
  ui::VideoWindowController* video_window_controller_;
#endif
  gfx::Rect previous_frame_underlay_rect_;
  gfx::Rect previous_frame_underlay_occlusion_;
  gfx::RectF previous_display_rect_;
  bool processed_overlay_in_frame_ = false;

  // Store information about clipped punch-through rects in target space for
  // non-root render passes. These rects are used to clear the corresponding
  // areas in parent render passes.
  base::flat_map<RenderPassId, std::vector<gfx::Rect>>
      pass_punch_through_rects_;

  DISALLOW_COPY_AND_ASSIGN(NevaLayerOverlayProcessor);
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_DISPLAY_NEVA_NEVA_LAYER_OVERLAY_H_

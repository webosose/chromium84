// Copyright 2015-2020 LG Electronics, Inc.
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

#include "neva/app_runtime/webapp_window.h"

#include <climits>
#include <cstdint>

#include "base/strings/utf_string_conversions.h"
#include "content/browser/web_contents/web_contents_view.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "neva/app_runtime/public/app_runtime_event.h"
#include "neva/app_runtime/public/webapp_window_delegate.h"
#include "neva/app_runtime/public/window_group_configuration.h"
#include "neva/app_runtime/ui/desktop_aura/app_runtime_desktop_native_widget_aura.h"
#include "ui/aura/window.h"
#include "ui/aura/window_tree_host.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/base/ui_base_types.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/events/blink/web_input_event.h"
#include "ui/gfx/geometry/size.h"
#include "ui/platform_window/neva/window_group_configuration.h"
#include "ui/platform_window/neva/xinput_types.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/test/desktop_test_views_delegate.h"
#include "ui/views/view.h"
#include "ui/views/widget/desktop_aura/desktop_native_cursor_manager.h"
#include "ui/views/widget/desktop_aura/desktop_window_tree_host.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/wm/core/cursor_manager.h"

namespace neva_app_runtime {
namespace {

const std::size_t kUInt32BitLength = sizeof(std::uint32_t) * CHAR_BIT;

const int kKeyboardHeightMargin = 10;
const int kKeyboardAnimationTime = 600;

inline bool ExistsInUiKeyMaskType(std::uint32_t key_mask) {
  ui::KeyMask key_masks = static_cast<ui::KeyMask>(key_mask);
  switch (key_masks) {
    case ui::KeyMask::kHome:
    case ui::KeyMask::kBack:
    case ui::KeyMask::kExit:
    case ui::KeyMask::kNavigationLeft:
    case ui::KeyMask::kNavigationRight:
    case ui::KeyMask::kNavigationUp:
    case ui::KeyMask::kNavigationDown:
    case ui::KeyMask::kNavigationOk:
    case ui::KeyMask::kNumericKeys:
    case ui::KeyMask::kRemoteColorRed:
    case ui::KeyMask::kRemoteColorGreen:
    case ui::KeyMask::kRemoteColorYellow:
    case ui::KeyMask::kRemoteColorBlue:
    case ui::KeyMask::kRemoteProgrammeGroup:
    case ui::KeyMask::kRemotePlaybackGroup:
    case ui::KeyMask::kRemoteTeletextGroup:
    case ui::KeyMask::kLocalLeft:
    case ui::KeyMask::kLocalRight:
    case ui::KeyMask::kLocalUp:
    case ui::KeyMask::kLocalDown:
    case ui::KeyMask::kLocalOk:
    case ui::KeyMask::kRemoteMagnifierGroup:
    case ui::KeyMask::kMinimalPlaybackGroup:
    case ui::KeyMask::kGuide:
      return true;
    default:
      LOG(WARNING) << __func__ << "(): unknown key mask value: " << key_mask;
      return false;
  }
}

bool IsLandscape(int rotation) {
  return rotation == 0 || rotation == 180;
}

bool IsPortrait(int rotation) {
  return rotation == 90 || rotation == 270;
}

}  // namespace

// Scroll app window if needed when vkb is visible by moving native view
// in case app didn't consume wheel event
class WebAppScrollObserver
    : public content::RenderWidgetHost::InputEventObserver {
 public:
  WebAppScrollObserver(WebAppWindow* window) : window_(window) {
    window_->GetWebContents()
        ->GetMainFrame()
        ->GetView()
        ->GetRenderWidgetHost()
        ->AddInputEventObserver(this);
  }
  ~WebAppScrollObserver() override {
    if (window_->GetWebContents())
      window_->GetWebContents()
          ->GetMainFrame()
          ->GetView()
          ->GetRenderWidgetHost()
          ->RemoveInputEventObserver(this);
  }

  void OnInputEventAck(blink::mojom::InputEventResultSource source,
                       blink::mojom::InputEventResultState state,
                       const blink::WebInputEvent& event) override {
    if (event.GetType() == blink::WebInputEvent::Type::kMouseWheel &&
        state != blink::mojom::InputEventResultState::kConsumed) {
      int input_panel_real_height =
          window_->input_panel_height() > 0 ? window_->input_panel_height() : 0;
      gfx::Rect bounds =
          window_->GetWebContents()->GetContentNativeView()->bounds();
      const blink::WebMouseWheelEvent& wheel_event =
          *static_cast<const blink::WebMouseWheelEvent*>(&event);
      window_->GetWebContents()->GetContentNativeView()->SetBounds(gfx::Rect(
          bounds.x(),
          (static_cast<int>(wheel_event.delta_y) > 0)
              ? std::min(bounds.y() + static_cast<int>(wheel_event.delta_y), 0)
              : std::max(bounds.y() + static_cast<int>(wheel_event.delta_y),
                         -input_panel_real_height),
          bounds.width(), bounds.height()));
    }
  }

 private:
  WebAppWindow* window_;

  DISALLOW_COPY_AND_ASSIGN(WebAppScrollObserver);
};

WebAppWindow::WebAppWindow(const CreateParams& params,
                           WebAppWindowDelegate* delegate)
    : delegate_(delegate),
      params_(params),
      rect_(params.bounds),
      window_host_state_(ui::WidgetState::UNINITIALIZED),
      window_host_state_about_to_change_(ui::WidgetState::UNINITIALIZED) {
  InitWindow();

  ComputeScaleFactor();

  if (params.web_contents) {
    SetupWebContents(static_cast<content::WebContents*>(params.web_contents));
  }

  if (display::Screen::GetScreen()->GetNumDisplays() > 0) {
    current_rotation_ =
        display::Screen::GetScreen()->GetPrimaryDisplay().RotationAsDegree();
  }

  display::Screen::GetScreen()->AddObserver(this);
}

WebAppWindow::~WebAppWindow() {
  display::Screen::GetScreen()->RemoveObserver(this);
  if (desktop_native_widget_aura_) {
    desktop_native_widget_aura_->OnWebAppWindowRemoved();
    desktop_native_widget_aura_->SetNativeEventDelegate(nullptr);
  }
}

void WebAppWindow::OnDisplayMetricsChanged(const display::Display& display,
                                           uint32_t metrics) {
  if (metrics & display::DisplayObserver::DISPLAY_METRIC_ROTATION) {
    // Get new rotation from primary display
    int screen_rotation =
        display::Screen::GetScreen()->GetPrimaryDisplay().RotationAsDegree();

    if (screen_rotation == current_rotation_)
      return;

    // FIXME: Wayland shell surface already reports resize request but for now
    // LSM fails to report that resize is due to rotation change. We need this
    // information in order do swap width and height because otherwise by
    // default resizing window is denied by allow_window_resize_ flag. If LSM
    // would report it correctly then this width and height swapping would not
    // need to be done here but on ozone wayland level.
    if (!rect_.IsEmpty()) {
      if (IsLandscape(current_rotation_)) {
        if (IsPortrait(screen_rotation)) {
          Resize(rect_.height(), rect_.width());
        }
      } else if (IsPortrait(current_rotation_)) {
        if (IsLandscape(screen_rotation)) {
          Resize(rect_.height(), rect_.width());
        }
      }
    }

    current_rotation_ = screen_rotation;
  }
}

bool WebAppWindow::HandleEvent(AppRuntimeEvent* app_runtime_event) {
  if (delegate_)
    return delegate_->HandleEvent(app_runtime_event);

  return false;
}

void WebAppWindow::OnDesktopNativeWidgetAuraRemoved() {
  desktop_native_widget_aura_ = nullptr;
}

bool WebAppWindow::IsInputPanelVisible() const {
  return input_panel_visible_;
}

void WebAppWindow::SetUseVirtualKeyboard(bool enable) {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->SetUseVirtualKeyboard(enable);
}

void WebAppWindow::SetupWebContents(content::WebContents* web_contents) {
  if (web_contents && !webview_) {
    webview_ = new views::WebView(web_contents->GetBrowserContext());
    SetLayoutManager(std::make_unique<views::FillLayout>());
    AddChildView(webview_);
  }
  if (webview_) {
    webview_->SetWebContents(web_contents);
    // Do layout after web contents has been attached so that correct bounds
    // will be probagated to content layer and render widget host.
    Layout();
  }

  web_contents_ = web_contents;
  Observe(web_contents_);
}

content::WebContents* WebAppWindow::GetWebContents() {
  return web_contents_;
}

// static
WidgetState WebAppWindow::ToExposedWidgetStateType(ui::WidgetState state) {
  switch (state) {
    case ui::WidgetState::SHOW:
      return WidgetState::SHOW;
    case ui::WidgetState::HIDE:
      return WidgetState::HIDE;
    case ui::WidgetState::FULLSCREEN:
      return WidgetState::FULLSCREEN;
    case ui::WidgetState::MAXIMIZED:
      return WidgetState::MAXIMIZED;
    case ui::WidgetState::MINIMIZED:
      return WidgetState::MINIMIZED;
    case ui::WidgetState::RESTORE:
      return WidgetState::RESTORE;
    case ui::WidgetState::ACTIVE:
      return WidgetState::ACTIVE;
    case ui::WidgetState::INACTIVE:
      return WidgetState::INACTIVE;
    case ui::WidgetState::RESIZE:
      return WidgetState::RESIZE;
    case ui::WidgetState::DESTROYED:
      return WidgetState::DESTROYED;
    default:
      return WidgetState::UNINITIALIZED;
  }
}

// static
ui::WidgetState WebAppWindow::FromExposedWidgetStateType(WidgetState state) {
  switch (state) {
    case WidgetState::SHOW:
      return ui::WidgetState::SHOW;
    case WidgetState::HIDE:
      return ui::WidgetState::HIDE;
    case WidgetState::FULLSCREEN:
      return ui::WidgetState::FULLSCREEN;
    case WidgetState::MAXIMIZED:
      return ui::WidgetState::MAXIMIZED;
    case WidgetState::MINIMIZED:
      return ui::WidgetState::MINIMIZED;
    case WidgetState::RESTORE:
      return ui::WidgetState::RESTORE;
    case WidgetState::ACTIVE:
      return ui::WidgetState::ACTIVE;
    case WidgetState::INACTIVE:
      return ui::WidgetState::INACTIVE;
    case WidgetState::RESIZE:
      return ui::WidgetState::RESIZE;
    case WidgetState::DESTROYED:
      return ui::WidgetState::DESTROYED;
    default:
      return ui::WidgetState::UNINITIALIZED;
  }
}

void WebAppWindow::SetWindowHostState(ui::WidgetState state) {
  if (!host_)
    return;

  switch (state) {
  case ui::WidgetState::FULLSCREEN:
    host_->SetFullscreen(true);
    break;
  case ui::WidgetState::MAXIMIZED:
    if (host_->IsFullscreen())
      host_->SetFullscreen(false);

    host_->Maximize();
    break;
  case ui::WidgetState::MINIMIZED:
    host_->Minimize();
    break;
  default:
    break;
  }
}

ui::WidgetState WebAppWindow::GetWindowHostState() const {
  return window_host_state_;
}

ui::WidgetState WebAppWindow::GetWindowHostStateAboutToChange() const {
  return window_host_state_about_to_change_;
}

void WebAppWindow::Activate() {
  VLOG(1) << __func__;
  if (!contents_swapped_) {
    VLOG(1) << __func__ << ": contents not swapped, waiting";
    pending_activate_ = true;
    return;
  }
  pending_activate_ = false;
  if (widget_) {
    VLOG(1) << __func__ << ": contents swapped, activating";
    widget_->Activate();
  }
}

void WebAppWindow::SetBounds(int x, int y, int width, int height) {
  if (widget_)
    widget_->SetBounds(gfx::Rect(x, y, width, height));
}

void WebAppWindow::SetWindowProperty(const std::string& name,
                                     const std::string& value) {
  window_property_list_[name] = value;

  if (!host_)
    return;

  LOG(INFO) << __func__
            << "(): SetWindowProperty is called. Name: " << name.c_str()
            << " Value: " << value.c_str();

  host_->AsWindowTreeHost()->SetWindowProperty(name, value);
}

void WebAppWindow::SetLocationHint(gfx::LocationHint value) {
  if (!host_)
    return;

  aura::WindowTreeHost* wth = host_->AsWindowTreeHost();
  DCHECK(wth) << "aura::WindowTreeHost is unavailable";

  wth->SetLocationHint(value);
}

void WebAppWindow::Show() {
  VLOG(1) << __func__;
  if (!contents_swapped_) {
    VLOG(1) << __func__ << " waiting swap";
    pending_show_ = true;
    return;
  }
  pending_show_ = false;
  if (widget_) {
    VLOG(1) << __func__ << " ready to show";
    widget_->Show();
  }
}

void WebAppWindow::Hide() {
  contents_swapped_ = false;
  pending_show_ = false;
  if (widget_)
    widget_->Hide();
}

void WebAppWindow::Minimize() {
  if (widget_)
    widget_->Minimize();
}

void WebAppWindow::Restore() {
  if (widget_)
    widget_->Restore();
}

void WebAppWindow::Close() {
  widget_closed_ = true;
  contents_swapped_ = false;
  pending_show_ = false;

  if (widget_)
    widget_->Close();
  else if (deferred_deleting_)
    delete this;
}

void WebAppWindow::CursorVisibilityChanged(bool visible) {
  if (cursor_manager_.get())
    cursor_manager_->SetPlatformCursorVisibility(visible);
  if (delegate_)
    delegate_->CursorVisibilityChanged(visible);
}

int WebAppWindow::CalculateTextInputOverlappedHeight(const gfx::Rect& rect) {
  int shift_height = 0;
  if (!host_)
    return shift_height;

  ui::InputMethod* ime = host_->AsWindowTreeHost()->GetInputMethod();
  if (!ime || !ime->GetTextInputClient())
    return shift_height;

  gfx::Rect input_bounds = ime->GetTextInputClient()->GetTextInputBounds();
  gfx::Rect scaled_rect =
      gfx::Rect(rect.x() / scale_factor_, rect.y() / scale_factor_,
                rect.width() / scale_factor_, rect.height() / scale_factor_);

  if (input_bounds.Intersects(scaled_rect))
    shift_height = input_bounds.bottom() - scaled_rect.y();

  return shift_height;
}

bool WebAppWindow::CanShiftContent(int shift_height) {
  if (!host_)
    return false;

  ui::InputMethod* ime = host_->AsWindowTreeHost()->GetInputMethod();
  if (!ime || !ime->GetTextInputClient())
    return false;

  gfx::Rect input_bounds = ime->GetTextInputClient()->GetTextInputBounds();

  return input_bounds.y() - rect_.y() >= shift_height;
}

void WebAppWindow::InputPanelVisibilityChanged(bool visible) {
  if (visible) {
    web_app_scroll_observer_.reset(new WebAppScrollObserver(this));
    CheckShiftContent();
  } else {
    web_app_scroll_observer_.reset();
    RestoreContentByY();
  }

  input_panel_visible_ = visible;
}

void WebAppWindow::InputPanelRectChanged(int32_t x,
                                         int32_t y,
                                         uint32_t width,
                                         uint32_t height) {
  input_panel_rect_.SetRect(x, y, width, height);
  if (input_panel_visible_)
    CheckShiftContent();
}

void WebAppWindow::CheckShiftContent() {
  if (input_panel_rect_.height()) {
    gfx::Rect panel_rect_margin = gfx::Rect(
        input_panel_rect_.x(), input_panel_rect_.y() - kKeyboardHeightMargin,
        input_panel_rect_.width(),
        input_panel_rect_.height() + kKeyboardHeightMargin);
    int shift_height = CalculateTextInputOverlappedHeight(panel_rect_margin);
    if (shift_height != 0 && CanShiftContent(shift_height))
        ShiftContentByY(shift_height);
  }
}

void WebAppWindow::ShiftContentByY(int shift_height) {
  if (!web_contents_ || web_contents_->IsBeingDestroyed() ||
      !web_contents_->GetContentNativeView())
    return;

  if (shift_height == 0)
    return;

  gfx::Rect bounds = web_contents_->GetContentNativeView()->bounds();
  shift_y_ = bounds.y() - shift_height;

  if (!is_shifted_content_) {
    native_view_bounds_for_restoring_ = bounds;
    is_shifted_content_ = true;
  }

  if (bounds.y()) {
    UpdateViewportYCallback();
  } else {
    if (viewport_timer_.IsRunning())
      viewport_timer_.Reset();
    else
      viewport_timer_.Start(
          FROM_HERE, base::TimeDelta::FromMilliseconds(kKeyboardAnimationTime),
          this, &WebAppWindow::UpdateViewportYCallback);
  }
}

void WebAppWindow::UpdateViewportYCallback() {
  gfx::Rect bounds = web_contents_->GetContentNativeView()->bounds();
  web_contents_->GetContentNativeView()->SetBounds(gfx::Rect(
      bounds.x(), shift_y_, bounds.width(), bounds.height()));
}

void WebAppWindow::RestoreContentByY() {
  if (is_shifted_content_) {
    shift_y_ = native_view_bounds_for_restoring_.y();
    UpdateViewportYCallback();
    is_shifted_content_ = false;
  }
}

void WebAppWindow::KeyboardEnter() {
  AppRuntimeEvent event(AppRuntimeEvent::Type::FocusIn);
  HandleEvent(&event);
}

void WebAppWindow::KeyboardLeave() {
  AppRuntimeEvent event(AppRuntimeEvent::Type::FocusOut);
  HandleEvent(&event);
}

void WebAppWindow::WindowHostClose() {
  AppRuntimeEvent event(AppRuntimeEvent::Type::Close);
  HandleEvent(&event);
}

void WebAppWindow::WindowHostExposed() {
  AppRuntimeEvent event(AppRuntimeEvent::Type::Expose);
  HandleEvent(&event);
}

void WebAppWindow::WindowHostStateChanged(ui::WidgetState new_state) {
  if (window_host_state_ == new_state)
    return;

  window_host_state_ = new_state;

  AppRuntimeEvent app_runtime_event(AppRuntimeEvent::Type::WindowStateChange);
  HandleEvent(&app_runtime_event);
}

void WebAppWindow::WindowHostStateAboutToChange(ui::WidgetState state) {
  window_host_state_about_to_change_ = state;

  AppRuntimeEvent app_runtime_event(AppRuntimeEvent::Type::WindowStateAboutToChange);
  HandleEvent(&app_runtime_event);
}

views::Widget* WebAppWindow::GetWidget() {
  return widget_;
}

const views::Widget* WebAppWindow::GetWidget() const {
  return widget_;
}

bool WebAppWindow::CanResize() const {
  return false;
}

bool WebAppWindow::CanMaximize() const {
  return true;
}

bool WebAppWindow::ShouldShowWindowTitle() const {
  return true;
}

bool WebAppWindow::ShouldShowCloseButton() const {
  return false;
}

views::View* WebAppWindow::GetContentsView() {
  return this;
}

void WebAppWindow::ComputeScaleFactor() {
  int display_height =
      display::Screen::GetScreen()->GetPrimaryDisplay().bounds().height();
  scale_factor_ = static_cast<float>(display_height) / rect_.height();
}

void WebAppWindow::Resize(int width, int height) {
  if (rect_.width() != width || rect_.height() != height) {
    rect_.set_width(width);
    rect_.set_height(height);

    ComputeScaleFactor();

    if (widget_)
      widget_->SetSize(gfx::Size(width, height));
  }
}

void WebAppWindow::SetInputRegion(const std::vector<gfx::Rect>& region) {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->SetInputRegion(region);
}

// static
ui::KeyMask WebAppWindow::FromExposedKeyMaskType(KeyMask key_mask) {
  if (key_mask == KeyMask::kDefault)
    return ui::KeyMask::kDefault;

  std::uint32_t key_masks = static_cast<std::uint32_t>(key_mask);
  std::uint32_t result = 0;

  for (std::size_t i = 0; i < kUInt32BitLength; ++i) {
    std::uint32_t mask = 1 << i;
    if ((key_masks & mask) && ExistsInUiKeyMaskType(mask))
      result |= mask;
  }

  return static_cast<ui::KeyMask>(result);
}

void WebAppWindow::SetGroupKeyMask(ui::KeyMask key_mask) {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->SetGroupKeyMask(key_mask);
}

void WebAppWindow::SetKeyMask(ui::KeyMask key_mask, bool set) {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->SetKeyMask(key_mask, set);
}

void WebAppWindow::SetOpacity(float opacity) {
  DCHECK(opacity >= 0.f && opacity <= 1.f) << "invalid opacity value provided";

  if (!host_)
    return;

  host_->SetOpacity(opacity);
}

void WebAppWindow::SetRootLayerOpacity(float opacity) {
  DCHECK(opacity >= 0.f && opacity <= 1.f) << "invalid opacity value provided";

  if (!host_)
    return;

  host_->AsWindowTreeHost()->SetRootLayerOpacity(opacity);
}

void WebAppWindow::SetCustomCursor(CustomCursorType type,
                                   const std::string& path,
                                   int hotspot_x,
                                   int hotspot_y) {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->SetCustomCursor(type, path, hotspot_x, hotspot_y);
}

void WebAppWindow::SetWindowTitle(const base::string16& title) {
  title_ = title;
  if (widget_)
    widget_->UpdateWindowTitle();
}

base::string16 WebAppWindow::GetWindowTitle() const {
  return title_;
}

void WebAppWindow::WindowClosing() {
  if (delegate_)
    delegate_->OnWindowClosing();
}

void WebAppWindow::CloseWindow() {
  if (!host_)
    return;

  // Remove a window, but keep a renderer alive.
  if (host_)
    host_->Close();
}

void WebAppWindow::DeleteDelegate() {
  // In case of deferred_deleting_, WebAppWindow will be deleted by owner
  if (!deferred_deleting_ || widget_closed_)
    delete this;
  else {
    if (webview_) {
      delete webview_;
      webview_ = nullptr;
    }

    host_ = nullptr;
    widget_ = nullptr;
  }
}

void WebAppWindow::OnMouseEvent(ui::MouseEvent* event) {
  switch (event->type()) {
    case ui::EventType::ET_MOUSE_PRESSED:
    case ui::EventType::ET_MOUSE_DRAGGED:
    case ui::EventType::ET_MOUSE_RELEASED:
    case ui::EventType::ET_MOUSE_MOVED:
    case ui::EventType::ET_MOUSE_ENTERED:
    case ui::EventType::ET_MOUSE_EXITED:
    case ui::EventType::ET_MOUSE_CAPTURE_CHANGED:
    case ui::EventType::ET_MOUSEWHEEL: {
      break;
    }
    default:
      NOTIMPLEMENTED();
  }
}

void WebAppWindow::OnKeyEvent(ui::KeyEvent* event) {
  switch (event->type()) {
    case ui::EventType::ET_KEY_PRESSED:
      {
        AppRuntimeKeyEvent app_runtime_key_event(
            AppRuntimeEvent::Type::KeyPress, event->key_code());
        HandleEvent(&app_runtime_key_event);
      }
      break;
    case ui::EventType::ET_KEY_RELEASED:
      {
        AppRuntimeKeyEvent app_runtime_key_event(
            AppRuntimeEvent::Type::KeyRelease, event->key_code());
        HandleEvent(&app_runtime_key_event);
      }
      break;
    default:
      LOG(WARNING) << __func__ << "(): unknown key event type: " <<
          event->type();
      break;
  }
}

void WebAppWindow::InitWindow() {
  widget_ = new views::Widget();
  views::Widget::InitParams init_params(params_.type);
  // Update params width and and height with current window rect because bounds
  // might have changed due to resize or orientation change. This is needed
  // for keep alive apps.when WebAppWindow is not getting destroyed but
  // reinitialized.
  params_.bounds.set_width(rect_.width());
  params_.bounds.set_height(rect_.height());
  init_params.bounds = params_.bounds;

  init_params.delegate = this;
  init_params.show_state = params_.show_state;
  desktop_native_widget_aura_ = new AppRuntimeDesktopNativeWidgetAura(this);
  desktop_native_widget_aura_->SetNativeEventDelegate(this);
  init_params.native_widget = desktop_native_widget_aura_;
  init_params.desktop_window_tree_host = host_ =
      views::DesktopWindowTreeHost::Create(widget_,
                                           desktop_native_widget_aura_);

  if (host_) {
    aura::WindowTreeHost* wth = host_->AsWindowTreeHost();

    wth->AddPreTargetHandler(this);

    views::DesktopNativeCursorManager* native_cursor_manager =
        new views::DesktopNativeCursorManager();
    cursor_manager_ = std::make_unique<wm::CursorManager>(
        std::unique_ptr<wm::NativeCursorManager>(native_cursor_manager));
    native_cursor_manager->AddHost(wth);

    aura::client::SetCursorClient(wth->window(), cursor_manager_.get());
  }

  widget_->Init(std::move(init_params));

  if (params_.show_state == ui::SHOW_STATE_FULLSCREEN)
    widget_->SetFullscreen(true);

  SetLocationHint(params_.location_hint);
}

void WebAppWindow::RecreateIfNeeded() {
  if (widget_ || host_ || !GetWebContents())
    return;

  SetupWebContents(GetWebContents());

  InitWindow();

  if (!host_)
    return;

  aura::WindowTreeHost* wth = host_->AsWindowTreeHost();
  for (const auto& property : window_property_list_)
    wth->SetWindowProperty(property.first, property.second);
}
void WebAppWindow::CompositorResumeDrawing() {
  if (!host_)
    return;

  LOG(INFO) << __func__ << "(): Reset compositor painting";

  host_->AsWindowTreeHost()->CompositorResumeDrawing();
}

void WebAppWindow::XInputActivate(const std::string& type) {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->XInputActivate(type);
}

void WebAppWindow::XInputDeactivate() {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->XInputDeactivate();
}

void WebAppWindow::XInputInvokeAction(uint32_t keysym,
                                      XInputKeySymbolType symbol_type,
                                      XInputEventType event_type) {
  if (!host_)
    return;

  ui::XInputKeySymbolType ui_symbol_type;
  switch (symbol_type) {
    case XInputKeySymbolType::XINPUT_QT_KEY_SYMBOL:
      ui_symbol_type = ui::XINPUT_QT_KEY_SYMBOL;
      break;
    case XInputKeySymbolType::XINPUT_NATIVE_KEY_SYMBOL:
      ui_symbol_type = ui::XINPUT_NATIVE_KEY_SYMBOL;
      break;
  };

  ui::XInputEventType ui_event_type;
  switch (event_type) {
    case XInputEventType::XINPUT_PRESS_AND_RELEASE:
      ui_event_type = ui::XINPUT_PRESS_AND_RELEASE;
      break;
    case XInputEventType::XINPUT_PRESS:
      ui_event_type = ui::XINPUT_PRESS;
      break;
    case XInputEventType::XINPUT_RELEASE:
      ui_event_type = ui::XINPUT_RELEASE;
      break;
  };

  host_->AsWindowTreeHost()->XInputInvokeAction(keysym, ui_symbol_type,
                                                ui_event_type);
}

int WebAppWindow::GetWidth() const {
  return widget_ ? widget_->GetWindowBoundsInScreen().width() : 0;
}

int WebAppWindow::GetHeight() const {
  return widget_ ? widget_->GetWindowBoundsInScreen().height() : 0;
}

void WebAppWindow::BeginPrepareStackForWebApp() {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->BeginPrepareStackForWebApp();
}

void WebAppWindow::FinishPrepareStackForWebApp() {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->FinishPrepareStackForWebApp();
}

void WebAppWindow::CreateGroup(const WindowGroupConfiguration& config) {
  if (!host_)
    return;

  ui::WindowGroupConfiguration ui_config = config;
  host_->AsWindowTreeHost()->CreateGroup(ui_config);
}

void WebAppWindow::AttachToGroup(const std::string& group,
                                 const std::string& layer) {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->AttachToGroup(group, layer);
}

void WebAppWindow::FocusGroupOwner() {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->FocusGroupOwner();
}

void WebAppWindow::FocusGroupLayer() {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->FocusGroupLayer();
}

void WebAppWindow::DetachGroup() {
  if (!host_)
    return;

  host_->AsWindowTreeHost()->DetachGroup();
}

void WebAppWindow::DidCompleteSwap() {
  contents_swapped_ = true;
  if (pending_activate_)
    Activate();
  if (pending_show_)
    Show();
}

}  // namespace neva_app_runtime

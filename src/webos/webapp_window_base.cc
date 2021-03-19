// Copyright 2016-2018 LG Electronics, Inc.
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

#include "webos/webapp_window_base.h"

#include "base/logging.h"
#include "components/viz/common/switches.h"
#include "neva/app_runtime/public/app_runtime_constants.h"
#include "neva/app_runtime/public/window_group_configuration.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/widget/desktop_aura/desktop_window_tree_host.h"
#include "webos/common/webos_types_conversion_utils.h"
#include "webos/webapp_window.h"
#include "webos/window_group_configuration.h"

#if defined(OS_WEBOS)
#include "components/viz/common/switches.h"
#endif

namespace webos {
namespace {

neva_app_runtime::CustomCursorType ToAppruntimeCursorType(CustomCursorType type) {
  switch (type) {
    case CUSTOM_CURSOR_NOT_USE :
      return neva_app_runtime::CustomCursorType::kNotUse;
    case CUSTOM_CURSOR_BLANK :
      return neva_app_runtime::CustomCursorType::kBlank;
    case CUSTOM_CURSOR_PATH :
      return neva_app_runtime::CustomCursorType::kPath;
  }

  NOTREACHED();
}

inline neva_app_runtime::XInputKeySymbolType ToAppruntimeXInputKeySymbolType(
    SpecialKeySymbolType symbol_type) {
  switch (symbol_type) {
    case QT_KEY_SYMBOL:
      return neva_app_runtime::XInputKeySymbolType::XINPUT_QT_KEY_SYMBOL;
    case NATIVE_KEY_SYMBOL:
      return neva_app_runtime::XInputKeySymbolType::XINPUT_NATIVE_KEY_SYMBOL;
  }
  NOTREACHED();
}

inline neva_app_runtime::XInputEventType ToAppruntimeXInputEventType(
    XInputEventType event_type) {
  switch (event_type) {
    case XINPUT_PRESS_AND_RELEASE:
      return neva_app_runtime::XInputEventType::XINPUT_PRESS_AND_RELEASE;
    case XINPUT_PRESS:
      return neva_app_runtime::XInputEventType::XINPUT_PRESS;
    case XINPUT_RELEASE:
      return neva_app_runtime::XInputEventType::XINPUT_RELEASE;
  }
  NOTREACHED();
}

inline gfx::LocationHint ToGfxLocationHint(
    WebAppWindowBase::LocationHint value) {
  switch (value) {
    case WebAppWindowBase::LocationHint::kUnknown:
      return gfx::LocationHint::kUnknown;
    case WebAppWindowBase::LocationHint::kNorth:
      return gfx::LocationHint::kNorth;
    case WebAppWindowBase::LocationHint::kWest:
      return gfx::LocationHint::kWest;
    case WebAppWindowBase::LocationHint::kSouth:
      return gfx::LocationHint::kSouth;
    case WebAppWindowBase::LocationHint::kEast:
      return gfx::LocationHint::kEast;
    case WebAppWindowBase::LocationHint::kCenter:
      return gfx::LocationHint::kCenter;
    case WebAppWindowBase::LocationHint::kNorthWest:
      return gfx::LocationHint::kNorthWest;
    case WebAppWindowBase::LocationHint::kNorthEast:
      return gfx::LocationHint::kNorthEast;
    case WebAppWindowBase::LocationHint::kSouthWest:
      return gfx::LocationHint::kSouthWest;
    case WebAppWindowBase::LocationHint::kSouthEast:
      return gfx::LocationHint::kSouthEast;
    default:
      NOTREACHED() << __func__ << "(): unknown location hint value: "
                   << static_cast<uint32_t>(value);
      return gfx::LocationHint::kUnknown;
  }
}

}  // namespace

///////////////////////////////////////////////////////////////////////////////
// WebAppWindowBase, public:

WebAppWindowBase::WebAppWindowBase() {}

WebAppWindowBase::~WebAppWindowBase() {
  DetachWebContents();
  if (webapp_window_) {
    webapp_window_->SetDelegate(nullptr);
    webapp_window_->Close();
  }
}

void WebAppWindowBase::InitWindow(int width, int height) {
  // We may call InitWindow again when the web app window size has changed.
  // I.e. this happens when we launch an application with a custom window size.
  // In this case we just destroy the previous window and create a new one.
  if (webapp_window_) {
    webapp_window_->SetDelegate(nullptr);
    webapp_window_->Close();
  }

  neva_app_runtime::WebAppWindow::CreateParams params;
  params.bounds.set_width(width);
  params.bounds.set_height(height);
  params.show_state = ui::SHOW_STATE_DEFAULT;
  params.type = views::Widget::InitParams::TYPE_WINDOW_FRAMELESS;
  webapp_window_ = new WebAppWindow(params);
  webapp_window_->SetDelegate(this);
  webapp_window_->BeginPrepareStackForWebApp();
}

void WebAppWindowBase::Show() {
  if (!webapp_window_) {
    LOG(INFO) << __PRETTY_FUNCTION__ << ": no webapp_window_";
    return;
  }

  webapp_window_->FinishPrepareStackForWebApp();
  webapp_window_->ActivateAndShow();
}

void WebAppWindowBase::Hide() {
  if (webapp_window_)
    webapp_window_->CloseWindow();
}

// TODO(sergey.kipet@lge.com): deprecated (to be excluded as soon as removed
// from every component it is called from, e.g. WAM)
unsigned WebAppWindowBase::GetWindowHandle() {
  NOTIMPLEMENTED() << "WebOS specific api of DesktopTreeWindowHostOzone";
  return 0;
}

void WebAppWindowBase::SetCustomCursor(CustomCursorType type,
                                       const std::string& path,
                                       int hotspot_x,
                                       int hotspot_y) {
  webapp_window_->SetCustomCursor(ToAppruntimeCursorType(type), path,
                                  hotspot_x, hotspot_y);
}

int WebAppWindowBase::DisplayWidth() const {
  if (display::Screen::GetScreen()->GetNumDisplays() > 0)
    return display::Screen::GetScreen()->GetPrimaryDisplay().bounds().width();
  return 0;
}

int WebAppWindowBase::DisplayHeight() const {
  if (display::Screen::GetScreen()->GetNumDisplays() > 0)
    return display::Screen::GetScreen()->GetPrimaryDisplay().bounds().height();
  return 0;
}

void WebAppWindowBase::AttachWebContents(void* web_contents) {
  if (webapp_window_) {
    webapp_window_->SetupWebContents(
        static_cast<content::WebContents*>(web_contents));
  }
}

void WebAppWindowBase::DetachWebContents() {
  if (webapp_window_)
    webapp_window_->SetupWebContents(nullptr);
}

void WebAppWindowBase::RecreatedWebContents() {
  if (webapp_window_)
    webapp_window_->CompositorResumeDrawing();
}

NativeWindowState WebAppWindowBase::GetWindowHostState() const {
  if (webapp_window_)
    return ToNativeWindowState(webapp_window_->GetWindowHostState());

  return NATIVE_WINDOW_DEFAULT;
}

NativeWindowState WebAppWindowBase::GetWindowHostStateAboutToChange() const {
  if (webapp_window_)
    return ToNativeWindowState(
        webapp_window_->GetWindowHostStateAboutToChange());

  return NATIVE_WINDOW_DEFAULT;
}

void WebAppWindowBase::SetWindowHostState(NativeWindowState state) {
  if (webapp_window_) {
    VLOG(1) << __PRETTY_FUNCTION__ << ": state=" << state;
    webapp_window_->SetWindowHostState(ToWidgetState(state));
  } else {
    LOG(INFO) << __PRETTY_FUNCTION__ << ": no webapp_window_";
  }
}

void WebAppWindowBase::SetInputRegion(const std::vector<gfx::Rect>& region) {
  if (webapp_window_)
    webapp_window_->SetInputRegion(region);
}

void WebAppWindowBase::SetKeyMask(WebOSKeyMask key_mask) {
  if (webapp_window_)
    webapp_window_->SetGroupKeyMask(ToKeyMask(key_mask));
}

void WebAppWindowBase::SetKeyMask(WebOSKeyMask key_mask, bool set) {
  if (webapp_window_)
    webapp_window_->SetKeyMask(ToKeyMask(key_mask), set);
}

void WebAppWindowBase::SetWindowProperty(const std::string& name,
                                         const std::string& value) {
  if (webapp_window_)
    webapp_window_->SetWindowProperty(name, value);
}

void WebAppWindowBase::SetLocationHint(LocationHint value) {
  if (webapp_window_)
    webapp_window_->SetLocationHint(ToGfxLocationHint(value));
}

void WebAppWindowBase::SetOpacity(float opacity) {
  // SetRootLayerOpacity instead of SetOpacity should be called for WebOS
  if (webapp_window_)
    webapp_window_->SetRootLayerOpacity(opacity);
}

void WebAppWindowBase::Resize(int width, int height) {
  if (webapp_window_)
    webapp_window_->Resize(width, height);
}

bool WebAppWindowBase::IsKeyboardVisible() {
  return webapp_window_->IsInputPanelVisible();
}

void WebAppWindowBase::SetUseVirtualKeyboard(bool enable) {
  if (webapp_window_)
    webapp_window_->SetUseVirtualKeyboard(enable);
}

void WebAppWindowBase::CreateWindowGroup(
    const WindowGroupConfiguration& config) {
  neva_app_runtime::WindowGroupConfiguration ui_config;

  ui_config.name = config.GetName();
  ui_config.is_anonymous = config.GetIsAnonymous();

  for (auto& it : config.GetLayers()) {
    neva_app_runtime::WindowGroupLayerConfiguration layer_config;
    layer_config.name = it.GetName();
    layer_config.z_order = it.GetZOrder();
    ui_config.layers.push_back(layer_config);
  }
  webapp_window_->CreateGroup(ui_config);
}

void WebAppWindowBase::AttachToWindowGroup(const std::string& group,
                                           const std::string& layer) {
  webapp_window_->AttachToGroup(group, layer);
}

void WebAppWindowBase::FocusWindowGroupOwner() {
  webapp_window_->FocusGroupOwner();
}

void WebAppWindowBase::FocusWindowGroupLayer() {
  webapp_window_->FocusGroupLayer();
}

void WebAppWindowBase::DetachWindowGroup() {
  webapp_window_->DetachGroup();
}

void WebAppWindowBase::OnWindowClosing() {
}

void WebAppWindowBase::XInputActivate(const std::string& type) {
  if (webapp_window_)
    webapp_window_->XInputActivate(type);
}

void WebAppWindowBase::XInputDeactivate() {
  if (webapp_window_)
    webapp_window_->XInputDeactivate();
}

void WebAppWindowBase::XInputInvokeAction(uint32_t keysym,
                                          SpecialKeySymbolType symbol_type,
                                          XInputEventType event_type) {
  if (webapp_window_)
    webapp_window_->XInputInvokeAction(
        keysym,
        ToAppruntimeXInputKeySymbolType(symbol_type),
        ToAppruntimeXInputEventType(event_type));
}

void WebAppWindowBase::WebAppWindowDestroyed() {
  webapp_window_ = nullptr;
}

}  // namespace webos

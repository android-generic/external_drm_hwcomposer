/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "drmhwc"

#include "DrmHwcThree.h"

#include <cinttypes>

#include "Utils.h"
#include "aidl/android/hardware/graphics/common/Dataspace.h"
#if __ANDROID_API__ >= 35
#include "aidl/android/hardware/graphics/common/DisplayHotplugEvent.h"
#endif

namespace aidl::android::hardware::graphics::composer3::impl {

using ::android::HwcDisplay;

DrmHwcThree::~DrmHwcThree() {
  /* Display deinit routine is handled by resource manager */
  GetResMan().DeInit();
}

void DrmHwcThree::Init(std::shared_ptr<IComposerCallback> callback) {
  composer_callback_ = std::move(callback);
  GetResMan().Init();
}

void DrmHwcThree::SendVsyncPeriodTimingChangedEventToClient(
    uint64_t display_id, int64_t timestamp) const {
  VsyncPeriodChangeTimeline timeline;
  timeline.newVsyncAppliedTimeNanos = timestamp;
  timeline.refreshRequired = false;
  timeline.refreshTimeNanos = 0;

  composer_callback_->onVsyncPeriodTimingChanged(static_cast<int64_t>(
                                                     display_id),
                                                 timeline);
}

void DrmHwcThree::SendRefreshEventToClient(uint64_t display_id) {
  composer_resources_->SetDisplayMustValidateState(display_id, true);
  composer_callback_->onRefresh(static_cast<int64_t>(display_id));
}

void DrmHwcThree::SendVsyncEventToClient(uint64_t display_id, int64_t timestamp,
                                         uint32_t vsync_period) const {
  composer_callback_->onVsync(static_cast<int64_t>(display_id), timestamp,
                              static_cast<int32_t>(vsync_period));
}

#if __ANDROID_API__ >= 35

void DrmHwcThree::SendHotplugEventToClient(
    hwc2_display_t display_id, DrmHwc::DisplayStatus display_status) {
  common::DisplayHotplugEvent event = common::DisplayHotplugEvent::DISCONNECTED;
  switch (display_status) {
    case DrmHwc::kDisconnected:
      event = common::DisplayHotplugEvent::DISCONNECTED;
      HandleDisplayHotplugEvent(static_cast<uint64_t>(display_id), false);
      break;
    case DrmHwc::kConnected:
      event = common::DisplayHotplugEvent::CONNECTED;
      HandleDisplayHotplugEvent(static_cast<uint64_t>(display_id), true);
      break;
    case DrmHwc::kLinkTrainingFailed:
      event = common::DisplayHotplugEvent::ERROR_INCOMPATIBLE_CABLE;
      break;
  }
  composer_callback_->onHotplugEvent(static_cast<int64_t>(display_id), event);
}

#else

void DrmHwcThree::SendHotplugEventToClient(
    hwc2_display_t display_id, DrmHwc::DisplayStatus display_status) {
  bool connected = display_status != DrmHwc::kDisconnected;
  HandleDisplayHotplugEvent(static_cast<uint64_t>(display_id), connected);
  composer_callback_->onHotplug(static_cast<int64_t>(display_id), connected);
}

#endif

void DrmHwcThree::CleanDisplayResources(uint64_t display_id) {
  DEBUG_FUNC();
  HwcDisplay* display = GetDisplay(display_id);
  if (display == nullptr) {
    return;
  }

  display->SetPowerMode(static_cast<int32_t>(PowerMode::OFF));

  size_t cache_size = 0;
  auto err = composer_resources_->GetDisplayClientTargetCacheSize(display_id,
                                                                  &cache_size);
  if (err != hwc3::Error::kNone) {
    ALOGE("%s: Could not clear target buffer cache for display: %" PRIu64,
          __func__, display_id);
    return;
  }

  for (size_t slot = 0; slot < cache_size; slot++) {
    buffer_handle_t buffer_handle = nullptr;
    auto buf_releaser = ComposerResources::CreateResourceReleaser(true);

    Buffer buf{};
    buf.slot = static_cast<int32_t>(slot);
    err = composer_resources_->GetDisplayClientTarget(display_id, buf,
                                                      &buffer_handle,
                                                      buf_releaser.get());
    if (err != hwc3::Error::kNone) {
      continue;
    }

    err = Hwc2toHwc3Error(
        display->SetClientTarget(buffer_handle, -1,
                                 static_cast<int32_t>(
                                     common::Dataspace::UNKNOWN),
                                 {}));
    if (err != hwc3::Error::kNone) {
      ALOGE(
          "%s: Could not clear slot %zu of the target buffer cache for "
          "display %" PRIu64,
          __func__, slot, display_id);
    }
  }
}

void DrmHwcThree::HandleDisplayHotplugEvent(uint64_t display_id,
                                            bool connected) {
  DEBUG_FUNC();
  if (!connected) {
    composer_resources_->RemoveDisplay(display_id);
    Displays().erase(display_id);
    return;
  }

  if (composer_resources_->HasDisplay(display_id)) {
    /* Cleanup existing display resources */
    CleanDisplayResources(display_id);
    composer_resources_->RemoveDisplay(display_id);
    Displays().erase(display_id);
  }
  composer_resources_->AddPhysicalDisplay(display_id);
}

}  // namespace aidl::android::hardware::graphics::composer3::impl

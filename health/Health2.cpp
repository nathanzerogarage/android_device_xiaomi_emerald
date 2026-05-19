/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "health-impl/Health.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/hardware/health/translate-ndk.h>
#include <health/utils.h>

#include "LinkedCallback.h"
#include "health-convert.h"

using std::string_literals::operator""s;

namespace aidl::android::hardware::health {

namespace {
// Wrap LinkedCallback::OnCallbackDied() into a void(void*).
void OnCallbackDiedWrapped(void* cookie) {
    LinkedCallback* linked = reinterpret_cast<LinkedCallback*>(cookie);
    linked->OnCallbackDied();
}
// Delete the owned cookie.
void onCallbackUnlinked(void* cookie) {
    LinkedCallback* linked = reinterpret_cast<LinkedCallback*>(cookie);
    delete linked;
}
}  // namespace

/*
// If you need to call healthd_board_init, construct the Health instance with
// the healthd_config after calling healthd_board_init:
class MyHealth : public Health {
  protected:
    MyHealth() : Health(CreateConfig()) {}
  private:
    static std::unique_ptr<healthd_config> CreateConfig() {
      auto config = std::make_unique<healthd_config>();
      ::android::hardware::health::InitHealthdConfig(config.get());
      healthd_board_init(config.get());
      return std::move(config);
    }
};
*/
void update_thread(std::shared_ptr<HealthSharedData> data) {
    LOG(ERROR) << "update_thread start";
    while (true) {
	data->wait();
        LOG(ERROR) << "update_thread do update";

        data->second_monitor_->updateValues();

        std::lock_guard<decltype(data->update_lock_)> lock(data->update_lock_);
        data->second_monitor_.swap(data->battery_monitor_);
    }
}

HealthSharedData::HealthSharedData(healthd_config *config) {
    battery_monitor_ = std::make_unique<::android::BatteryMonitor>();
    second_monitor_ = std::make_unique<::android::BatteryMonitor>();
    battery_monitor_->init(config);
    second_monitor_->init(config);
}

void HealthSharedData::wait() {
    std::unique_lock lk(wake_lock_);
    update_cv_.wait(lk);
    lk.unlock();
}

void HealthSharedData::update() {
    update_cv_.notify_one();
}

void HealthSharedData::OnHealthInfoChanged(const HealthInfo& health_info) {
    // Notify all callbacks
    std::unique_lock<decltype(callbacks_lock_)> lock(callbacks_lock_);
    // is_dead notifies a callback and return true if it is dead.
    auto is_dead = [&](const auto& linked) {
        auto res = linked->callback()->healthInfoChanged(health_info);
        return IsDeadObjectLogged(res);
    };
    auto it = std::remove_if(callbacks_.begin(), callbacks_.end(), is_dead);
    callbacks_.erase(it, callbacks_.end());  // calls unlinkToDeath on deleted callbacks.
    lock.unlock();
}

bool HealthSharedData::registerCallback(const std::shared_ptr<IHealthInfoCallback>& callback) {
    {
        std::lock_guard<decltype(callbacks_lock_)> lock(callbacks_lock_);
        auto linked_callback_result = LinkedCallback::Make(ref<Health>(), callback);
        if (!linked_callback_result.ok()) {
	    return false;
        }
        callbacks_[*linked_callback_result] = callback;
    }

    HealthInfo health_info;

    auto res = callback->healthInfoChanged(getHealthInfo());

    return true;
}

bool HealthSharedData::unregisterCallback(
        const std::shared_ptr<IHealthInfoCallback>& callback) {
    std::lock_guard<decltype(callbacks_lock_)> lock(callbacks_lock_);

    auto matches = [callback](const auto& cb) {
        return cb->asBinder() == callback->asBinder();  // compares binder object
    };
    bool removed = false;
    for (auto it = callbacks_.begin(); it != callbacks_.end();) {
        if (it->second->asBinder() == callback->asBinder()) {
            auto status = AIBinder_unlinkToDeath(callback->asBinder().get(), death_recipient_.get(),
                                                 reinterpret_cast<void*>(it->first));
            if (status != STATUS_OK && status != STATUS_DEAD_OBJECT) {
                LOG(WARNING) << __func__
                             << "Cannot unregister callback: " << ::android::statusToString(status);
            }
            it = callbacks_.erase(it);
            removed = true;
        } else {
            it++;
        }
    }
    return removed;
}

HealthInfo HealthSharedData::getHealthInfo() {
    std::lock_guard<decltype(update_lock_)> lock(update_lock_);
    return battery_monitor_->getHealthInfo();
}


Health::Health(std::string_view instance_name, std::unique_ptr<struct healthd_config>&& config)
    : instance_name_(instance_name),
      healthd_config_(std::move(config)),
      death_recipient_(AIBinder_DeathRecipient_new(&OnCallbackDiedWrapped)) {
    AIBinder_DeathRecipient_setOnUnlinked(death_recipient_.get(), onCallbackUnlinked);
    data_ = std::make_shared<HealthSharedData>(healthd_config_.get());
    update_thread_ = std::thread(update_thread, data_);
}

Health::~Health() {}

static inline ndk::ScopedAStatus TranslateStatus(::android::status_t err) {
    switch (err) {
        case ::android::OK:
            return ndk::ScopedAStatus::ok();
        case ::android::NAME_NOT_FOUND:
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        default:
            return ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(
                    IHealth::STATUS_UNKNOWN, ::android::statusToString(err).c_str());
    }
}

//
// Getters.
//

template <typename T>
ndk::ScopedAStatus HealthSharedData::GetProperty(int id, T defaultValue, T* out) {
    std::lock_guard<decltype(update_lock_)> lock(update_lock_);
    auto monitor = &*battery_monitor_;
    *out = defaultValue;
    struct ::android::BatteryProperty prop;
    ::android::status_t err = monitor->getProperty(static_cast<int>(id), &prop);
    if (err == ::android::OK) {
        *out = static_cast<T>(prop.valueInt64);
    } else {
        LOG(DEBUG) << "getProperty(" << id << ")"
                   << " fails: (" << err << ") " << ::android::statusToString(err);
    }
    return TranslateStatus(err);
}

ndk::ScopedAStatus Health::getChargeCounterUah(int32_t* out) {
    return data_->GetProperty<int32_t>(::android::BATTERY_PROP_CHARGE_COUNTER, 0, out);
}

ndk::ScopedAStatus Health::getCurrentNowMicroamps(int32_t* out) {
    return data_->GetProperty<int32_t>(::android::BATTERY_PROP_CURRENT_NOW, 0, out);
}

ndk::ScopedAStatus Health::getCurrentAverageMicroamps(int32_t* out) {
    return data_->GetProperty<int32_t>(::android::BATTERY_PROP_CURRENT_AVG, 0, out);
}

ndk::ScopedAStatus Health::getCapacity(int32_t* out) {
    return data_->GetProperty<int32_t>(::android::BATTERY_PROP_CAPACITY, 0, out);
}

ndk::ScopedAStatus Health::getEnergyCounterNwh(int64_t* out) {
    return data_->GetProperty<int64_t>(::android::BATTERY_PROP_ENERGY_COUNTER, 0, out);
}

ndk::ScopedAStatus Health::getChargeStatus(BatteryStatus* out) {
    return data_->GetProperty(::android::BATTERY_PROP_BATTERY_STATUS,
                       BatteryStatus::UNKNOWN, out);
}

ndk::ScopedAStatus Health::setChargingPolicy(BatteryChargingPolicy in_value) {
    in_value = static_cast<BatteryChargingPolicy>(0);
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Health::getChargingPolicy(BatteryChargingPolicy* out) {
    return data_->GetProperty(::android::BATTERY_PROP_CHARGING_POLICY,
                       BatteryChargingPolicy::DEFAULT, out);
}

ndk::ScopedAStatus Health::getBatteryHealthData(BatteryHealthData* out) {
    if (auto res =
                data_->GetProperty<int64_t>(::android::BATTERY_PROP_MANUFACTURING_DATE,
                                     0, &out->batteryManufacturingDateSeconds);
        !res.isOk()) {
        LOG(WARNING) << "Cannot get Manufacturing_date: " << res.getDescription();
    }
    if (auto res = data_->GetProperty<int64_t>(::android::BATTERY_PROP_FIRST_USAGE_DATE,
                                        0, &out->batteryFirstUsageSeconds);
        !res.isOk()) {
        LOG(WARNING) << "Cannot get First_usage_date: " << res.getDescription();
    }
    if (auto res = data_->GetProperty<int64_t>(::android::BATTERY_PROP_STATE_OF_HEALTH,
                                        0, &out->batteryStateOfHealth);
        !res.isOk()) {
        LOG(WARNING) << "Cannot get Battery_state_of_health: " << res.getDescription();
    }
    if (auto res = data_->battery_monitor_->getSerialNumber(&out->batterySerialNumber);
        res != ::android::OK) {
        LOG(WARNING) << "Cannot get Battery_serial_number: "
                     << TranslateStatus(res).getDescription();
    }

    int64_t part_status = static_cast<int64_t>(BatteryPartStatus::UNSUPPORTED);
    if (auto res = data_->GetProperty<int64_t>(::android::BATTERY_PROP_PART_STATUS,
                                        static_cast<int64_t>(BatteryPartStatus::UNSUPPORTED),
                                        &part_status);
        !res.isOk()) {
        LOG(WARNING) << "Cannot get Battery_part_status: " << res.getDescription();
    }
    out->batteryPartStatus = static_cast<BatteryPartStatus>(part_status);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Health::getDiskStats(std::vector<DiskStats>*) {
    // This implementation does not support DiskStats. An implementation may extend this
    // class and override this function to support disk stats.
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Health::getStorageInfo(std::vector<StorageInfo>*) {
    // This implementation does not support StorageInfo. An implementation may extend this
    // class and override this function to support storage info.
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Health::getHingeInfo(std::vector<HingeInfo>*) {
    // This implementation does not support HingeInfo. An implementation may extend this
    // class and override this function to support storage info.
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Health::getHealthInfo(HealthInfo* out) {

    {
        std::lock_guard<decltype(data_->update_lock_)> lock(data_->update_lock_);
        *out = data_->getHealthInfo();
    }

    UpdateHealthInfo(out);

    return ndk::ScopedAStatus::ok();
}

binder_status_t Health::dump(int fd, const char**, uint32_t) {
    std::lock_guard<decltype(data_->update_lock_)> lock(data_->update_lock_);
    data_->battery_monitor_->dumpState(fd);

    ::android::base::WriteStringToFd("\ngetHealthInfo -> ", fd);
    HealthInfo health_info;
    auto res = getHealthInfo(&health_info);
    if (res.isOk()) {
        ::android::base::WriteStringToFd(health_info.toString(), fd);
    } else {
        ::android::base::WriteStringToFd(res.getDescription(), fd);
    }
    ::android::base::WriteStringToFd("\n", fd);

    fsync(fd);
    return STATUS_OK;
}

std::optional<bool> Health::ShouldKeepScreenOn() {
    if (!healthd_config_->screen_on) {
        return std::nullopt;
    }

    HealthInfo health_info;
    auto res = getHealthInfo(&health_info);
    if (!res.isOk()) {
        return std::nullopt;
    }

    ::android::BatteryProperties props = {};
    convert(health_info, &props);
    return healthd_config_->screen_on(&props);
}

//
// Subclass helpers / overrides
//

void Health::UpdateHealthInfo(HealthInfo* /* health_info */) {
    /*
        // Sample code for a subclass to implement this:
        // If you need to modify values (e.g. batteryChargeTimeToFullNowSeconds), do it here.
        health_info->batteryChargeTimeToFullNowSeconds = calculate_charge_time_seconds();

        // If you need to call healthd_board_battery_update, modify its signature
        // and implementation to operate on HealthInfo directly, then call:
        healthd_board_battery_update(health_info);
    */
}

// A combination of the HIDL version
//   android::hardware::health::V2_1::implementation::Health::update() and
//   android::hardware::health::V2_1::implementation::BinderHealth::update()
ndk::ScopedAStatus Health::update() {
    LOG(ERROR) << "Update health\n";
    data_->update();

    return ndk::ScopedAStatus::ok();
}


void Health::BinderEvent(uint32_t /*epevents*/) {
    if (binder_fd_ >= 0) {
        ABinderProcess_handlePolledCommands();
    }
}

void Health::OnInit(HalHealthLoop* hal_health_loop, struct healthd_config* config) {
    LOG(INFO) << instance_name_ << " instance initializing with healthd_config...";

    // Similar to HIDL's android::hardware::health::V2_1::implementation::HalHealthLoop::Init,
    // copy configuration parameters to |config| for HealthLoop (e.g. uevent / wake alarm periods)
    *config = *healthd_config_.get();

    binder_status_t status = ABinderProcess_setupPolling(&binder_fd_);

    if (status == ::STATUS_OK && binder_fd_ >= 0) {
        std::shared_ptr<Health> thiz = ref<Health>();
        auto binder_event = [thiz](auto*, uint32_t epevents) { thiz->BinderEvent(epevents); };
        if (hal_health_loop->RegisterEvent(binder_fd_, binder_event, EVENT_NO_WAKEUP_FD) != 0) {
            PLOG(ERROR) << instance_name_ << " instance: Register for binder events failed";
        }
    }

    std::string health_name = IHealth::descriptor + "/"s + instance_name_;
    CHECK_EQ(STATUS_OK, AServiceManager_addService(this->asBinder().get(), health_name.c_str()))
            << instance_name_ << ": Failed to register HAL";

    LOG(INFO) << instance_name_ << ": Hal init done ";
}

// Unlike hwbinder, for binder, there's no need to explicitly call flushCommands()
// in PrepareToWait(). See b/139697085.

}  // namespace aidl::android::hardware::health

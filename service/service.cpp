// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
#define LOG_TAG "android.hardware.media.c2@1.0-service-v4l2"
#else
#error "V4L2_CODEC2_SERVICE_V4L2_STORE has to be defined"
#endif

#include <C2Component.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <base/logging.h>
#include <codec2/aidl/ComponentStore.h>
#include <log/log.h>
#include <minijail.h>

#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
#include <v4l2_codec2/v4l2/V4L2ComponentStore.h>
#endif

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-default-seccomp_policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-extended-seccomp_policy";

int main(int /* argc */, char** /* argv */) {
    ALOGD("Service starting...");

    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);

    ABinderProcess_setThreadPoolMaxThreadCount(16);
    ABinderProcess_startThreadPool();

#if LOG_NDEBUG == 0
    ALOGD("Enable all verbose logging of libchrome");
    logging::SetMinLogLevel(-5);
#endif

    // Create IComponentStore service.
    {
        using namespace ::aidl::android::hardware::media::c2;
        std::shared_ptr<IComponentStore> store;

#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
        ALOGD("Instantiating Codec2's V4L2 IComponentStore service...");
        store = ::ndk::SharedRefBase::make<utils::ComponentStore>(
                android::V4L2ComponentStore::Create());
#endif

        if (store == nullptr) {
            ALOGE("Cannot create Codec2's IComponentStore service.");
        } else {
            const std::string serviceName = std::string(IComponentStore::descriptor) + "/default";
            binder_exception_t ex =
                    AServiceManager_addService(store->asBinder().get(), serviceName.c_str());
            if (ex != EX_NONE) {
                ALOGE("Cannot register Codec2's IComponentStore service"
                      " with instance name \"%s\"",
                      serviceName.c_str());
            } else {
                ALOGD("Codec2's IComponentStore service registered. "
                      "Instance name: \"%s\"",
                      serviceName.c_str());
            }
        }
    }

    ABinderProcess_joinThreadPool();
    ALOGD("Service shutdown.");
    return 0;
}

/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef ANDROID_SENSORS_TEST_SHARED_MEMORY_H
#define ANDROID_SENSORS_TEST_SHARED_MEMORY_H

#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferAllocator.h>
#include <ui/GraphicBufferMapper.h>

#include <android-base/macros.h>
#include <log/log.h>

#include <sys/mman.h>
#include <cinttypes>

#include <cutils/ashmem.h>

using namespace ::android::hardware::sensors::V1_0;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::aidl::android::hardware::graphics::common::PixelFormat;

template <class SensorTypeVersion, class EventType>
class SensorsTestSharedMemory {
  public:
    static SensorsTestSharedMemory* create(SharedMemType type, size_t size) {
        constexpr size_t kMaxSize =
                128 * 1024 * 1024;  // sensor test should not need more than 128M
        if (size == 0 || size >= kMaxSize) {
            return nullptr;
        }

        auto m = new SensorsTestSharedMemory<SensorTypeVersion, EventType>(type, size);
        if (m->mSize != size || m->mBuffer == nullptr) {
            delete m;
            m = nullptr;
        }
        return m;
    }

    SharedMemInfo getSharedMemInfo() const {
        if (mType == SharedMemType::GRALLOC) {
            SharedMemInfo mem = {.type = mType,
                                 .format = SharedMemFormat::SENSORS_EVENT,
                                 .size = static_cast<uint32_t>(mSize),
                                 .memoryHandle = mBufferHandle};
            return mem;

        } else {
            SharedMemInfo mem = {.type = mType,
                                 .format = SharedMemFormat::SENSORS_EVENT,
                                 .size = static_cast<uint32_t>(mSize),
                                 .memoryHandle = mNativeHandle};
            return mem;
        }
    }
    char* getBuffer() const { return mBuffer; }
    size_t getSize() const { return mSize; }
    std::vector<EventType> parseEvents(int64_t lastCounter = -1, size_t offset = 0) const {
        constexpr size_t kEventSize = static_cast<size_t>(SensorsEventFormatOffset::TOTAL_LENGTH);
        constexpr size_t kOffsetSize = static_cast<size_t>(SensorsEventFormatOffset::SIZE_FIELD);
        constexpr size_t kOffsetToken = static_cast<size_t>(SensorsEventFormatOffset::REPORT_TOKEN);
        constexpr size_t kOffsetType = static_cast<size_t>(SensorsEventFormatOffset::SENSOR_TYPE);
        constexpr size_t kOffsetAtomicCounter =
                static_cast<size_t>(SensorsEventFormatOffset::ATOMIC_COUNTER);
        constexpr size_t kOffsetTimestamp =
                static_cast<size_t>(SensorsEventFormatOffset::TIMESTAMP);
        constexpr size_t kOffsetData = static_cast<size_t>(SensorsEventFormatOffset::DATA);

        std::vector<EventType> events;
        std::vector<float> data(16);

        while (offset + kEventSize <= mSize) {
            int64_t atomicCounter =
                    *reinterpret_cast<uint32_t*>(mBuffer + offset + kOffsetAtomicCounter);
            if (atomicCounter <= lastCounter) {
                ALOGV("atomicCounter = %" PRId64 ", lastCounter = %" PRId64, atomicCounter,
                      lastCounter);
                break;
            }

            int32_t size = *reinterpret_cast<int32_t*>(mBuffer + offset + kOffsetSize);
            if (size != kEventSize) {
                // unknown error, events parsed may be wrong, remove all
                events.clear();
                break;
            }

            int32_t token = *reinterpret_cast<int32_t*>(mBuffer + offset + kOffsetToken);
            int32_t type = *reinterpret_cast<int32_t*>(mBuffer + offset + kOffsetType);
            int64_t timestamp = *reinterpret_cast<int64_t*>(mBuffer + offset + kOffsetTimestamp);

            ALOGV("offset = %zu, cnt %" PRId64 ", token %" PRId32 ", type %" PRId32
                  ", timestamp %" PRId64,
                  offset, atomicCounter, token, type, timestamp);

            EventType event = {
                    .timestamp = timestamp,
                    .sensorHandle = token,
                    .sensorType = static_cast<SensorTypeVersion>(type),
            };
            event.u.data = android::hardware::hidl_array<float, 16>(
                    reinterpret_cast<float*>(mBuffer + offset + kOffsetData));

            events.push_back(event);

            lastCounter = atomicCounter;
            offset += kEventSize;
        }

        return events;
    }

    virtual ~SensorsTestSharedMemory() {
        switch (mType) {
            case SharedMemType::ASHMEM: {
                if (mSize != 0) {
                    ::munmap(mBuffer, mSize);
                    mBuffer = nullptr;

                    ::native_handle_close(mNativeHandle);
                    ::native_handle_delete(mNativeHandle);

                    mNativeHandle = nullptr;
                    mSize = 0;
                }
                break;
            }
            case SharedMemType::GRALLOC: {
                if (mSize != 0) {
                    android::status_t status =
                            android::GraphicBufferAllocator::get().free(mBufferHandle);
                    if (status != android::OK) {
                        ALOGE("SensorsAidlTestSharedMemory Gralloc failed to free buffer. Status: "
                              "%s",
                              android::statusToString(status).c_str());
                    }
                    mBufferHandle = nullptr;
                    mBuffer = nullptr;
                    mSize = 0;
                }
                break;
            }
            default: {
                if (mNativeHandle != nullptr || mSize != 0 || mBuffer != nullptr ||
                    mBufferHandle != nullptr) {
                    ALOGE("SensorsAidlTestSharedMemory %p not properly destructed: "
                          "type %d, native handle %p, size %zu, buffer %p, buffer handle %p",
                          this, static_cast<int>(mType), mNativeHandle, mSize, mBuffer,
                          mBufferHandle);
                }
                break;
            }
        }
    }

  private:
    SensorsTestSharedMemory(SharedMemType type, size_t size)
        : mType(type), mSize(0), mBuffer(nullptr) {
        native_handle_t* handle = nullptr;
        char* buffer = nullptr;
        switch (type) {
            case SharedMemType::ASHMEM: {
                int fd;
                handle = ::native_handle_create(1 /*nFds*/, 0 /*nInts*/);
                if (handle != nullptr) {
                    handle->data[0] = fd = ::ashmem_create_region("SensorsTestSharedMemory", size);
                    if (handle->data[0] > 0) {
                        // memory is pinned by default
                        buffer = static_cast<char*>(
                                ::mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
                        if (buffer != reinterpret_cast<char*>(MAP_FAILED)) {
                            break;
                        }
                        ::native_handle_close(handle);
                    }
                    ::native_handle_delete(handle);
                    handle = nullptr;
                }
                break;
            }
            case SharedMemType::GRALLOC: {
                static constexpr uint64_t kBufferUsage =
                        static_cast<uint64_t>(BufferUsage::SENSOR_DIRECT_DATA) |
                        static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN) |
                        static_cast<uint64_t>(BufferUsage::CPU_WRITE_RARELY);

                uint32_t stride = 0;
                buffer_handle_t bufferHandle;
                android::status_t status = android::GraphicBufferAllocator::get().allocate(
                        size, 1, static_cast<int>(PixelFormat::BLOB), 1, kBufferUsage,
                        &bufferHandle, &stride, "SensorVts");
                if (status != android::OK) {
                    ALOGE("SensorsAidlTestSharedMemory failed to allocate memory. Status: %s",
                          android::statusToString(status).c_str());
                    break;
                }
                // Per the HAL, all-zeros Rect means the entire buffer
                android::Rect rect = {0, 0, 0, 0};
                void* ret;
                status = android::GraphicBufferMapper::get().lock(bufferHandle, kBufferUsage, rect,
                                                                  &ret);
                if (status != android::OK) {
                    ALOGE("SensorsAidlTestSharedMemory failed to import buffer: Status: %s",
                          android::statusToString(status).c_str());
                } else {
                    buffer = static_cast<char*>(ret);
                    mBufferHandle = bufferHandle;
                }
                break;
            }
            default:
                break;
        }

        if (buffer != nullptr) {
            mNativeHandle = handle;
            mSize = size;
            mBuffer = buffer;
        }
    }

    SharedMemType mType;
    native_handle_t* mNativeHandle;
    buffer_handle_t mBufferHandle;
    size_t mSize;
    char* mBuffer;

    DISALLOW_COPY_AND_ASSIGN(SensorsTestSharedMemory);
};

#endif  // ANDROID_SENSORS_TEST_SHARED_MEMORY_H

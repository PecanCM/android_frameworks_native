/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <stdint.h>
#include <math.h>
#include <sys/types.h>

#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Singleton.h>

#include <binder/BinderService.h>
#include <binder/Parcel.h>
#include <binder/IServiceManager.h>

#include <hardware/sensors.h>

#include "SensorDevice.h"
#include "SensorService.h"

#include "sensors_deprecated.h"

#ifdef SYSFS_LIGHT_SENSOR
#include <fcntl.h>
#endif

namespace android {
// ---------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE(SensorDevice)

#ifdef SYSFS_LIGHT_SENSOR
#define DUMMY_ALS_HANDLE 0xdeadbeef
static ssize_t addDummyLightSensor(sensor_t const **list, ssize_t count) {
    struct sensor_t dummy_light =     {
                  name            : "CyanogenMod dummy light sensor",
                  vendor          : "CyanogenMod",
                  version         : 1,
                  handle          : DUMMY_ALS_HANDLE,
                  type            : SENSOR_TYPE_LIGHT,
                  maxRange        : 20,
                  resolution      : 0.1,
                  power           : 20,
    };
    void * new_list = malloc((count+1)*sizeof(sensor_t));
    new_list = memcpy(new_list, *list, count*sizeof(sensor_t));
    ((sensor_t *)new_list)[count] = dummy_light;
    *list = (sensor_t const *)new_list;
    count++;
    return count;
}
#endif

SensorDevice::SensorDevice()
    :  mSensorDevice(0),
       mOldSensorsEnabled(0),
       mOldSensorsCompatMode(false),
       mSensorModule(0)
{
    status_t err = hw_get_module(SENSORS_HARDWARE_MODULE_ID,
            (hw_module_t const**)&mSensorModule);

    ALOGE_IF(err, "couldn't load %s module (%s)",
            SENSORS_HARDWARE_MODULE_ID, strerror(-err));

    if (mSensorModule) {
#ifdef ENABLE_SENSORS_COMPAT
        sensors_control_open(&mSensorModule->common, &mSensorControlDevice) ;
        sensors_data_open(&mSensorModule->common, &mSensorDataDevice) ;
        mOldSensorsCompatMode = true;
#endif
        err = sensors_open(&mSensorModule->common, &mSensorDevice);

        ALOGE_IF(err, "couldn't open device for module %s (%s)",
                SENSORS_HARDWARE_MODULE_ID, strerror(-err));

        if (mSensorDevice || mOldSensorsCompatMode) {
            sensor_t const* list;
            ssize_t count = mSensorModule->get_sensors_list(mSensorModule, &list);
#ifdef SYSFS_LIGHT_SENSOR
            count = addDummyLightSensor(&list, count);
#endif

           if (mOldSensorsCompatMode) {
               mOldSensorsList = list;    
               mOldSensorsCount = count;
               mSensorDataDevice->data_open(mSensorDataDevice,
                            mSensorControlDevice->open_data_source(mSensorControlDevice));
            }

            mActivationCount.setCapacity(count);
            Info model;
            for (size_t i=0 ; i<size_t(count) ; i++) {
                mActivationCount.add(list[i].handle, model);
                mActivationCount.add(list[i].handle, model);
                if (mOldSensorsCompatMode) {
                    mSensorControlDevice->activate(mSensorControlDevice, list[i].handle, 0);
                } else {
                    mSensorDevice->activate(mSensorDevice, list[i].handle, 0);
                }
            }
        }
    }
}

void SensorDevice::dump(String8& result, char* buffer, size_t SIZE)
{
    if (!mSensorModule) return;
    sensor_t const* list;
    ssize_t count = mSensorModule->get_sensors_list(mSensorModule, &list);

    snprintf(buffer, SIZE, "%d h/w sensors:\n", int(count));
    result.append(buffer);

    Mutex::Autolock _l(mLock);
    for (size_t i=0 ; i<size_t(count) ; i++) {
        const Info& info = mActivationCount.valueFor(list[i].handle);
        snprintf(buffer, SIZE, "handle=0x%08x, active-count=%d, rates(ms)={ ",
                list[i].handle,
                info.rates.size());
        result.append(buffer);
        for (size_t j=0 ; j<info.rates.size() ; j++) {
            snprintf(buffer, SIZE, "%4.1f%s",
                    info.rates.valueAt(j) / 1e6f,
                    j<info.rates.size()-1 ? ", " : "");
            result.append(buffer);
        }
        snprintf(buffer, SIZE, " }, selected=%4.1f ms\n",  info.delay / 1e6f);
        result.append(buffer);
    }
}

ssize_t SensorDevice::getSensorList(sensor_t const** list) {
    if (!mSensorModule) return NO_INIT;
    ssize_t count = mSensorModule->get_sensors_list(mSensorModule, list);
#ifdef SYSFS_LIGHT_SENSOR
    return addDummyLightSensor(list, count);
#else
    return count;
#endif
}

status_t SensorDevice::initCheck() const {
     return (mSensorDevice || mOldSensorsCompatMode) && mSensorModule ? NO_ERROR : NO_INIT;
}

ssize_t SensorDevice::poll(sensors_event_t* buffer, size_t count) {
    if (!mSensorDevice && !mOldSensorsCompatMode) return NO_INIT;
    ssize_t c;
        if (mOldSensorsCompatMode) {
        size_t pollsDone = 0;
        //LOGV("%d buffers were requested",count);
        while (!mOldSensorsEnabled) {
            sleep(1);
            ALOGV("Waiting...");
        }
        while (pollsDone < (size_t)mOldSensorsEnabled && pollsDone < count) {
            sensors_data_t oldBuffer;
            long result =  mSensorDataDevice->poll(mSensorDataDevice, &oldBuffer);
            int sensorType = -1;
            int maxRange = -1;

            if (result == 0x7FFFFFFF) {
                continue;
            } else {
                /* the old data_poll is supposed to return a handle,
                 * which has to be mapped to the type. */
                for (size_t i=0 ; i<size_t(mOldSensorsCount) && sensorType < 0 ; i++) {
                    if (mOldSensorsList[i].handle == result) {
                        sensorType = mOldSensorsList[i].type;
                        maxRange = mOldSensorsList[i].maxRange;
                        ALOGV("mapped sensor type to %d",sensorType);
                    }
                }
            }
            if ( sensorType <= 0 ||
                 sensorType > SENSOR_TYPE_ROTATION_VECTOR) {
                ALOGV("Useless output at round %u from %d",pollsDone, oldBuffer.sensor);
                count--;
                continue;
            }
            buffer[pollsDone].version = sizeof(struct sensors_event_t);
            buffer[pollsDone].timestamp = oldBuffer.time;
            buffer[pollsDone].type = sensorType;
            buffer[pollsDone].sensor = result;
            /* This part is a union. Regardless of the sensor type,
             * we only need to copy a sensors_vec_t and a float */
            buffer[pollsDone].acceleration = oldBuffer.vector;
            buffer[pollsDone].temperature = oldBuffer.temperature;
            ALOGV("Adding results for sensor %d", buffer[pollsDone].sensor);
            /* The ALS and PS sensors only report values on change,
             * instead of a data "stream" like the others. So don't wait
             * for the number of requested samples to fill, and deliver
             * it immediately */
            if (sensorType == SENSOR_TYPE_PROXIMITY) {
#ifdef FOXCONN_SENSORS
            /* Fix ridiculous API breakages from FIH. */
            /* These idiots are returning -1 for FAR, and 1 for NEAR */
                if (buffer[pollsDone].distance > 0) {
                    buffer[pollsDone].distance = 0;
                } else {
                    buffer[pollsDone].distance = 1;
                }
#elif defined(PROXIMITY_LIES)
                if (buffer[pollsDone].distance >= PROXIMITY_LIES)
            buffer[pollsDone].distance = maxRange;
#endif
                return pollsDone+1;
            } else if (sensorType == SENSOR_TYPE_LIGHT) {
                return pollsDone+1;
            }
            pollsDone++;
        }
        return pollsDone;
    } else {
        do {
            c = mSensorDevice->poll(mSensorDevice, buffer, count);
        } while (c == -EINTR);
        return c;
    }
}

status_t SensorDevice::activate(void* ident, int handle, int enabled)
{
    if (!mSensorDevice && !mOldSensorsCompatMode) return NO_INIT;
    status_t err(NO_ERROR);
    bool actuateHardware = false;

#ifdef SYSFS_LIGHT_SENSOR
    if (handle == DUMMY_ALS_HANDLE) {
        int nwr, ret, fd;
        char value[2];

        fd = open(SYSFS_LIGHT_SENSOR, O_RDWR);
        if(fd < 0)
            return -ENODEV;

        nwr = snprintf(value, 2, "%d\n", enabled ? 1 : 0);
        write(fd, value, nwr);
        close(fd);
        return 0;
    }
#endif

    Info& info( mActivationCount.editValueFor(handle) );


    ALOGD_IF(DEBUG_CONNECTIONS,
            "SensorDevice::activate: ident=%p, handle=0x%08x, enabled=%d, count=%d",
            ident, handle, enabled, info.rates.size());

    if (enabled) {
        Mutex::Autolock _l(mLock);
        ALOGD_IF(DEBUG_CONNECTIONS, "... index=%ld",
                info.rates.indexOfKey(ident));

        if (info.rates.indexOfKey(ident) < 0) {
            info.rates.add(ident, DEFAULT_EVENTS_PERIOD);
            if (info.rates.size() == 1) {
                actuateHardware = true;
            }
        } else {
            // sensor was already activated for this ident
        }
    } else {
        Mutex::Autolock _l(mLock);
        ALOGD_IF(DEBUG_CONNECTIONS, "... index=%ld",
                info.rates.indexOfKey(ident));

        ssize_t idx = info.rates.removeItem(ident);
        if (idx >= 0) {
            if (info.rates.size() == 0) {
                actuateHardware = true;
            }
        } else {
            // sensor wasn't enabled for this ident
        }
    }

    if (actuateHardware) {
        ALOGD_IF(DEBUG_CONNECTIONS, "\t>>> actuating h/w");

        if (mOldSensorsCompatMode) {
            if (enabled)
                mOldSensorsEnabled++;
            else if (mOldSensorsEnabled > 0)
                mOldSensorsEnabled--;
            ALOGV("Activation for %d (%d)",handle,enabled);
            if (enabled) {
                mSensorControlDevice->wake(mSensorControlDevice);
            }
            err = mSensorControlDevice->activate(mSensorControlDevice, handle, enabled);
            err = 0;
        } else {
            err = mSensorDevice->activate(mSensorDevice, handle, enabled);
        }
        ALOGE_IF(err, "Error %s sensor %d (%s)",
                enabled ? "activating" : "disabling",
                handle, strerror(-err));
    }

    { // scope for the lock
        Mutex::Autolock _l(mLock);
        nsecs_t ns = info.selectDelay();
        if (mOldSensorsCompatMode) {
            mSensorControlDevice->set_delay(mSensorControlDevice, (ns/(1000*1000)));
        } else {
            mSensorDevice->setDelay(mSensorDevice, handle, ns);
        }
    }

    return err;
}

status_t SensorDevice::setDelay(void* ident, int handle, int64_t ns)
{
    if (!mSensorDevice && !mOldSensorsCompatMode) return NO_INIT;
    Mutex::Autolock _l(mLock);
    Info& info( mActivationCount.editValueFor(handle) );
    status_t err = info.setDelayForIdent(ident, ns);
    if (err < 0) return err;
    ns = info.selectDelay();
    if (mOldSensorsCompatMode) {
        return mSensorControlDevice->set_delay(mSensorControlDevice, (ns/(1000*1000)));
    } else {
        return mSensorDevice->setDelay(mSensorDevice, handle, ns);
    }
}

// ---------------------------------------------------------------------------

status_t SensorDevice::Info::setDelayForIdent(void* ident, int64_t ns)
{
    ssize_t index = rates.indexOfKey(ident);
    if (index < 0) {
        ALOGE("Info::setDelayForIdent(ident=%p, ns=%lld) failed (%s)",
                ident, ns, strerror(-index));
        return BAD_INDEX;
    }
    rates.editValueAt(index) = ns;
    return NO_ERROR;
}

nsecs_t SensorDevice::Info::selectDelay()
{
    nsecs_t ns = rates.valueAt(0);
    for (size_t i=1 ; i<rates.size() ; i++) {
        nsecs_t cur = rates.valueAt(i);
        if (cur < ns) {
            ns = cur;
        }
    }
    delay = ns;
    return ns;
}

// ---------------------------------------------------------------------------
}; // namespace android


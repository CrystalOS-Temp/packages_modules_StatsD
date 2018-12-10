/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "StatsPuller.h"
#include "StatsPullerManager.h"
#include "guardrail/StatsdStats.h"
#include "puller_util.h"
#include "stats_log_util.h"

namespace android {
namespace os {
namespace statsd {

using std::lock_guard;

sp<UidMap> StatsPuller::mUidMap = nullptr;
void StatsPuller::SetUidMap(const sp<UidMap>& uidMap) { mUidMap = uidMap; }

StatsPuller::StatsPuller(const int tagId)
    : mTagId(tagId) {
    // Pullers can cause significant impact to system health and battery.
    // So that we don't pull too frequently.
    mCoolDownNs = StatsPullerManager::kAllPullAtomInfo.find(tagId)->second.coolDownNs;
    VLOG("Puller for tag %d created. Cooldown set to %lld", mTagId, (long long)mCoolDownNs);
}

bool StatsPuller::Pull(const int64_t elapsedTimeNs, std::vector<std::shared_ptr<LogEvent>>* data) {
    lock_guard<std::mutex> lock(mLock);
    int64_t wallClockTimeNs = getWallClockNs();
    StatsdStats::getInstance().notePull(mTagId);
    if (elapsedTimeNs - mLastPullTimeNs < mCoolDownNs) {
        (*data) = mCachedData;
        StatsdStats::getInstance().notePullFromCache(mTagId);
        StatsdStats::getInstance().notePullDelay(mTagId, getElapsedRealtimeNs() - elapsedTimeNs);
        return true;
    }
    if (mMinPullIntervalNs > elapsedTimeNs - mLastPullTimeNs) {
        mMinPullIntervalNs = elapsedTimeNs - mLastPullTimeNs;
        StatsdStats::getInstance().updateMinPullIntervalSec(mTagId,
                                                            mMinPullIntervalNs / NS_PER_SEC);
    }
    mCachedData.clear();
    mLastPullTimeNs = elapsedTimeNs;
    int64_t pullStartTimeNs = getElapsedRealtimeNs();
    bool ret = PullInternal(&mCachedData);
    if (!ret) {
        mCachedData.clear();
        return false;
    }
    StatsdStats::getInstance().notePullTime(mTagId, getElapsedRealtimeNs() - pullStartTimeNs);
    for (const shared_ptr<LogEvent>& data : mCachedData) {
        data->setElapsedTimestampNs(elapsedTimeNs);
        data->setLogdWallClockTimestampNs(wallClockTimeNs);
    }

    if (mCachedData.size() > 0) {
        mapAndMergeIsolatedUidsToHostUid(mCachedData, mUidMap, mTagId);
        (*data) = mCachedData;
    }

    StatsdStats::getInstance().notePullDelay(mTagId, getElapsedRealtimeNs() - elapsedTimeNs);
    return ret;
}

int StatsPuller::ForceClearCache() {
    return clearCache();
}

int StatsPuller::clearCache() {
    lock_guard<std::mutex> lock(mLock);
    int ret = mCachedData.size();
    mCachedData.clear();
    mLastPullTimeNs = 0;
    return ret;
}

int StatsPuller::ClearCacheIfNecessary(int64_t timestampNs) {
    if (timestampNs - mLastPullTimeNs > mCoolDownNs) {
        return clearCache();
    } else {
        return 0;
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android

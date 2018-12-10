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

#pragma once

#include <gtest/gtest_prod.h>
#include <utils/threads.h>
#include <list>
#include "../anomaly/AnomalyTracker.h"
#include "../condition/ConditionTracker.h"
#include "../external/PullDataReceiver.h"
#include "../external/StatsPullerManager.h"
#include "../matchers/EventMatcherWizard.h"
#include "../stats_log_util.h"
#include "MetricProducer.h"
#include "frameworks/base/cmds/statsd/src/statsd_config.pb.h"

namespace android {
namespace os {
namespace statsd {

struct ValueBucket {
    int64_t mBucketStartNs;
    int64_t mBucketEndNs;
    std::vector<int> valueIndex;
    std::vector<Value> values;
};

class ValueMetricProducer : public virtual MetricProducer, public virtual PullDataReceiver {
public:
    ValueMetricProducer(const ConfigKey& key, const ValueMetric& valueMetric,
                        const int conditionIndex, const sp<ConditionWizard>& conditionWizard,
                        const int whatMatcherIndex,
                        const sp<EventMatcherWizard>& matcherWizard,
                        const int pullTagId, const int64_t timeBaseNs, const int64_t startTimeNs,
                        const sp<StatsPullerManager>& pullerManager);

    virtual ~ValueMetricProducer();

    // Process data pulled on bucket boundary.
    void onDataPulled(const std::vector<std::shared_ptr<LogEvent>>& data) override;

    // ValueMetric needs special logic if it's a pulled atom.
    void notifyAppUpgrade(const int64_t& eventTimeNs, const string& apk, const int uid,
                          const int64_t version) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mIsPulled && mCondition) {
            pullAndMatchEventsLocked(eventTimeNs - 1);
        }
        flushCurrentBucketLocked(eventTimeNs);
        mCurrentBucketStartTimeNs = eventTimeNs;
    };

protected:
    void onMatchedLogEventInternalLocked(
            const size_t matcherIndex, const MetricDimensionKey& eventKey,
            const ConditionKey& conditionKey, bool condition,
            const LogEvent& event) override;

private:
    void onDumpReportLocked(const int64_t dumpTimeNs,
                            const bool include_current_partial_bucket,
                            const bool erase_data,
                            std::set<string> *str_set,
                            android::util::ProtoOutputStream* protoOutput) override;
    void clearPastBucketsLocked(const int64_t dumpTimeNs) override;

    // Internal interface to handle condition change.
    void onConditionChangedLocked(const bool conditionMet, const int64_t eventTime) override;

    // Internal interface to handle sliced condition change.
    void onSlicedConditionMayChangeLocked(bool overallCondition, const int64_t eventTime) override;

    // Internal function to calculate the current used bytes.
    size_t byteSizeLocked() const override;

    void dumpStatesLocked(FILE* out, bool verbose) const override;

    // Util function to flush the old packet.
    void flushIfNeededLocked(const int64_t& eventTime) override;

    void flushCurrentBucketLocked(const int64_t& eventTimeNs) override;

    void dropDataLocked(const int64_t dropTimeNs) override;

    // Calculate previous bucket end time based on current time.
    int64_t calcPreviousBucketEndTime(const int64_t currentTimeNs);

    const int mWhatMatcherIndex;

    sp<EventMatcherWizard> mEventMatcherWizard;

    sp<StatsPullerManager> mPullerManager;

    // Value fields for matching.
    std::vector<Matcher> mFieldMatchers;

    // tagId for pulled data. -1 if this is not pulled
    const int mPullTagId;

    // if this is pulled metric
    const bool mIsPulled;

    // internal state of an ongoing aggregation bucket.
    typedef struct {
        // Index in multi value aggregation.
        int valueIndex;
        // Holds current base value of the dimension. Take diff and update if necessary.
        Value base;
        // Whether there is a base to diff to.
        bool hasBase;
        // Current value, depending on the aggregation type.
        Value value;
        // Number of samples collected.
        int sampleSize;
        // If this dimension has any non-tainted value. If not, don't report the
        // dimension.
        bool hasValue;
    } Interval;

    std::unordered_map<MetricDimensionKey, std::vector<Interval>> mCurrentSlicedBucket;

    std::unordered_map<MetricDimensionKey, int64_t> mCurrentFullBucket;

    // Save the past buckets and we can clear when the StatsLogReport is dumped.
    std::unordered_map<MetricDimensionKey, std::vector<ValueBucket>> mPastBuckets;

    // Pairs of (elapsed start, elapsed end) denoting buckets that were skipped.
    std::list<std::pair<int64_t, int64_t>> mSkippedBuckets;

    const int64_t mMinBucketSizeNs;

    // Util function to check whether the specified dimension hits the guardrail.
    bool hitGuardRailLocked(const MetricDimensionKey& newKey);

    void pullAndMatchEventsLocked(const int64_t timestampNs);

    // Reset diff base and mHasGlobalBase
    void resetBase();

    static const size_t kBucketSize = sizeof(ValueBucket{});

    const size_t mDimensionSoftLimit;

    const size_t mDimensionHardLimit;

    const bool mUseAbsoluteValueOnReset;

    const ValueMetric::AggregationType mAggregationType;

    const bool mUseDiff;

    const ValueMetric::ValueDirection mValueDirection;

    const bool mSkipZeroDiffOutput;

    // If true, use a zero value as base to compute the diff.
    // This is used for new keys which are present in the new data but was not
    // present in the base data.
    // The default base will only be used if we have a global base.
    const bool mUseZeroDefaultBase;

    // For pulled metrics, this is always set to true whenever a pull succeeds.
    // It is set to false when a pull fails, or upon condition change to false.
    // This is used to decide if we have the right base data to compute the
    // diff against.
    bool mHasGlobalBase;

    FRIEND_TEST(ValueMetricProducerTest, TestPulledEventsNoCondition);
    FRIEND_TEST(ValueMetricProducerTest, TestPulledEventsWithFiltering);
    FRIEND_TEST(ValueMetricProducerTest, TestPulledEventsTakeAbsoluteValueOnReset);
    FRIEND_TEST(ValueMetricProducerTest, TestPulledEventsTakeZeroOnReset);
    FRIEND_TEST(ValueMetricProducerTest, TestEventsWithNonSlicedCondition);
    FRIEND_TEST(ValueMetricProducerTest, TestPushedEventsWithUpgrade);
    FRIEND_TEST(ValueMetricProducerTest, TestPulledValueWithUpgrade);
    FRIEND_TEST(ValueMetricProducerTest, TestPulledValueWithUpgradeWhileConditionFalse);
    FRIEND_TEST(ValueMetricProducerTest, TestPushedEventsWithoutCondition);
    FRIEND_TEST(ValueMetricProducerTest, TestPushedEventsWithCondition);
    FRIEND_TEST(ValueMetricProducerTest, TestAnomalyDetection);
    FRIEND_TEST(ValueMetricProducerTest, TestBucketBoundaryNoCondition);
    FRIEND_TEST(ValueMetricProducerTest, TestBucketBoundaryWithCondition);
    FRIEND_TEST(ValueMetricProducerTest, TestBucketBoundaryWithCondition2);
    FRIEND_TEST(ValueMetricProducerTest, TestPushedAggregateMin);
    FRIEND_TEST(ValueMetricProducerTest, TestPushedAggregateMax);
    FRIEND_TEST(ValueMetricProducerTest, TestPushedAggregateAvg);
    FRIEND_TEST(ValueMetricProducerTest, TestPushedAggregateSum);
    FRIEND_TEST(ValueMetricProducerTest, TestFirstBucket);
    FRIEND_TEST(ValueMetricProducerTest, TestCalcPreviousBucketEndTime);
    FRIEND_TEST(ValueMetricProducerTest, TestSkipZeroDiffOutput);
    FRIEND_TEST(ValueMetricProducerTest, TestUseZeroDefaultBase);
    FRIEND_TEST(ValueMetricProducerTest, TestUseZeroDefaultBaseWithPullFailures);
};

}  // namespace statsd
}  // namespace os
}  // namespace android

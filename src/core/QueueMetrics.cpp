// src/core/QueueMetrics.cpp
#include "core/QueueMetrics.h"
#include "utils/Utils.h"
#include <cstring>
#include <cstdio>
#include <esp_timer.h>
#include <Arduino.h>

void QueueMetrics::reset() {
    totalSent_ = 0;
    totalReceived_ = 0;
    totalDropped_ = 0;
    overflowCount_ = 0;

    dropsByReason_.fill(0);

    highWaterMark_ = 0;
    timeAtHighWater_ = 0;
    averageUtilizationFP_ = 0;
    currentUtilizationFP_ = 0;
    utilizationSamples_ = 0;
    
    maxSendTime_ = 0;
    totalSendTime_ = 0;
    sendCount_ = 0;
    
    uint32_t now = millis();
    lastSendTime_ = now;
    lastReceiveTime_ = now;
    lastDropTime_ = 0;
    startTime_ = now;
    
    dropWindow_.fill(0);
    windowIndex_ = 0;
}

void QueueMetrics::recordSend(bool success, size_t queueDepth) {
    uint32_t now = millis();
    
    if (success) {
        totalSent_++;
        lastSendTime_ = now;
        
        // Update high water mark
        if (queueDepth > highWaterMark_) {
            highWaterMark_ = queueDepth;
            timeAtHighWater_ = now;
        }
        
        // Track performance
        if (sendCount_ > 0) {
            uint32_t sendTime = now - lastSendTime_;
            totalSendTime_ += sendTime;
            if (sendTime > maxSendTime_) {
                maxSendTime_ = sendTime;
            }
        }
        sendCount_++;
        
        // Update drop window
        dropWindow_[windowIndex_] = 0;
        windowIndex_ = (windowIndex_ + 1) % WINDOW_SIZE;
    }
}

void QueueMetrics::recordReceive(size_t queueDepth) {
    totalReceived_++;
    lastReceiveTime_ = millis();
}

void QueueMetrics::recordDrop(DropReason reason) {
    totalDropped_++;
    lastDropTime_ = millis();
    
    // Track drop reason
    size_t index = static_cast<size_t>(reason);
    if (index < dropsByReason_.size()) {
        dropsByReason_[index]++;
    }
    
    // Update drop window
    dropWindow_[windowIndex_] = 1;
    windowIndex_ = (windowIndex_ + 1) % WINDOW_SIZE;
}

void QueueMetrics::recordOverflow() {
    overflowCount_++;
    recordDrop(DropReason::QUEUE_FULL);
}

void QueueMetrics::updateUtilization(size_t current, size_t max) {
    if (max == 0) return;

    // Fixed-point: (current * 10000) / max = utilization in 0-10000 range
    currentUtilizationFP_ = static_cast<uint16_t>((current * FP_SCALE) / max);

    // Update rolling average using fixed-point EMA
    // alpha = 0.1 = 1/10, so: new_avg = (1 * current + 9 * old_avg) / 10
    if (utilizationSamples_ == 0) {
        averageUtilizationFP_ = currentUtilizationFP_;
    } else {
        // Integer EMA: (current + 9 * average) / 10
        averageUtilizationFP_ = static_cast<uint16_t>(
            (currentUtilizationFP_ + 9u * averageUtilizationFP_) / 10u);
    }
    utilizationSamples_++;
}

uint32_t QueueMetrics::getDropsByReason(DropReason reason) const {
    size_t index = static_cast<size_t>(reason);
    return (index < dropsByReason_.size()) ? dropsByReason_[index] : 0;
}

uint16_t QueueMetrics::getDropRateFP() const {
    // Calculate drop rate from rolling window (fixed-point: 0-10000 = 0-100%)
    uint32_t drops = 0;
    for (auto drop : dropWindow_) {
        drops += drop;
    }
    // (drops * 10000) / WINDOW_SIZE
    return static_cast<uint16_t>((drops * FP_SCALE) / WINDOW_SIZE);
}

uint32_t QueueMetrics::getAverageSendTime() const {
    if (sendCount_ <= 1) return 0;
    return totalSendTime_ / (sendCount_ - 1);
}

uint32_t QueueMetrics::getTimeSinceLastSend() const {
    return Utils::elapsedMs(lastSendTime_);
}

uint32_t QueueMetrics::getTimeSinceLastReceive() const {
    return Utils::elapsedMs(lastReceiveTime_);
}

uint32_t QueueMetrics::getTimeSinceLastDrop() const {
    if (lastDropTime_ == 0) return UINT32_MAX;
    return Utils::elapsedMs(lastDropTime_);
}

bool QueueMetrics::isHealthy() const {
    return getDropRateFP() < HEALTHY_DROP_RATE_FP &&
           averageUtilizationFP_ < WARNING_UTILIZATION_FP &&
           !hasRecentDrops();
}

bool QueueMetrics::hasRecentDrops() const {
    return getTimeSinceLastDrop() < RECENT_TIME_MS;
}

bool QueueMetrics::isNearCapacity() const {
    return currentUtilizationFP_ >= WARNING_UTILIZATION_FP;
}

void QueueMetrics::toJSON(char* buffer, size_t bufferSize) const {
    uint32_t uptime = millis() - startTime_;

    // Convert fixed-point to display format (e.g., 8123 -> 81.23)
    uint16_t dropRate = getDropRateFP();
    uint16_t avgUtil = averageUtilizationFP_;
    uint16_t curUtil = currentUtilizationFP_;

    snprintf(buffer, bufferSize,
        "{"
        "\"sent\":%lu,"
        "\"recv\":%lu,"
        "\"drop\":%lu,"
        "\"dropRate\":%u.%02u,"
        "\"overflow\":%lu,"
        "\"hwm\":%zu,"
        "\"avgUtil\":%u.%02u,"
        "\"curUtil\":%u.%02u,"
        "\"healthy\":%s,"
        "\"uptime\":%lu"
        "}",
        totalSent_,
        totalReceived_,
        totalDropped_,
        dropRate / 100, dropRate % 100,  // Fixed-point to X.XX%
        overflowCount_,
        highWaterMark_,
        avgUtil / 100, avgUtil % 100,    // Fixed-point to X.XX%
        curUtil / 100, curUtil % 100,    // Fixed-point to X.XX%
        isHealthy() ? "true" : "false",
        uptime
    );
}
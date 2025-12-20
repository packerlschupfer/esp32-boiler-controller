// src/modules/control/PIDAutoTuner.h
#ifndef PID_AUTO_TUNER_H
#define PID_AUTO_TUNER_H

#include <cmath>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config/SystemConstants.h"

/**
 * @brief Fixed-size circular buffer for memory-bounded data collection
 * @tparam T Element type
 * @tparam N Maximum capacity
 */
template<typename T, size_t N>
class CircularBuffer {
public:
    CircularBuffer() : head_(0), tail_(0), count_(0) {}

    void clear() {
        head_ = tail_ = count_ = 0;
    }

    void push_back(const T& item) {
        buffer_[head_] = item;
        head_ = (head_ + 1) % N;
        if (count_ < N) {
            count_++;
        } else {
            tail_ = (tail_ + 1) % N;  // Overwrite oldest
        }
    }

    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    bool full() const { return count_ == N; }

    // Access elements by logical index (0 = oldest)
    T& operator[](size_t idx) {
        return buffer_[(tail_ + idx) % N];
    }

    const T& operator[](size_t idx) const {
        return buffer_[(tail_ + idx) % N];
    }

    // Access last element
    T& back() { return (*this)[count_ - 1]; }
    const T& back() const { return (*this)[count_ - 1]; }

    // Iterator support for range-based for and algorithms
    class iterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;
        using iterator_category = std::random_access_iterator_tag;

        iterator(CircularBuffer* buf, size_t idx) : buf_(buf), idx_(idx) {}
        reference operator*() { return (*buf_)[idx_]; }
        iterator& operator++() { ++idx_; return *this; }
        iterator operator++(int) { iterator tmp = *this; ++idx_; return tmp; }
        iterator& operator--() { --idx_; return *this; }
        iterator& operator+=(difference_type n) { idx_ += n; return *this; }
        iterator operator+(difference_type n) const { return iterator(buf_, idx_ + n); }
        difference_type operator-(const iterator& other) const { return idx_ - other.idx_; }
        bool operator==(const iterator& other) const { return idx_ == other.idx_; }
        bool operator!=(const iterator& other) const { return idx_ != other.idx_; }
        bool operator<(const iterator& other) const { return idx_ < other.idx_; }
    private:
        CircularBuffer* buf_;
        size_t idx_;
    };

    class const_iterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;
        using iterator_category = std::random_access_iterator_tag;

        const_iterator(const CircularBuffer* buf, size_t idx) : buf_(buf), idx_(idx) {}
        reference operator*() const { return (*buf_)[idx_]; }
        const_iterator& operator++() { ++idx_; return *this; }
        const_iterator operator++(int) { const_iterator tmp = *this; ++idx_; return tmp; }
        const_iterator& operator--() { --idx_; return *this; }
        const_iterator& operator+=(difference_type n) { idx_ += n; return *this; }
        const_iterator operator+(difference_type n) const { return const_iterator(buf_, idx_ + n); }
        difference_type operator-(const const_iterator& other) const { return idx_ - other.idx_; }
        bool operator==(const const_iterator& other) const { return idx_ == other.idx_; }
        bool operator!=(const const_iterator& other) const { return idx_ != other.idx_; }
        bool operator<(const const_iterator& other) const { return idx_ < other.idx_; }
    private:
        const CircularBuffer* buf_;
        size_t idx_;
    };

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, count_); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, count_); }

private:
    T buffer_[N];
    size_t head_;   // Next write position
    size_t tail_;   // Oldest element position
    size_t count_;  // Current number of elements
};

/**
 * @brief PID Auto-tuning module using relay feedback method
 * 
 * This module implements the relay feedback auto-tuning method,
 * which is safer than the original Ziegler-Nichols method as it
 * doesn't require bringing the system to the edge of stability.
 */
class PIDAutoTuner {
public:
    // Tuning methods
    enum class TuningMethod {
        ZIEGLER_NICHOLS_PI,      // Conservative PI tuning
        ZIEGLER_NICHOLS_PID,     // Classic PID tuning
        TYREUS_LUYBEN,           // More conservative, less overshoot
        COHEN_COON,              // For processes with time delay
        LAMBDA_TUNING            // Smooth control, minimal overshoot
    };
    
    // Tuning state
    enum class TuningState {
        IDLE,
        RELAY_TEST,              // Performing relay feedback test
        ANALYZING,               // Analyzing oscillations
        COMPLETE,                // Tuning complete
        FAILED                   // Tuning failed
    };
    
    // Tuning results
    struct TuningResult {
        float Kp;
        float Ki;
        float Kd;
        float ultimateGain;      // Critical gain Ku
        float ultimatePeriod;    // Critical period Tu (seconds)
        bool valid;
        
        TuningResult() : Kp(0), Ki(0), Kd(0), ultimateGain(0), 
                        ultimatePeriod(0), valid(false) {}
    };
    
    // Oscillation data point
    struct OscillationPoint {
        float time;              // Time in seconds
        float value;             // Process value
        float output;            // Control output
    };
    
private:
    
    // Configuration
    float setpoint;              // Target temperature
    float outputStep;            // Relay output amplitude (e.g., ±50%)
    float hysteresis;            // Relay hysteresis band
    TuningMethod method;         // Selected tuning method
    
    // State
    TuningState state;
    bool relayState;             // Current relay state (high/low)
    float startTime;             // Tuning start time
    float lastSwitchTime;        // Last relay switch time

    // Peak/trough tracking during relay phases
    float phaseMaxTemp;          // Max temp seen during current ON phase (for peaks)
    float phaseMinTemp;          // Min temp seen during current OFF phase (for troughs)
    float phaseMaxTime;          // Time when max was seen
    float phaseMinTime;          // Time when min was seen
    
    // Data collection - fixed-size circular buffers to prevent memory growth
    // Oscillation data: stores last 1000 points (~1KB at 12 bytes per point)
    // At 100ms sample rate, this covers ~100 seconds of data
    static constexpr size_t OSCILLATION_BUFFER_SIZE = 1000;
    // Peak/trough data: stores last 32 values (enough for analysis)
    static constexpr size_t PEAK_BUFFER_SIZE = 32;

    CircularBuffer<OscillationPoint, OSCILLATION_BUFFER_SIZE> oscillationData;
    CircularBuffer<float, PEAK_BUFFER_SIZE> peakTimes;      // Times of peaks
    CircularBuffer<float, PEAK_BUFFER_SIZE> peakValues;     // Values at peaks
    CircularBuffer<float, PEAK_BUFFER_SIZE> troughTimes;    // Times of troughs
    CircularBuffer<float, PEAK_BUFFER_SIZE> troughValues;   // Values at troughs
    
    // Results
    TuningResult result;
    
    // Thread safety
    SemaphoreHandle_t mutex;
    
    // Tuning parameters
    // Use constants from SystemConstants::PID::Autotune
    static constexpr uint8_t MIN_CYCLES = SystemConstants::PID::Autotune::MIN_CYCLES;
    static constexpr uint8_t MAX_CYCLES = SystemConstants::PID::Autotune::MAX_CYCLES;
    static constexpr float MAX_TUNING_TIME = SystemConstants::PID::Autotune::MAX_TUNING_TIME_SECONDS;
    static constexpr float NOISE_BAND = 0.5;        // Noise band for peak detection
    
public:
    PIDAutoTuner();
    ~PIDAutoTuner();
    
    /**
     * @brief Start auto-tuning process
     * @param targetSetpoint Target temperature
     * @param relayAmplitude Output step size (typically 30-50% of max output)
     * @param relayHysteresis Hysteresis band (typically 1-2°C)
     * @param tuningMethod Method to use for calculating PID parameters
     */
    bool startTuning(float targetSetpoint,
                     float relayAmplitude = SystemConstants::PID::Autotune::DEFAULT_RELAY_AMPLITUDE,
                     float relayHysteresis = SystemConstants::PID::Autotune::DEFAULT_RELAY_HYSTERESIS,
                     TuningMethod tuningMethod = TuningMethod::ZIEGLER_NICHOLS_PI);
    
    /**
     * @brief Update auto-tuning process
     * @param currentTemp Current process temperature
     * @param currentTime Current time in seconds
     * @return Control output (-100 to 100)
     */
    float update(float currentTemp, float currentTime);
    
    /**
     * @brief Stop auto-tuning
     */
    void stopTuning();
    
    /**
     * @brief Get tuning state
     */
    TuningState getState() const { return state; }
    
    /**
     * @brief Check if tuning is complete
     */
    bool isComplete() const { return state == TuningState::COMPLETE; }
    
    /**
     * @brief Get tuning results
     */
    TuningResult getResults() const { return result; }
    
    /**
     * @brief Get progress percentage
     */
    uint8_t getProgress() const;

    /**
     * @brief Get number of complete cycles detected
     */
    uint8_t getCycleCount() const;

    /**
     * @brief Get minimum cycles required for completion
     */
    uint8_t getMinCycles() const { return MIN_CYCLES; }

    /**
     * @brief Get elapsed tuning time in seconds
     */
    float getElapsedTime() const;

    /**
     * @brief Get maximum tuning time in seconds
     */
    float getMaxTuningTime() const { return MAX_TUNING_TIME; }

    /**
     * @brief Get status message
     */
    const char* getStatusMessage() const;
    
private:
    /**
     * @brief Perform relay feedback control
     */
    float relayControl(float currentTemp);
    
    /**
     * @brief Detect peaks and troughs in oscillation
     */
    void detectPeaksAndTroughs(float currentTemp, float currentTime);
    
    /**
     * @brief Analyze oscillations to determine ultimate gain and period
     */
    bool analyzeOscillations();
    
    /**
     * @brief Calculate PID parameters from ultimate gain and period
     */
    void calculatePIDParameters();
    
    /**
     * @brief Apply specific tuning method formulas
     */
    void applyTuningMethod(float Ku, float Tu);
    
    /**
     * @brief Check if we have enough oscillation cycles
     */
    bool hasEnoughCycles() const;
    
    /**
     * @brief Calculate average period from oscillation data
     */
    float calculateAveragePeriod() const;
    
    /**
     * @brief Calculate amplitude from oscillation data
     */
    float calculateAmplitude() const;
};

#endif // PID_AUTO_TUNER_H
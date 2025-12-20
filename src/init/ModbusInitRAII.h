// src/init/ModbusInitRAII.h
#pragma once

#include <memory>

// RAII wrapper for ModbusInitState to ensure proper cleanup
template<typename T>
class TaskParamRAII {
private:
    T* param;
    bool released;
    
public:
    explicit TaskParamRAII(T* p) : param(p), released(false) {}
    
    ~TaskParamRAII() {
        if (!released && param) {
            delete param;
        }
    }
    
    // Release ownership (when task takes over)
    T* release() {
        released = true;
        return param;
    }
    
    T* get() { return param; }
    T* operator->() { return param; }
    
    // Prevent copying
    TaskParamRAII(const TaskParamRAII&) = delete;
    TaskParamRAII& operator=(const TaskParamRAII&) = delete;
    
    // Allow moving
    TaskParamRAII(TaskParamRAII&& other) noexcept 
        : param(other.param), released(other.released) {
        other.param = nullptr;
        other.released = true;
    }
};
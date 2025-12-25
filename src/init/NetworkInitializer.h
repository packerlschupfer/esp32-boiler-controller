// src/init/NetworkInitializer.h
#ifndef NETWORK_INITIALIZER_H
#define NETWORK_INITIALIZER_H

#include "utils/ErrorHandler.h"

/**
 * @brief Handles network initialization
 *
 * Initializes Ethernet in async mode (non-blocking) and spawns
 * a background task to wait for connection and set NETWORK_READY bit.
 */
class NetworkInitializer {
public:
    /**
     * @brief Initialize network asynchronously (non-blocking)
     *
     * Starts Ethernet initialization and creates a background task
     * that will set NETWORK_READY event bit when connected.
     *
     * @return Result indicating success or failure of starting initialization
     */
    static Result<void> initializeAsync();

    /**
     * @brief Initialize network synchronously (blocking)
     *
     * Waits for Ethernet connection before returning.
     *
     * @return Result indicating success or failure
     */
    static Result<void> initializeBlocking();

private:
    // Prevent instantiation
    NetworkInitializer() = delete;

    /**
     * @brief Background task that monitors network connection
     * @param param Task parameter (unused)
     */
    static void networkMonitorTask(void* param);
};

#endif // NETWORK_INITIALIZER_H

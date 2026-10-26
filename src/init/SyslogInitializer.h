// src/init/SyslogInitializer.h
#ifndef SYSLOG_INITIALIZER_H
#define SYSLOG_INITIALIZER_H

#include "utils/ErrorHandler.h"

/**
 * @brief Initializes syslog remote logging
 *
 * Creates a FreeRTOS task that waits for network and storage to be ready,
 * then initializes the syslog client based on settings from NVS.
 */
class SyslogInitializer {
public:
    /**
     * @brief Start the syslog initialization task
     * @return Result indicating success or failure
     */
    static Result<void> initialize();

    /**
     * @brief Syslog task entry point
     *
     * Waits for NETWORK_READY and STORAGE_READY events, then:
     * - Checks syslogEnabled setting
     * - Initializes Syslog with server IP, port, facility from settings
     * - Registers with SystemResourceProvider
     * - Deletes itself after initialization
     *
     * @param param Unused
     */
    static void SyslogTask(void* param);
};

#endif // SYSLOG_INITIALIZER_H

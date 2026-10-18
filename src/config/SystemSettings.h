#ifndef SYSTEM_SETTINGS_H
#define SYSTEM_SETTINGS_H

#include "SystemSettingsStruct.h"

/**
 * @brief Access system settings through SystemResourceProvider.
 * 
 * Use SRP::getSystemSettings() to access the current runtime configuration.
 * The settings should be initialized using `getDefaultSystemSettings()`.
 * 
 * Example: SRP::getSystemSettings().targetTemperatureInside
 */
// System settings are now accessed through SRP::getSystemSettings()

/**
 * @brief Get the default system settings.
 * 
 * This function returns a `SystemSettings` object initialized with default values.
 * 
 * @return A `SystemSettings` instance with default configuration.
 */
SystemSettings getDefaultSystemSettings();

#endif // SYSTEM_SETTINGS_H

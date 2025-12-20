#pragma once
#include <array>
#include "config/RelayIndices.h"

namespace RelayBindings {

    /**
     * @brief Runtime relay pointer bindings (lives in RAM)
     *
     * This array is initialized once at startup and connects the logical
     * RelayIndex constants to their corresponding bool pointers in
     * SharedRelayReadings.
     */
    extern std::array<bool*, RelayIndex::MAX_RELAYS> pointers;

    /**
     * @brief Initialize pointer bindings based on RelayIndex assignments
     *
     * This function MUST be called during system initialization before
     * any relay operations. It connects the RelayIndex constants to the
     * actual SharedRelayReadings struct members.
     */
    void initialize();

    /**
     * @brief Get state pointer for a relay
     * @param index Logical relay index (use RelayIndex constants)
     * @return Pointer to relay state bool
     */
    inline bool* getStatePtr(uint8_t index) {
        return pointers[index];
    }

    /**
     * @brief Get the entire pointer array for binding to RYN4
     * @return Reference to pointer array
     */
    inline const std::array<bool*, RelayIndex::MAX_RELAYS>& getPointerArray() {
        return pointers;
    }
}

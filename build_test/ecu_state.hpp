#pragma once

/**
 * @file ecu_state.hpp
 * @brief Single source of truth for the ECU's possible operational states.
 */
enum class EcuState {
    BOOT,            // Initial state: integrity check and peripheral init
    APPLICATION,     // Normal operating mode: runs control loop
    UPDATE_PENDING,  // Ready to receive OTA firmware via UDS
    BRICKED          // Terminal fault state: critical error detected
};

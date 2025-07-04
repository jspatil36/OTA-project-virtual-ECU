#pragma once

// This file is the single source of truth for the ECU's possible states.
enum class EcuState {
    BOOT,
    APPLICATION,
    UPDATE_PENDING,
    BRICKED
};

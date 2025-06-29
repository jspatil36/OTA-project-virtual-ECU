#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>

// Include our new NVRAM manager
#include "nvram_manager.hpp"

// Represents the possible operational states of our virtual ECU.
enum class EcuState
{
    BOOT,
    APPLICATION,
    UPDATE_PENDING,
    BRICKED
};

// A global, thread-safe variable to hold the current state of the ECU.
std::atomic<EcuState> g_ecu_state(EcuState::BOOT);

// A global flag to control the main loop.
std::atomic<bool> g_running(true);

// --- Global Objects ---
// Create a global instance of our NVRAM manager. It will use a file named "nvram.dat".
NVRAMManager g_nvram("nvram.dat");

// --- Function Prototypes ---
void run_boot_sequence();
void run_application_mode();
void handle_signal(int signal);

/**
 * @brief The main entry point for the Virtual ECU simulation.
 */
int main()
{
    // Register a signal handler to catch Ctrl+C (SIGINT) for a clean exit.
    signal(SIGINT, handle_signal);

    std::cout << "--- Virtual ECU Simulation Started ---" << std::endl;
    std::cout << "Press Ctrl+C to shut down." << std::endl;

    while (g_running)
    {
        switch (g_ecu_state)
        {
        case EcuState::BOOT:
            run_boot_sequence();
            break;
        case EcuState::APPLICATION:
            run_application_mode();
            break;
        case EcuState::UPDATE_PENDING:
            std::cout << "[STATE] In UPDATE_PENDING. Waiting for commands..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            break;
        case EcuState::BRICKED:
            std::cerr << "[STATE] ECU is BRICKED. Halting operations." << std::endl;
            g_running = false; // Stop the main loop
            break;
        }
    }

    std::cout << "--- Virtual ECU Simulation Shutting Down ---" << std::endl;
    return 0;
}

/**
 * @brief Simulates the ECU's boot sequence.
 */
void run_boot_sequence()
{
    std::cout << "[STATE] Entering BOOT..." << std::endl;

    // Load data from our persistent storage.
    if (!g_nvram.load())
    {
        std::cerr << "[BOOT] CRITICAL: Failed to load NVRAM. Entering BRICKED state." << std::endl;
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    // Read the firmware version from NVRAM and display it.
    auto fw_version = g_nvram.get_string("FIRMWARE_VERSION");
    if (fw_version)
    {
        std::cout << "[BOOT] Current Firmware Version: " << *fw_version << std::endl;
    }

    std::cout << "  -> Initializing peripherals (simulated)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout << "  -> Performing Power-On Self-Test (POST)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // For now, we assume the boot check passes and transition to the main application.
    // In Phase 4, we will add the SHA-256 integrity check here.
    std::cout << "  -> Boot successful. Transitioning to APPLICATION state." << std::endl;
    g_ecu_state = EcuState::APPLICATION;
}

/**
 * @brief Simulates the ECU's main application mode.
 */
void run_application_mode()
{
    std::cout << "[STATE] In APPLICATION. Running main logic..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
}

/**
 * @brief Handles termination signals like Ctrl+C.
 */
void handle_signal(int signal)
{
    if (signal == SIGINT)
    {
        std::cout << "\n[INFO] Shutdown signal received. Terminating..." << std::endl;
        g_running = false;
    }
}

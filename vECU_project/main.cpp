#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>

// Represents the possible operational states of our virtual ECU.
enum class EcuState {
    BOOT,
    APPLICATION,
    UPDATE_PENDING,
    BRICKED
};

// A global, thread-safe variable to hold the current state of the ECU.
// std::atomic ensures that changes to this variable are safe across different threads,
// which will be important when our network listener needs to change the state.
std::atomic<EcuState> g_ecu_state(EcuState::BOOT);

// A global flag to control the main loop.
// This allows us to gracefully shut down the ECU simulation.
std::atomic<bool> g_running(true);

// --- Function Prototypes ---
void run_boot_sequence();
void run_application_mode();
void handle_signal(int signal);

/**
 * @brief The main entry point for the Virtual ECU simulation.
 *
 * This function initializes the signal handler for graceful shutdown (Ctrl+C)
 * and runs the main ECU state machine loop.
 */
int main() {
    // Register a signal handler to catch Ctrl+C (SIGINT) for a clean exit.
    signal(SIGINT, handle_signal);

    std::cout << "--- Virtual ECU Simulation Started ---" << std::endl;
    std::cout << "Press Ctrl+C to shut down." << std::endl;

    // The main loop of the ECU. It continuously checks the current state
    // and executes the corresponding logic until g_running is set to false.
    while (g_running) {
        switch (g_ecu_state) {
            case EcuState::BOOT:
                run_boot_sequence();
                break;
            case EcuState::APPLICATION:
                run_application_mode();
                break;
            case EcuState::UPDATE_PENDING:
                // In a real ECU, this state would wait for the update to start.
                // For now, we just log and wait.
                std::cout << "[STATE] In UPDATE_PENDING. Waiting for commands..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                break;
            case EcuState::BRICKED:
                // This is a terminal state. The ECU is non-functional.
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
 *
 * In this phase, a real ECU would initialize hardware and run self-checks.
 * Here, we will eventually add our "Secure Boot" check.
 */
void run_boot_sequence() {
    std::cout << "[STATE] Entering BOOT..." << std::endl;
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
 *
 * This represents the ECU performing its primary function, like controlling an engine
 * or managing infotainment. We just print a message periodically.
 */
void run_application_mode() {
    std::cout << "[STATE] In APPLICATION. Running main logic..." << std::endl;

    // Simulate the ECU doing work for a couple of seconds.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Note: In the final version, a network listener will run in a separate thread.
    // A UDS command could change g_ecu_state to UPDATE_PENDING, breaking this loop.
}

/**
 * @brief Handles termination signals like Ctrl+C.
 *
 * This function ensures a graceful shutdown by setting the global running flag to false,
 * allowing the main loop to terminate cleanly.
 * @param signal The signal number received.
 */
void handle_signal(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n[INFO] Shutdown signal received. Terminating..." << std::endl;
        g_running = false;
    }
}

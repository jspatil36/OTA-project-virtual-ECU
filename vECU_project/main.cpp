#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <memory>

#include "nvram_manager.hpp"
#include "doip_server.hpp" // Include our new DoIP server

// Represents the possible operational states of our virtual ECU.
enum class EcuState {
    BOOT,
    APPLICATION,
    UPDATE_PENDING,
    BRICKED
};

std::atomic<EcuState> g_ecu_state(EcuState::BOOT);
std::atomic<bool> g_running(true);
NVRAMManager g_nvram("nvram.dat");

// --- Networking objects ---
boost::asio::io_context g_io_context;
std::unique_ptr<DoIPServer> g_doip_server;
std::thread g_server_thread;


// --- Function Prototypes ---
void run_boot_sequence();
void run_application_mode();
void handle_signal(int signal);
void start_network_server();
void stop_network_server();


int main() {
    signal(SIGINT, handle_signal);

    std::cout << "--- Virtual ECU Simulation Started ---" << std::endl;
    std::cout << "Press Ctrl+C to shut down." << std::endl;

    start_network_server();

    while (g_running) {
        switch (g_ecu_state) {
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

    // Cleanly stop the network server and join its thread.
    stop_network_server();

    std::cout << "--- Virtual ECU Simulation Shutting Down ---" << std::endl;
    return 0;
}

/**
 * @brief Creates the DoIP server and runs it in a background thread.
 */
void start_network_server() {
    try {
        g_doip_server = std::make_unique<DoIPServer>(g_io_context, 13400);
        g_server_thread = std::thread([]() {
            g_doip_server->run();
        });

    } catch (const std::exception& e) {
        std::cerr << "Failed to start network server: " << e.what() << std::endl;
        g_ecu_state = EcuState::BRICKED;
    }
}

/**
 * @brief Waits for the network server thread to finish.
 * The server is now stopped via the signal handler.
 */
void stop_network_server() {
    if (g_server_thread.joinable()) {
        g_server_thread.join(); // Wait for the server thread to finish.
    }
}

void run_boot_sequence() {
    std::cout << "[STATE] Entering BOOT..." << std::endl;
    
    if (!g_nvram.load()) {
        std::cerr << "[BOOT] CRITICAL: Failed to load NVRAM. Entering BRICKED state." << std::endl;
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    auto fw_version = g_nvram.get_string("FIRMWARE_VERSION");
    if(fw_version) {
        std::cout << "[BOOT] Current Firmware Version: " << *fw_version << std::endl;
    }

    std::cout << "  -> Initializing peripherals (simulated)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout << "  -> Performing Power-On Self-Test (POST)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "  -> Boot successful. Transitioning to APPLICATION state." << std::endl;
    g_ecu_state = EcuState::APPLICATION;
}

void run_application_mode() {
    // Only print if the loop is still supposed to be running.
    if (g_running) {
        std::cout << "[STATE] In APPLICATION. Running main logic..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

/**
 * @brief Handles termination signals like Ctrl+C.
 *
 * This function now takes direct action to make shutdown more responsive.
 * @param signal The signal number received.
 */
void handle_signal(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n[INFO] Shutdown signal received. Initiating shutdown..." << std::endl;
        
        // 1. Immediately tell the network server to stop its I/O operations.
        //    This unblocks the server thread so it can exit cleanly.
        if (g_doip_server) {
            g_doip_server->stop();
        }

        // 2. Set the flag to terminate the main application loop.
        g_running = false;
    }
}

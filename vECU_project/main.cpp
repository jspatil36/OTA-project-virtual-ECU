#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <memory>
#include <vector>
#include <iomanip>
#include <sstream>
#include <fstream>

// OpenSSL headers for SHA-256
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "nvram_manager.hpp"
#include "doip_server.hpp"

// Represents the possible operational states of our virtual ECU.
enum class EcuState {
    BOOT,
    APPLICATION,
    UPDATE_PENDING,
    BRICKED
};

// Global state and control variables
std::atomic<EcuState> g_ecu_state(EcuState::BOOT);
std::atomic<bool> g_running(true);
NVRAMManager g_nvram("nvram.dat");

// Networking objects
boost::asio::io_context g_io_context;
std::unique_ptr<DoIPServer> g_doip_server;
std::thread g_server_thread;

// --- Function Prototypes ---
void run_boot_sequence(const std::string& executable_path);
void run_application_mode();
void handle_signal(int signal);
void start_network_server();
void stop_network_server();
std::optional<std::string> calculate_file_hash(const std::string& file_path);


/**
 * @brief Calculates the SHA-256 hash of a file.
 * @param file_path The path to the file to hash.
 * @return An std::optional<std::string> containing the hex hash, or nullopt on failure.
 */
std::optional<std::string> calculate_file_hash(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[HASH] ERROR: Could not open file: " << file_path << std::endl;
        return std::nullopt;
    }

    EVP_MD_CTX* md_context = EVP_MD_CTX_new();
    if (!md_context) return std::nullopt;

    if (1 != EVP_DigestInit_ex(md_context, EVP_sha256(), NULL)) {
        EVP_MD_CTX_free(md_context);
        return std::nullopt;
    }

    char buffer[1024];
    while (file.read(buffer, sizeof(buffer))) {
        if (1 != EVP_DigestUpdate(md_context, buffer, file.gcount())) {
            EVP_MD_CTX_free(md_context);
            return std::nullopt;
        }
    }
    // Handle the last chunk of the file
    if (file.gcount() > 0) {
        if (1 != EVP_DigestUpdate(md_context, buffer, file.gcount())) {
            EVP_MD_CTX_free(md_context);
            return std::nullopt;
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    if (1 != EVP_DigestFinal_ex(md_context, hash, &hash_len)) {
        EVP_MD_CTX_free(md_context);
        return std::nullopt;
    }
    EVP_MD_CTX_free(md_context);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

/**
 * @brief Main entry point of the application.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Exit code.
 */
int main(int argc, char* argv[]) {
    if (argc < 1) return 1;
    std::string executable_path = argv[0];

    signal(SIGINT, handle_signal);

    std::cout << "--- Virtual ECU Simulation Started ---" << std::endl;
    std::cout << "Press Ctrl+C to shut down." << std::endl;

    start_network_server();

    while (g_running) {
        switch (g_ecu_state) {
            case EcuState::BOOT:
                run_boot_sequence(executable_path);
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
                g_running = false;
                break;
        }
    }

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
 */
void stop_network_server() {
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
}

/**
 * @brief Handles the ECU's boot-up sequence, including Secure Boot verification.
 * @param executable_path The path to this application's executable for hashing.
 */
void run_boot_sequence(const std::string& executable_path) {
    std::cout << "[STATE] Entering BOOT..." << std::endl;
    
    if (!g_nvram.load()) {
        std::cerr << "[BOOT] CRITICAL: Failed to load NVRAM. Entering BRICKED state." << std::endl;
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    // --- SECURE BOOT VERIFICATION ---
    std::cout << "[BOOT] Performing Secure Boot integrity check..." << std::endl;
    
    auto golden_hash_opt = g_nvram.get_string("FIRMWARE_HASH_GOLDEN");
    if (!golden_hash_opt) {
        std::cerr << "[BOOT] CRITICAL: Golden firmware hash not found in NVRAM. Halting." << std::endl;
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    auto calculated_hash_opt = calculate_file_hash(executable_path);
    if (!calculated_hash_opt) {
        std::cerr << "[BOOT] CRITICAL: Could not calculate hash of running executable. Halting." << std::endl;
        g_ecu_state = EcuState::BRICKED;
        return;
    }
    
    std::cout << "  -> Golden Hash: " << *golden_hash_opt << std::endl;
    std::cout << "  -> Calculated Hash: " << *calculated_hash_opt << std::endl;

    if (*golden_hash_opt != *calculated_hash_opt) {
        std::cerr << "[BOOT] !!! INTEGRITY CHECK FAILED !!! Hashes do not match. Entering BRICKED state." << std::endl;
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    std::cout << "[BOOT] Integrity check PASSED." << std::endl;
    // --- END SECURE BOOT VERIFICATION ---

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

/**
 * @brief Runs the main application logic when the ECU is in its normal operational state.
 */
void run_application_mode() {
    if (g_running) {
        std::cout << "[STATE] In APPLICATION. Running main logic..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

/**
 * @brief Handles termination signals like Ctrl+C for graceful shutdown.
 * @param signal The signal number received.
 */
void handle_signal(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n[INFO] Shutdown signal received. Initiating shutdown..." << std::endl;
        
        if (g_doip_server) {
            g_doip_server->stop();
        }

        g_running = false;
    }
}

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
#include <cstdio>
#include <mutex>
#include <filesystem>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "ecu_state.hpp"
#include "nvram_manager.hpp"
#include "dtc_manager.hpp"
#include "doip_server.hpp"

// ---------------------------------------------------------------------------
// Global ECU state
// ---------------------------------------------------------------------------
std::atomic<EcuState> g_ecu_state(EcuState::BOOT);
std::atomic<bool>     g_running(true);

NVRAMManager g_nvram("nvram.dat");
DTCManager   g_dtc_manager(g_nvram);
std::string  g_executable_path;

// ---------------------------------------------------------------------------
// Simulated sensor data (read by $22 RDBI handler in doip_session.hpp)
// ---------------------------------------------------------------------------
std::atomic<int>  g_engine_temp_c(20);   // Degrees Celsius, starts at ambient
std::atomic<bool> g_fan_active(false);    // Cooling fan state

// Mutex protecting console output from main + server threads
std::mutex g_console_mutex;

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
boost::asio::io_context g_io_context;
std::unique_ptr<DoIPServer> g_doip_server;
std::thread g_server_thread;

// ---------------------------------------------------------------------------
// Function Prototypes
// ---------------------------------------------------------------------------
void run_boot_sequence(const std::string& executable_path);
void run_application_mode();
void handle_signal(int signal);
void start_network_server();
void stop_network_server();
std::optional<std::string> calculate_file_hash(const std::string& file_path);
void apply_update(const std::string& current_executable_path);


// ---------------------------------------------------------------------------
// SHA-256 file hash (used by secure boot and OTA verification)
// ---------------------------------------------------------------------------
std::optional<std::string> calculate_file_hash(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[HASH] ERROR: Could not open file: " << file_path << std::endl;
        return std::nullopt;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || 1 != EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) {
        if (ctx) EVP_MD_CTX_free(ctx);
        return std::nullopt;
    }

    char buf[4096];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        if (1 != EVP_DigestUpdate(ctx, buf, file.gcount())) {
            EVP_MD_CTX_free(ctx);
            return std::nullopt;
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hash_len = 0;
    if (1 != EVP_DigestFinal_ex(ctx, hash, &hash_len)) {
        EVP_MD_CTX_free(ctx);
        return std::nullopt;
    }
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return ss.str();
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 1) return 1;
    g_executable_path = argv[0];

    signal(SIGINT, handle_signal);

    std::cout << "============================================" << std::endl;
    std::cout << "   Virtual ECU Simulation V2 Started" << std::endl;
    std::cout << "   Press Ctrl+C to shut down." << std::endl;
    std::cout << "============================================" << std::endl;

    start_network_server();

    while (g_running) {
        switch (g_ecu_state.load()) {
            case EcuState::BOOT:
                run_boot_sequence(g_executable_path);
                break;
            case EcuState::APPLICATION:
                run_application_mode();
                break;
            case EcuState::UPDATE_PENDING:
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cout << "[STATE] UPDATE_PENDING — waiting for firmware transfer..." << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
                break;
            case EcuState::BRICKED:
                std::cerr << "[STATE] ECU is BRICKED. Halting." << std::endl;
                g_running = false;
                break;
        }
    }

    stop_network_server();
    std::cout << "--- Virtual ECU Simulation Shutting Down ---" << std::endl;
    return 0;
}


// ---------------------------------------------------------------------------
// Network server lifecycle
// ---------------------------------------------------------------------------
void start_network_server() {
    try {
        g_doip_server = std::make_unique<DoIPServer>(g_io_context, 13400);
        g_server_thread = std::thread([]() { g_doip_server->run(); });
    } catch (const std::exception& e) {
        std::cerr << "[NET] Failed to start server: " << e.what() << std::endl;
        g_ecu_state = EcuState::BRICKED;
    }
}

void stop_network_server() {
    if (g_server_thread.joinable())
        g_server_thread.join();
}


// ---------------------------------------------------------------------------
// Boot sequence (Phase 3: Secure Boot)
// ---------------------------------------------------------------------------
void run_boot_sequence(const std::string& executable_path) {
    std::cout << "[BOOT] Entering BOOT sequence..." << std::endl;

    // Load NVRAM
    if (!g_nvram.load()) {
        std::cerr << "[BOOT] CRITICAL: Failed to load NVRAM." << std::endl;
        g_dtc_manager.set_dtc(DTC::NVRAM_LOAD_FAILURE);
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    // Load persisted DTCs
    g_dtc_manager.load();

    // Secure Boot integrity check
    std::cout << "[BOOT] Performing Secure Boot integrity check..." << std::endl;

    auto golden_opt = g_nvram.get_string("FIRMWARE_HASH_GOLDEN");
    if (!golden_opt) {
        std::cerr << "[BOOT] CRITICAL: Golden hash not in NVRAM." << std::endl;
        g_dtc_manager.set_dtc(DTC::SECURE_BOOT_FAILURE);
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    auto calc_opt = calculate_file_hash(executable_path);
    if (!calc_opt) {
        std::cerr << "[BOOT] CRITICAL: Could not hash executable." << std::endl;
        g_dtc_manager.set_dtc(DTC::SECURE_BOOT_FAILURE);
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    std::cout << "  -> Golden Hash:     " << *golden_opt << std::endl;
    std::cout << "  -> Calculated Hash: " << *calc_opt   << std::endl;

    if (*golden_opt != *calc_opt) {
        std::cerr << "[BOOT] !!! INTEGRITY CHECK FAILED — entering BRICKED state." << std::endl;
        g_dtc_manager.set_dtc(DTC::SECURE_BOOT_FAILURE);
        g_ecu_state = EcuState::BRICKED;
        return;
    }

    std::cout << "[BOOT] Integrity check PASSED." << std::endl;

    auto fw_ver = g_nvram.get_string("FIRMWARE_VERSION");
    if (fw_ver) std::cout << "[BOOT] Firmware Version: " << *fw_ver << std::endl;

    std::cout << "[BOOT] Initializing peripherals (simulated)..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::cout << "[BOOT] Power-On Self-Test (POST) complete." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // Reset transient sensor state
    g_engine_temp_c = 20;
    g_fan_active    = false;

    std::cout << "[BOOT] Boot successful. -> APPLICATION state." << std::endl;
    g_ecu_state = EcuState::APPLICATION;
}


// ---------------------------------------------------------------------------
// Application / control loop (Phase 6: sensor simulation)
// ---------------------------------------------------------------------------
// Simple model:
//   - Engine temp rises ~1°C per 2s tick when running (max 120°C)
//   - Fan turns ON at >= 90°C, turns OFF at <= 70°C (hysteresis)
//   - DTC ENGINE_OVERTEMP set if temp >= 110°C
//   - DTC FAN_CONTROL_FAULT set if temp >= 100°C and fan is still off
// ---------------------------------------------------------------------------
void run_application_mode() {
    if (!g_running) return;

    int  temp     = g_engine_temp_c.load();
    bool fan      = g_fan_active.load();
    bool fault    = false;

    // Heating model
    int heat_rate = fan ? 0 : 1;          // Fan stops heating
    int cool_rate = fan ? 2 : 0;          // Fan cools when active
    temp += heat_rate - cool_rate;
    temp = std::max(20, std::min(temp, 120));

    // Fan control with hysteresis
    if (!fan && temp >= 90) {
        fan = true;
        std::lock_guard<std::mutex> lk(g_console_mutex);
        std::cout << "[APP] Cooling fan ACTIVATED at " << temp << "°C" << std::endl;
    } else if (fan && temp <= 70) {
        fan = false;
        std::lock_guard<std::mutex> lk(g_console_mutex);
        std::cout << "[APP] Cooling fan DEACTIVATED at " << temp << "°C" << std::endl;
    }

    // Fault detection
    if (temp >= 110) {
        g_dtc_manager.set_dtc(DTC::ENGINE_OVERTEMP);
        fault = true;
    }
    if (temp >= 100 && !fan) {
        g_dtc_manager.set_dtc(DTC::FAN_CONTROL_FAULT);
        fault = true;
    }

    g_engine_temp_c = temp;
    g_fan_active    = fan;

    {
        std::lock_guard<std::mutex> lk(g_console_mutex);
        std::cout << "[APP] Tick — Temp: " << temp << "°C"
                  << "  Fan: " << (fan ? "ON" : "OFF")
                  << (fault ? "  [FAULT]" : "")
                  << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
}


// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------
void handle_signal(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n[INFO] Shutdown signal received." << std::endl;
        if (g_doip_server) g_doip_server->stop();
        g_running = false;
    }
}


// ---------------------------------------------------------------------------
// OTA update application (Phase 4)
// ---------------------------------------------------------------------------
void apply_update(const std::string& current_executable_path) {
    std::cout << "[OTA] Applying update..." << std::endl;
    if (std::rename("update.bin", current_executable_path.c_str()) != 0) {
        perror("[OTA] CRITICAL: Failed to apply update");
    } else {
        // Set execute permissions via C++17 filesystem
        namespace fs = std::filesystem;
        try {
            fs::permissions(current_executable_path,
                fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                fs::perm_options::add);
        } catch (...) {}
        std::cout << "[OTA] Update applied. ECU will reboot." << std::endl;
    }
    if (g_doip_server) g_doip_server->stop();
    g_running = false;
}

#pragma once // Ensures this file is included only once per compilation.

#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <optional>

/**
 * @class NVRAMManager
 * @brief Simulates a simple Non-Volatile RAM by reading from and writing to a file.
 *
 * This class provides a basic key-value store that persists data in a plain text file,
 * mimicking how an ECU might store configuration data in its flash memory.
 */
class NVRAMManager {
public:
    /**
     * @brief Constructor.
     * @param filename The path to the file to be used for persistent storage.
     */
    explicit NVRAMManager(const std::string& filename) : m_filename(filename) {}

    /**
     * @brief Loads the key-value data from the NVRAM file.
     *
     * If the file doesn't exist, it creates a default configuration.
     * @return True if loading was successful, false otherwise.
     */
    bool load() {
        std::ifstream file(m_filename);
        if (!file.is_open()) {
            std::cout << "[NVRAM] No existing NVRAM file found. Creating default." << std::endl;
            return create_default_nvram();
        }

        std::string line;
        while (std::getline(file, line)) {
            // Simple parsing for "KEY=VALUE" format
            size_t delimiter_pos = line.find('=');
            if (delimiter_pos != std::string::npos) {
                std::string key = line.substr(0, delimiter_pos);
                std::string value = line.substr(delimiter_pos + 1);
                m_data[key] = value;
            }
        }
        std::cout << "[NVRAM] Successfully loaded data from " << m_filename << std::endl;
        return true;
    }

    /**
     * @brief Saves the current key-value data to the NVRAM file.
     * @return True if saving was successful, false otherwise.
     */
    bool save() {
        std::ofstream file(m_filename);
        if (!file.is_open()) {
            std::cerr << "[NVRAM] ERROR: Could not open file for writing: " << m_filename << std::endl;
            return false;
        }

        for (const auto& pair : m_data) {
            file << pair.first << "=" << pair.second << std::endl;
        }
        std::cout << "[NVRAM] Data saved to " << m_filename << std::endl;
        return true;
    }

    /**
     * @brief Retrieves a string value for a given key.
     * @param key The key to look up.
     * @return An std::optional containing the value if the key exists, otherwise std::nullopt.
     */
    std::optional<std::string> get_string(const std::string& key) const {
        auto it = m_data.find(key);
        if (it != m_data.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /**
     * @brief Sets a string value for a given key.
     * @param key The key to set.
     * @param value The value to associate with the key.
     */
    void set_string(const std::string& key, const std::string& value) {
        m_data[key] = value;
    }

private:
    std::string m_filename;
    std::map<std::string, std::string> m_data;

    /**
     * @brief Creates a default NVRAM file with initial values.
     */
    bool create_default_nvram() {
        m_data["FIRMWARE_VERSION"] = "1.0.0";
        m_data["ECU_SERIAL_NUMBER"] = "VECU-2023-001";
        // In Phase 4, this hash will be critical for secure boot.
        m_data["FIRMWARE_HASH_GOLDEN"] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"; // SHA-256 of an empty file
        return save();
    }
};

#pragma once

/**
 * @file dtc_manager.hpp
 * @brief Diagnostic Trouble Code (DTC) manager for the virtual ECU.
 *
 * Implements a UDS-compatible DTC subsystem. DTCs are stored in NVRAM as a
 * comma-separated list under the key "ACTIVE_DTCS". Each DTC is a 3-byte
 * code encoded as a 6-character uppercase hex string (e.g., "P0300" -> "P0300"
 * or raw 3-byte: 0x000001 -> "000001").
 *
 * Standard DTC format (ISO 14229 / ISO 15031-6):
 *   Byte 1: High byte  (category + code)
 *   Byte 2: Mid byte
 *   Byte 3: Status byte (active/pending/confirmed)
 *
 * Status byte flags (simplified):
 *   Bit 0: testFailed (currently active)
 *   Bit 3: confirmedDTC
 *   Bit 5: pendingDTC
 */

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "nvram_manager.hpp"

// ---------------------------------------------------------------------------
// Well-known DTC codes used by this ECU simulation
// ---------------------------------------------------------------------------
namespace DTC {
    // 3-byte DTC codes (stored as uint32_t using only low 24 bits)
    // Format: 0xHHMMLO where HH=high, MM=mid, LO=low
    constexpr uint32_t SECURE_BOOT_FAILURE       = 0x000001; // P0001 analog - integrity check failed
    constexpr uint32_t NVRAM_LOAD_FAILURE        = 0x000002; // NVRAM could not be read
    constexpr uint32_t OTA_HASH_MISMATCH         = 0x000010; // Received firmware failed hash check
    constexpr uint32_t OTA_FILE_WRITE_ERROR      = 0x000011; // Could not write update.bin
    constexpr uint32_t INVALID_UDS_SEQUENCE      = 0x000020; // UDS command received out of sequence
    constexpr uint32_t ENGINE_OVERTEMP           = 0x000030; // Simulated sensor fault
    constexpr uint32_t FAN_CONTROL_FAULT         = 0x000031; // Fan did not activate on overtemp

    // Status byte flags
    constexpr uint8_t STATUS_TEST_FAILED   = 0x01;
    constexpr uint8_t STATUS_CONFIRMED     = 0x08;
    constexpr uint8_t STATUS_PENDING       = 0x20;
}

// ---------------------------------------------------------------------------
// DTCEntry: one stored DTC with its status byte
// ---------------------------------------------------------------------------
struct DTCEntry {
    uint32_t code;   // 24-bit DTC code
    uint8_t  status; // Status byte
};

// ---------------------------------------------------------------------------
// DTCManager
// ---------------------------------------------------------------------------
class DTCManager {
public:
    explicit DTCManager(NVRAMManager& nvram) : m_nvram(nvram) {}

    /**
     * @brief Load DTCs from NVRAM into memory.
     *  Format stored: "HHMMLLSS,HHMMLLSS,..." (4-byte hex per entry)
     */
    void load() {
        m_dtcs.clear();
        auto stored = m_nvram.get_string("ACTIVE_DTCS");
        if (!stored || stored->empty() || *stored == "NONE") return;

        std::stringstream ss(*stored);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.size() == 8) {
                uint32_t val = std::stoul(token, nullptr, 16);
                DTCEntry e;
                e.code   = (val >> 8) & 0xFFFFFF;
                e.status = val & 0xFF;
                m_dtcs.push_back(e);
            }
        }
        std::cout << "[DTC] Loaded " << m_dtcs.size() << " DTC(s) from NVRAM." << std::endl;
    }

    /**
     * @brief Persist current DTC list to NVRAM and flush to disk.
     */
    void save() {
        if (m_dtcs.empty()) {
            m_nvram.set_string("ACTIVE_DTCS", "NONE");
        } else {
            std::ostringstream oss;
            for (size_t i = 0; i < m_dtcs.size(); ++i) {
                if (i > 0) oss << ",";
                uint32_t packed = ((m_dtcs[i].code & 0xFFFFFF) << 8) | m_dtcs[i].status;
                oss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << packed;
            }
            m_nvram.set_string("ACTIVE_DTCS", oss.str());
        }
        m_nvram.save();
    }

    /**
     * @brief Set (store) a DTC. If already present, OR the status byte.
     * @param code   24-bit DTC code (use DTC:: constants).
     * @param status Status byte flags (use DTC::STATUS_* constants).
     */
    void set_dtc(uint32_t code, uint8_t status = DTC::STATUS_TEST_FAILED | DTC::STATUS_CONFIRMED) {
        for (auto& e : m_dtcs) {
            if (e.code == code) {
                e.status |= status;
                std::cout << "[DTC] Updated existing DTC 0x"
                          << std::hex << std::uppercase << std::setw(6) << std::setfill('0') << code
                          << " status=0x" << (int)e.status << std::dec << std::endl;
                save();
                return;
            }
        }
        m_dtcs.push_back({code, status});
        std::cout << "[DTC] Set new DTC 0x"
                  << std::hex << std::uppercase << std::setw(6) << std::setfill('0') << code
                  << " status=0x" << (int)status << std::dec << std::endl;
        save();
    }

    /**
     * @brief Clear all DTCs (UDS $14 ClearDiagnosticInformation).
     */
    void clear_all() {
        m_dtcs.clear();
        std::cout << "[DTC] All DTCs cleared." << std::endl;
        save();
    }

    /**
     * @brief Return all stored DTCs (for UDS $19 ReadDTCInformation).
     */
    const std::vector<DTCEntry>& get_all() const {
        return m_dtcs;
    }

    /**
     * @brief Serialize all DTCs into a UDS $59 response payload.
     *
     * Response format for sub-function 0x02 (reportDTCByStatusMask):
     *   Byte 0:   0x59 (positive response SID)
     *   Byte 1:   0x02 (sub-function echo)
     *   Byte 2:   DTCStatusAvailabilityMask (0xFF = all supported)
     *   For each DTC (4 bytes each):
     *     Bytes N+0..N+2:  DTC code (big-endian, 3 bytes)
     *     Byte  N+3:       status byte
     *
     * @param status_mask  Filter — only include DTCs whose status & mask != 0.
     *                     Pass 0xFF to return all.
     */
    std::vector<uint8_t> build_read_dtc_response(uint8_t status_mask = 0xFF) const {
        std::vector<uint8_t> payload;
        payload.push_back(0x59);              // Positive response for $19
        payload.push_back(0x02);              // Sub-function echo
        payload.push_back(0xFF);              // DTCStatusAvailabilityMask

        for (const auto& e : m_dtcs) {
            if ((e.status & status_mask) == 0) continue;
            payload.push_back((e.code >> 16) & 0xFF);
            payload.push_back((e.code >>  8) & 0xFF);
            payload.push_back( e.code        & 0xFF);
            payload.push_back(e.status);
        }
        return payload;
    }

private:
    NVRAMManager&       m_nvram;
    std::vector<DTCEntry> m_dtcs;
};

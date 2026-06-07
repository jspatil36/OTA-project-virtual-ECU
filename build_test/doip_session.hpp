#pragma once

/**
 * @file doip_session.hpp
 * @brief Manages a single DoIP/UDS client session asynchronously.
 *
 * Supported UDS services:
 *   $14  ClearDiagnosticInformation
 *   $19  ReadDTCInformation (sub-function 0x02: reportDTCByStatusMask)
 *   $22  ReadDataByIdentifier
 *   $31  RoutineControl (0xFF00 = enter programming session)
 *   $34  RequestDownload
 *   $36  TransferData
 *   $37  RequestTransferExit
 */

#include <iostream>
#include <fstream>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <boost/asio.hpp>

#include "ecu_state.hpp"
#include "dtc_manager.hpp"
#include "ecdsa_verifier.hpp"

// ---------------------------------------------------------------------------
// Externals from main.cpp
// ---------------------------------------------------------------------------
extern std::atomic<EcuState>  g_ecu_state;
extern std::string             g_executable_path;
extern DTCManager              g_dtc_manager;
extern std::atomic<int>        g_engine_temp_c;
extern std::atomic<bool>       g_fan_active;
extern std::mutex              g_console_mutex;

extern std::optional<std::string> calculate_file_hash(const std::string& file_path);
extern void apply_update(const std::string& current_executable_path);

using boost::asio::ip::tcp;

// ---------------------------------------------------------------------------
// DoIP header (8 bytes, network byte order on the wire)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct DoIPHeader {
    uint8_t  protocol_version;
    uint8_t  inverse_protocol_version;
    uint16_t payload_type;
    uint32_t payload_length;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// UDS Data Identifiers (for $22 ReadDataByIdentifier)
// ---------------------------------------------------------------------------
namespace DataID {
    constexpr uint16_t ENGINE_TEMP   = 0xF400; // Engine temperature in °C (2 bytes, signed)
    constexpr uint16_t FAN_STATUS    = 0xF401; // Fan active: 0x01 = ON, 0x00 = OFF (1 byte)
    constexpr uint16_t FW_VERSION    = 0xF189; // Firmware version string (ISO 14229 standard ID)
    constexpr uint16_t ECU_SERIAL    = 0xF18C; // ECU serial number
}

// ---------------------------------------------------------------------------
// DoIPSession
// ---------------------------------------------------------------------------
class DoIPSession : public std::enable_shared_from_this<DoIPSession> {
public:
    explicit DoIPSession(tcp::socket socket)
        : m_socket(std::move(socket)),
          m_firmware_file_size(0),
          m_bytes_received(0)
    {}

    void start() {
        do_read_header();
    }

private:
    // -----------------------------------------------------------------------
    // Async read pipeline: header -> payload -> process
    // -----------------------------------------------------------------------
    void do_read_header() {
        auto self = shared_from_this();
        boost::asio::async_read(m_socket,
            boost::asio::buffer(&m_received_header, sizeof(DoIPHeader)),
            [this, self](const boost::system::error_code& ec, std::size_t) {
                if (!ec) {
                    m_received_header.payload_type   = ntohs(m_received_header.payload_type);
                    m_received_header.payload_length = ntohl(m_received_header.payload_length);
                    {
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        printf("[SESSION] Header -> Type: 0x%04X, Len: %u\n",
                               m_received_header.payload_type, m_received_header.payload_length);
                    }
                    do_read_payload();
                } else if (ec != boost::asio::error::eof) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] Header read error: " << ec.message() << std::endl;
                }
            });
    }

    void do_read_payload() {
        auto self = shared_from_this();
        m_payload.resize(m_received_header.payload_length);

        if (m_received_header.payload_length == 0) {
            process_message();
            return;
        }

        boost::asio::async_read(m_socket,
            boost::asio::buffer(m_payload.data(), m_received_header.payload_length),
            [this, self](const boost::system::error_code& ec, std::size_t) {
                if (!ec) {
                    process_message();
                } else if (ec != boost::asio::error::eof) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] Payload read error: " << ec.message() << std::endl;
                }
            });
    }

    // -----------------------------------------------------------------------
    // Message dispatch
    // -----------------------------------------------------------------------
    void process_message() {
        switch (m_received_header.payload_type) {
            case 0x0004: // Vehicle Identification Request
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cout << "[SESSION] Vehicle ID Request received." << std::endl;
                }
                do_write_vehicle_announcement();
                break;

            case 0x8001: // UDS over DoIP
                handle_uds_message();
                break;

            default:
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    printf("[SESSION] Unhandled type 0x%04X\n", m_received_header.payload_type);
                }
                do_read_header();
                break;
        }
    }

    // -----------------------------------------------------------------------
    // UDS service router
    // -----------------------------------------------------------------------
    void handle_uds_message() {
        if (m_payload.empty()) { do_read_header(); return; }

        uint8_t sid = m_payload[0];

        switch (sid) {

            // -----------------------------------------------------------------
            // $14 — ClearDiagnosticInformation
            // Payload: [0x14, GroupOfDTC_H, GroupOfDTC_M, GroupOfDTC_L]
            //   0xFFFFFF = clear all DTCs
            // -----------------------------------------------------------------
            case 0x14: {
                uint32_t group = 0xFFFFFF;
                if (m_payload.size() >= 4) {
                    group = ((uint32_t)m_payload[1] << 16)
                          | ((uint32_t)m_payload[2] <<  8)
                          |  (uint32_t)m_payload[3];
                }
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    printf("[SESSION] $14 ClearDTCInformation — group=0x%06X\n", group);
                }
                // We only support clear-all (0xFFFFFF); could extend per-group later.
                g_dtc_manager.clear_all();

                // Positive response: 0x54 (echo of 0x14 + 0x40)
                do_write_generic_response(0x8001, {0x54});
                return;
            }

            // -----------------------------------------------------------------
            // $19 — ReadDTCInformation
            // Sub-function 0x02: reportDTCByStatusMask
            // Payload: [0x19, 0x02, statusMask]
            // -----------------------------------------------------------------
            case 0x19: {
                if (m_payload.size() < 2) break;
                uint8_t sub_fn = m_payload[1];

                if (sub_fn != 0x02) {
                    // Negative response: sub-function not supported (0x12)
                    do_write_generic_response(0x8001, {0x7F, 0x19, 0x12});
                    return;
                }

                uint8_t mask = (m_payload.size() >= 3) ? m_payload[2] : 0xFF;
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    printf("[SESSION] $19 ReadDTCInformation — statusMask=0x%02X\n", mask);
                }

                auto response = g_dtc_manager.build_read_dtc_response(mask);
                do_write_generic_response(0x8001, response);
                return;
            }

            // -----------------------------------------------------------------
            // $22 — ReadDataByIdentifier
            // Payload: [0x22, DID_H, DID_L]
            // Can request multiple DIDs; we handle one per message for simplicity.
            // -----------------------------------------------------------------
            case 0x22: {
                if (m_payload.size() < 3) break;
                uint16_t did = ((uint16_t)m_payload[1] << 8) | m_payload[2];

                std::vector<uint8_t> response;
                response.push_back(0x62);      // Positive response SID
                response.push_back(m_payload[1]);
                response.push_back(m_payload[2]);

                bool supported = true;
                switch (did) {
                    case DataID::ENGINE_TEMP: {
                        int16_t temp = static_cast<int16_t>(g_engine_temp_c.load());
                        response.push_back((temp >> 8) & 0xFF);
                        response.push_back( temp       & 0xFF);
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cout << "[SESSION] $22 RDBI ENGINE_TEMP = " << temp << "°C" << std::endl;
                        break;
                    }
                    case DataID::FAN_STATUS: {
                        response.push_back(g_fan_active.load() ? 0x01 : 0x00);
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cout << "[SESSION] $22 RDBI FAN_STATUS = "
                                  << (g_fan_active.load() ? "ON" : "OFF") << std::endl;
                        break;
                    }
                    case DataID::FW_VERSION: {
                        // Populate from NVRAM if possible; fallback to "1.0.0"
                        std::string ver = "1.0.0";
                        response.insert(response.end(), ver.begin(), ver.end());
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cout << "[SESSION] $22 RDBI FW_VERSION = " << ver << std::endl;
                        break;
                    }
                    case DataID::ECU_SERIAL: {
                        std::string serial = "VECU-SIM-1234567";
                        response.insert(response.end(), serial.begin(), serial.end());
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cout << "[SESSION] $22 RDBI ECU_SERIAL = " << serial << std::endl;
                        break;
                    }
                    default:
                        supported = false;
                        break;
                }

                if (!supported) {
                    // Negative response: requestOutOfRange (0x31)
                    do_write_generic_response(0x8001, {0x7F, 0x22, 0x31});
                } else {
                    do_write_generic_response(0x8001, response);
                }
                return;
            }

            // -----------------------------------------------------------------
            // $31 — RoutineControl  (0xFF00 = enter programming session)
            // -----------------------------------------------------------------
            case 0x31: {
                if (m_payload.size() < 4) break;
                uint16_t routine_id = ((uint16_t)m_payload[2] << 8) | m_payload[3];

                if (routine_id == 0xFF00) {
                    {
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cout << "[SESSION] $31 Enter Programming Session." << std::endl;
                    }
                    g_ecu_state = EcuState::UPDATE_PENDING;
                    std::vector<uint8_t> rsp = {0x71};
                    rsp.insert(rsp.end(), m_payload.begin() + 1, m_payload.end());
                    do_write_generic_response(0x8001, rsp);
                    return;
                }
                break;
            }

            // -----------------------------------------------------------------
            // $34 — RequestDownload
            // -----------------------------------------------------------------
            case 0x34: {
                if (g_ecu_state != EcuState::UPDATE_PENDING) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] $34 received outside UPDATE_PENDING." << std::endl;
                    g_dtc_manager.set_dtc(DTC::INVALID_UDS_SEQUENCE);
                    break;
                }
                if (m_payload.size() < 10) break;

                m_firmware_file_size = ((uint32_t)m_payload[6] << 24)
                                     | ((uint32_t)m_payload[7] << 16)
                                     | ((uint32_t)m_payload[8] <<  8)
                                     |  (uint32_t)m_payload[9];
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cout << "[SESSION] $34 RequestDownload — size: "
                              << m_firmware_file_size << " bytes." << std::endl;
                }

                m_update_file.open("update.bin", std::ios::binary | std::ios::trunc);
                if (!m_update_file.is_open()) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] CRITICAL: Cannot open update.bin." << std::endl;
                    g_dtc_manager.set_dtc(DTC::OTA_FILE_WRITE_ERROR);
                    break;
                }
                m_bytes_received = 0;
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cout << "[SESSION] update.bin opened. Ready for transfer." << std::endl;
                }
                do_write_generic_response(0x8001, {0x74, 0x20, 0x10, 0x00});
                return;
            }

            // -----------------------------------------------------------------
            // $36 — TransferData
            // -----------------------------------------------------------------
            case 0x36: {
                if (g_ecu_state != EcuState::UPDATE_PENDING || !m_update_file.is_open()) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] $36 received in wrong state." << std::endl;
                    g_dtc_manager.set_dtc(DTC::INVALID_UDS_SEQUENCE);
                    break;
                }
                const char* data = reinterpret_cast<const char*>(m_payload.data() + 2);
                size_t data_size = m_payload.size() - 2;
                m_update_file.write(data, data_size);
                m_bytes_received += data_size;
                {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cout << "[SESSION] $36 chunk " << (int)m_payload[1]
                              << " — " << data_size << " bytes ("
                              << m_bytes_received << "/" << m_firmware_file_size << ")" << std::endl;
                }
                do_write_generic_response(0x8001, {0x76, m_payload[1]});
                return;
            }

            // -----------------------------------------------------------------
            // $37 — RequestTransferExit
            //
            // Payload format (Phase 7 — ECDSA):
            //   [0x37, sig_len_H, sig_len_L, <DER signature bytes>]
            //
            // The ECU verifies the ECDSA P-256 signature of the SHA-256 digest
            // of update.bin using the embedded public key (firmware_signing_pub.pem).
            //
            // Fallback (legacy / no sig file): if sig_len == 0, falls back to
            // SHA-256 hash comparison (payload = [0x37, 0x00, 0x00, <hash_string>]).
            // -----------------------------------------------------------------
            case 0x37: {
                if (g_ecu_state != EcuState::UPDATE_PENDING || !m_update_file.is_open()) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] $37 received in wrong state." << std::endl;
                    g_dtc_manager.set_dtc(DTC::INVALID_UDS_SEQUENCE);
                    break;
                }
                m_update_file.close();

                if (m_payload.size() < 3) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] $37 payload too short." << std::endl;
                    break;
                }

                uint16_t sig_len = ((uint16_t)m_payload[1] << 8) | m_payload[2];
                bool verify_ok = false;

                if (sig_len > 0 && m_payload.size() >= 3u + sig_len) {
                    // --- ECDSA verification path ---
                    std::vector<uint8_t> signature(m_payload.begin() + 3,
                                                   m_payload.begin() + 3 + sig_len);
                    {
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cout << "[SESSION] Verifying ECDSA signature ("
                                  << sig_len << " bytes)..." << std::endl;
                    }

                    ECDSAVerifier verifier;
                    if (verifier.load_public_key("firmware_signing_pub.pem")) {
                        verify_ok = verifier.verify_file("update.bin", signature);
                    } else {
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cerr << "[SESSION] Public key unavailable — OTA aborted." << std::endl;
                        g_dtc_manager.set_dtc(DTC::OTA_HASH_MISMATCH);
                    }
                } else {
                    // --- Legacy SHA-256 hash comparison path ---
                    auto calc_hash = calculate_file_hash("update.bin");
                    if (!calc_hash) {
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cerr << "[SESSION] Could not hash update.bin." << std::endl;
                        break;
                    }
                    std::string expected_hash(m_payload.begin() + 3, m_payload.end());
                    {
                        std::lock_guard<std::mutex> lk(g_console_mutex);
                        std::cout << "[SESSION] (Legacy mode) Hash verification" << std::endl;
                        std::cout << "  -> Expected:   " << expected_hash << std::endl;
                        std::cout << "  -> Calculated: " << *calc_hash   << std::endl;
                    }
                    verify_ok = (*calc_hash == expected_hash);
                    if (!verify_ok) g_dtc_manager.set_dtc(DTC::OTA_HASH_MISMATCH);
                }

                if (verify_ok) {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cout << "[SESSION] Firmware verification PASSED. Applying update." << std::endl;
                    do_write_generic_response(0x8001, {0x77});
                    apply_update(g_executable_path);
                } else {
                    g_dtc_manager.set_dtc(DTC::OTA_HASH_MISMATCH);
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] !!! VERIFICATION FAILED — OTA aborted." << std::endl;
                }
                return;
            }

            default:
                break;
        }

        // Fell through — unsupported or out-of-sequence
        {
            std::lock_guard<std::mutex> lk(g_console_mutex);
            printf("[SESSION] Unsupported/out-of-sequence UDS SID=0x%02X\n", sid);
        }
        do_read_header();
    }

    // -----------------------------------------------------------------------
    // Write helpers
    // -----------------------------------------------------------------------
    void do_write_generic_response(uint16_t payload_type,
                                    const std::vector<uint8_t>& payload) {
        auto self = shared_from_this();
        auto hdr  = std::make_shared<DoIPHeader>();
        hdr->protocol_version        = 0x02;
        hdr->inverse_protocol_version = ~hdr->protocol_version;
        hdr->payload_type            = htons(payload_type);
        hdr->payload_length          = htonl(static_cast<uint32_t>(payload.size()));

        std::vector<boost::asio::const_buffer> bufs;
        bufs.push_back(boost::asio::buffer(hdr.get(), sizeof(DoIPHeader)));
        bufs.push_back(boost::asio::buffer(payload));

        boost::asio::async_write(m_socket, bufs,
            [this, self, hdr](const boost::system::error_code& ec, std::size_t) {
                if (!ec) {
                    do_read_header();
                } else {
                    std::lock_guard<std::mutex> lk(g_console_mutex);
                    std::cerr << "[SESSION] Write error: " << ec.message() << std::endl;
                }
            });
    }

    void do_write_vehicle_announcement() {
        auto self = shared_from_this();
        std::string vin = "VECU-SIM-1234567";
        std::vector<uint8_t> payload(vin.begin(), vin.end());

        auto hdr = std::make_shared<DoIPHeader>();
        hdr->protocol_version         = 0x02;
        hdr->inverse_protocol_version = ~hdr->protocol_version;
        hdr->payload_type             = htons(0x0005);
        hdr->payload_length           = htonl(static_cast<uint32_t>(payload.size()));

        std::vector<boost::asio::const_buffer> bufs;
        bufs.push_back(boost::asio::buffer(hdr.get(), sizeof(DoIPHeader)));
        bufs.push_back(boost::asio::buffer(payload));

        boost::asio::async_write(m_socket, bufs,
            [this, self, hdr](const boost::system::error_code& ec, std::size_t bytes) {
                std::lock_guard<std::mutex> lk(g_console_mutex);
                if (!ec) {
                    std::cout << "[SESSION] Vehicle announcement sent (" << bytes << " bytes)." << std::endl;
                    do_read_header();
                } else {
                    std::cerr << "[SESSION] Write error: " << ec.message() << std::endl;
                }
            });
    }

    // -----------------------------------------------------------------------
    // Member data
    // -----------------------------------------------------------------------
    tcp::socket           m_socket;
    DoIPHeader            m_received_header;
    std::vector<uint8_t>  m_payload;
    std::ofstream         m_update_file;
    uint32_t              m_firmware_file_size;
    uint32_t              m_bytes_received;
};

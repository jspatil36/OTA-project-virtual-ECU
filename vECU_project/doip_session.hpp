#pragma once

#include <iostream>
#include <fstream>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <cstdio>
#include <boost/asio.hpp>

#include "ecu_state.hpp"

// Forward declare global state variables and functions from main.cpp
extern std::atomic<EcuState> g_ecu_state;
extern std::string g_executable_path;
extern std::optional<std::string> calculate_file_hash(const std::string& file_path);
extern void apply_update(const std::string& current_executable_path);

using boost::asio::ip::tcp;

#pragma pack(push, 1)
struct DoIPHeader {
    uint8_t  protocol_version;
    uint8_t  inverse_protocol_version;
    uint16_t payload_type;
    uint32_t payload_length;
};
#pragma pack(pop)

class DoIPSession : public std::enable_shared_from_this<DoIPSession> {
public:
    DoIPSession(tcp::socket socket)
        : m_socket(std::move(socket)),
          m_firmware_file_size(0),
          m_bytes_received(0)
    {}

    void start() {
        do_read_header();
    }

private:
    void do_read_header() {
        auto self = shared_from_this();
        boost::asio::async_read(m_socket,
            boost::asio::buffer(&m_received_header, sizeof(DoIPHeader)),
            [this, self](const boost::system::error_code& ec, std::size_t length) {
                if (!ec) {
                    m_received_header.payload_type = ntohs(m_received_header.payload_type);
                    m_received_header.payload_length = ntohl(m_received_header.payload_length);

                    printf("[SESSION] Received Header -> Type: 0x%04X, Length: %u\n", m_received_header.payload_type, m_received_header.payload_length);
                    do_read_payload();
                } else if (ec != boost::asio::error::eof) {
                    std::cerr << "[SESSION] Error reading header: " << ec.message() << std::endl;
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
            [this, self](const boost::system::error_code& ec, std::size_t length) {
                if (!ec) {
                    process_message();
                } else if (ec != boost::asio::error::eof) {
                    std::cerr << "[SESSION] Error reading payload: " << ec.message() << std::endl;
                }
            });
    }

    void process_message() {
        switch (m_received_header.payload_type) {
            case 0x0004: // Vehicle Identification Request
                std::cout << "[SESSION] Responding to Vehicle ID Request..." << std::endl;
                do_write_vehicle_announcement();
                break;
            case 0x8001: // UDS Message
                handle_uds_message();
                break;
            default:
                std::cout << "[SESSION] Received unhandled message type. Waiting for next message." << std::endl;
                do_read_header();
                break;
        }
    }

    void handle_uds_message() {
        if (m_payload.empty()) {
            do_read_header();
            return;
        }

        uint8_t service_id = m_payload[0];
        std::vector<uint8_t> response_payload;

        switch (service_id) {
            case 0x31: { // Routine Control
                if (m_payload.size() < 4) break;
                uint16_t routine_id = (m_payload[2] << 8) | m_payload[3];
                if (routine_id == 0xFF00) {
                    std::cout << "[SESSION] Received command: Enter Programming Session." << std::endl;
                    g_ecu_state = EcuState::UPDATE_PENDING;
                    response_payload.push_back(0x71);
                    response_payload.insert(response_payload.end(), m_payload.begin() + 1, m_payload.end());
                    do_write_generic_response(0x8001, response_payload);
                    return;
                }
                break;
            }

            case 0x34: { // Request Download
                if (g_ecu_state != EcuState::UPDATE_PENDING) {
                     std::cout << "[SESSION] ERROR: Request Download received outside of update session." << std::endl;
                     break;
                }
                if (m_payload.size() < 10) break;
                m_firmware_file_size = (m_payload[6] << 24) | (m_payload[7] << 16) | (m_payload[8] << 8) | m_payload[9];
                std::cout << "[SESSION] Received Request Download. Firmware size: " << m_firmware_file_size << " bytes." << std::endl;
                m_update_file.open("update.bin", std::ios::binary | std::ios::trunc);
                if (!m_update_file.is_open()) {
                    std::cerr << "[SESSION] CRITICAL: Could not open update.bin for writing." << std::endl;
                    break;
                }
                m_bytes_received = 0;
                std::cout << "[SESSION] Opened update.bin for writing. Ready for data transfer." << std::endl;
                response_payload.push_back(0x74);
                response_payload.push_back(0x20);
                response_payload.push_back(0x10);
                response_payload.push_back(0x00);
                do_write_generic_response(0x8001, response_payload);
                return;
            }

            case 0x36: { // Transfer Data
                if (g_ecu_state != EcuState::UPDATE_PENDING || !m_update_file.is_open()) {
                    std::cout << "[SESSION] ERROR: Transfer Data received in wrong state." << std::endl;
                    break;
                }
                const char* data_to_write = reinterpret_cast<const char*>(m_payload.data() + 2);
                size_t data_size = m_payload.size() - 2;
                m_update_file.write(data_to_write, data_size);
                m_bytes_received += data_size;
                std::cout << "[SESSION] Wrote " << data_size << " bytes to update.bin. Total received: " << m_bytes_received << "/" << m_firmware_file_size << std::endl;
                response_payload.push_back(0x76);
                response_payload.push_back(m_payload[1]);
                do_write_generic_response(0x8001, response_payload);
                return;
            }

            case 0x37: { // Request Transfer Exit
                if (g_ecu_state != EcuState::UPDATE_PENDING || !m_update_file.is_open()) {
                    std::cout << "[SESSION] ERROR: Transfer Exit received in wrong state." << std::endl;
                    break;
                }
                m_update_file.close();
                std::cout << "[SESSION] Finalizing file transfer." << std::endl;
                auto calculated_hash_opt = calculate_file_hash("update.bin");
                if (!calculated_hash_opt) {
                    std::cerr << "[SESSION] Failed to hash update.bin" << std::endl;
                    break;
                }
                std::string expected_hash(m_payload.begin() + 1, m_payload.end());
                std::cout << "  -> Expected Hash:   " << expected_hash << std::endl;
                std::cout << "  -> Calculated Hash: " << *calculated_hash_opt << std::endl;
                if (*calculated_hash_opt == expected_hash) {
                    std::cout << "[SESSION] Integrity check PASSED for new firmware." << std::endl;
                    do_write_generic_response(0x8001, {0x77});
                    apply_update(g_executable_path);
                } else {
                    std::cerr << "[SESSION] !!! INTEGRITY CHECK FAILED for new firmware !!!" << std::endl;
                }
                return;
            }
        }
        std::cout << "[SESSION] Received unsupported or out-of-sequence UDS command." << std::endl;
        do_read_header();
    }

    void do_write_generic_response(uint16_t payload_type, const std::vector<uint8_t>& payload) {
        auto self = shared_from_this();
        auto response_header = std::make_shared<DoIPHeader>();
        response_header->protocol_version = 0x02;
        response_header->inverse_protocol_version = ~response_header->protocol_version;
        response_header->payload_type = htons(payload_type);
        response_header->payload_length = htonl(payload.size());

        std::vector<boost::asio::const_buffer> buffers;
        buffers.push_back(boost::asio::buffer(response_header.get(), sizeof(DoIPHeader)));
        buffers.push_back(boost::asio::buffer(payload));

        boost::asio::async_write(m_socket, buffers,
            [this, self, response_header](const boost::system::error_code& ec, std::size_t bytes) {
                if (!ec) {
                    do_read_header();
                } else {
                    std::cerr << "[SESSION] Error on write: " << ec.message() << std::endl;
                }
            });
    }

    void do_write_vehicle_announcement() {
        auto self = shared_from_this();
        std::string vin = "VECU-SIM-1234567";
        std::vector<uint8_t> payload;
        payload.insert(payload.end(), vin.begin(), vin.end());
        auto response_header = std::make_shared<DoIPHeader>();
        response_header->protocol_version = 0x02;
        response_header->inverse_protocol_version = ~response_header->protocol_version;
        response_header->payload_type = htons(0x0005);
        response_header->payload_length = htonl(payload.size());
        std::vector<boost::asio::const_buffer> buffers;
        buffers.push_back(boost::asio::buffer(response_header.get(), sizeof(DoIPHeader)));
        buffers.push_back(boost::asio::buffer(payload));
        boost::asio::async_write(m_socket, buffers,
            [this, self, response_header](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                if (!ec) {
                    std::cout << "[SESSION] Sent " << bytes_transferred << " byte response." << std::endl;
                    do_read_header();
                } else {
                    std::cerr << "[SESSION] Error on write: " << ec.message() << std::endl;
                }
            });
    }

    tcp::socket m_socket;
    DoIPHeader m_received_header;
    std::vector<uint8_t> m_payload;
    std::ofstream m_update_file;
    uint32_t m_firmware_file_size;
    uint32_t m_bytes_received;
};

#pragma once

#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <boost/asio.hpp>

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
    DoIPSession(tcp::socket socket) : m_socket(std::move(socket)) {}

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

        boost::asio::async_read(m_socket,
            boost::asio::buffer(m_payload.data(), m_received_header.payload_length),
            [this, self](const boost::system::error_code& ec, std::size_t length) {
                if (!ec) {
                    // --- Message processing logic goes here ---
                    process_message();
                } else if (ec != boost::asio::error::eof) {
                    std::cerr << "[SESSION] Error reading payload: " << ec.message() << std::endl;
                }
            });
    }
    
    // **NEW**: Function to decide what to do with a complete message
    void process_message() {
        // For now, we only handle one type of message
        if (m_received_header.payload_type == 0x0004) { // Vehicle Identification Request
            std::cout << "[SESSION] Responding to Vehicle ID Request..." << std::endl;
            do_write_vehicle_announcement();
        } else {
            // In the future, handle other message types or send an error
            std::cout << "[SESSION] Received unhandled message type. Waiting for next message." << std::endl;
            do_read_header(); // Wait for the next message
        }
    }

    // **NEW**: Function to build and send the vehicle announcement response
    void do_write_vehicle_announcement() {
        auto self = shared_from_this();
        
        // 1. Construct the payload (VIN, Logical Address, etc.)
        //    For now, we'll use placeholder data.
        std::string vin = "VECU-SIM-1234567";
        std::vector<uint8_t> payload;
        payload.insert(payload.end(), vin.begin(), vin.end());
        // In a real implementation, you'd add more data here.

        // 2. Construct the response header
        DoIPHeader response_header;
        response_header.protocol_version = 0x02;
        response_header.inverse_protocol_version = ~response_header.protocol_version;
        response_header.payload_type = htons(0x0005); // Vehicle Announcement
        response_header.payload_length = htonl(payload.size());

        // 3. Create a list of buffers to send (header + payload)
        std::vector<boost::asio::const_buffer> buffers;
        buffers.push_back(boost::asio::buffer(&response_header, sizeof(DoIPHeader)));
        buffers.push_back(boost::asio::buffer(payload));

        // 4. Asynchronously send the data
        boost::asio::async_write(m_socket, buffers,
            [this, self](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                if (!ec) {
                    std::cout << "[SESSION] Sent " << bytes_transferred << " byte response." << std::endl;
                    // After responding, wait for the next request from the client
                    do_read_header();
                } else {
                    std::cerr << "[SESSION] Error on write: " << ec.message() << std::endl;
                }
            });
    }

    tcp::socket m_socket;
    DoIPHeader m_received_header;
    std::vector<uint8_t> m_payload;
};

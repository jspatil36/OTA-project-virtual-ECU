#include <iostream>
#include <vector>
#include <boost/asio.hpp>
#include <arpa/inet.h> // **FIX:** Added for htons() and htonl() on macOS/Linux

// **FIX:** Define the 'tcp' namespace alias for convenience
using boost::asio::ip::tcp;

// Use the same header definition as the server
#pragma pack(push, 1)
struct DoIPHeader {
    uint8_t  protocol_version;
    uint8_t  inverse_protocol_version;
    uint16_t payload_type;
    uint32_t payload_length;
};
#pragma pack(pop)

int main() {
    try {
        boost::asio::io_context io_context;
        tcp::socket socket(io_context);

        // Connect to the server running on localhost port 13400
        tcp::resolver resolver(io_context);
        boost::asio::connect(socket, resolver.resolve("localhost", "13400"));
        std::cout << "[CLIENT] Connected to server." << std::endl;

        // --- Construct the DoIP message ---
        DoIPHeader header;
        header.protocol_version = 0x02;
        header.inverse_protocol_version = ~header.protocol_version;
        header.payload_type = 0x0004; // Vehicle Identification Request
        header.payload_length = 0;

        std::cout << "[CLIENT] Sending Vehicle ID Request..." << std::endl;

        // Convert multi-byte fields to Network Byte Order (Big Endian) before sending
        DoIPHeader network_header = header;
        network_header.payload_type = htons(header.payload_type);
        network_header.payload_length = htonl(header.payload_length);

        // Send the header over the socket
        boost::asio::write(socket, boost::asio::buffer(&network_header, sizeof(network_header)));

        std::cout << "[CLIENT] Message sent successfully." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Client Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

#include <iostream>
#include <vector>
#include <string>
#include <boost/asio.hpp>
#include <arpa/inet.h> 

using boost::asio::ip::tcp;

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

        tcp::resolver resolver(io_context);
        boost::asio::connect(socket, resolver.resolve("localhost", "13400"));
        std::cout << "[CLIENT] Connected to server." << std::endl;

        // --- Construct and Send the Request ---
        DoIPHeader request_header;
        request_header.protocol_version = 0x02;
        request_header.inverse_protocol_version = ~request_header.protocol_version;
        request_header.payload_type = htons(0x0004); // Vehicle Identification Request
        request_header.payload_length = htonl(0);    // No payload for this request

        std::cout << "[CLIENT] Sending Vehicle ID Request..." << std::endl;
        boost::asio::write(socket, boost::asio::buffer(&request_header, sizeof(request_header)));
        std::cout << "[CLIENT] Message sent successfully." << std::endl;

        // --- NEW: Wait for and Read the Response ---
        std::cout << "[CLIENT] Waiting for response..." << std::endl;
        
        // 1. Read the header of the response message
        DoIPHeader response_header;
        boost::asio::read(socket, boost::asio::buffer(&response_header, sizeof(response_header)));

        // Convert response header fields from network to host byte order
        response_header.payload_type = ntohs(response_header.payload_type);
        response_header.payload_length = ntohl(response_header.payload_length);

        // 2. Read the payload of the response message
        std::vector<uint8_t> response_payload(response_header.payload_length);
        boost::asio::read(socket, boost::asio::buffer(response_payload));

        // 3. Print and validate the response
        std::cout << "\n--- [CLIENT] Response Received ---" << std::endl;
        printf("  Response Type: 0x%04X\n", response_header.payload_type);
        printf("  Payload Length: %u\n", response_header.payload_length);

        if (response_header.payload_type == 0x0005) { // Vehicle Announcement
            // Convert payload to a string to print the VIN
            std::string vin(response_payload.begin(), response_payload.end());
            std::cout << "  VIN: " << vin << std::endl;
            std::cout << "--- Verification SUCCESS ---" << std::endl;
        } else {
            std::cerr << "--- Verification FAILED: Unexpected response type. ---" << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Client Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

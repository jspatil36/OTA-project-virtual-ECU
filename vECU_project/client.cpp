#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <boost/asio.hpp>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

using boost::asio::ip::tcp;

#pragma pack(push, 1)
struct DoIPHeader {
    uint8_t  protocol_version;
    uint8_t  inverse_protocol_version;
    uint16_t payload_type;
    uint32_t payload_length;
};
#pragma pack(pop)

// UDS Service IDs
const uint8_t UDS_ROUTINE_CONTROL = 0x31;
const uint8_t UDS_REQUEST_DOWNLOAD = 0x34;
const uint8_t UDS_TRANSFER_DATA = 0x36;
const uint8_t UDS_REQUEST_TRANSFER_EXIT = 0x37;

// UDS Routine Identifiers
const uint16_t UDS_ENTER_PROGRAMMING_SESSION = 0xFF00;

// Function Prototypes
bool send_and_receive(tcp::socket& socket, uint16_t type, const std::vector<uint8_t>& payload, std::vector<uint8_t>& response_payload);
std::optional<std::string> calculate_file_hash(const std::string& file_path);


int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " --identify | --program | --update <file>" << std::endl;
        return 1;
    }

    try {
        boost::asio::io_context io_context;
        tcp::socket socket(io_context);
        tcp::resolver resolver(io_context);
        boost::asio::connect(socket, resolver.resolve("localhost", "13400"));
        std::cout << "[CLIENT] Connected to server." << std::endl;
        
        std::string command = argv[1];
        std::vector<uint8_t> response_payload;

        if (command == "--identify") {
            if (!send_and_receive(socket, 0x0004, {}, response_payload)) return 1;

        } else if (command == "--program") {
            std::vector<uint8_t> payload;
            payload.push_back(UDS_ROUTINE_CONTROL);
            payload.push_back(0x01); // startRoutine
            payload.push_back((UDS_ENTER_PROGRAMMING_SESSION >> 8) & 0xFF);
            payload.push_back(UDS_ENTER_PROGRAMMING_SESSION & 0xFF);
            if (!send_and_receive(socket, 0x8001, payload, response_payload)) return 1;

        } else if (command == "--update") {
            if (argc != 3) {
                std::cerr << "Usage: " << argv[0] << " --update <file>" << std::endl;
                return 1;
            }
            std::string file_path = argv[2];
            
            auto new_firmware_hash_opt = calculate_file_hash(file_path);
            if (!new_firmware_hash_opt) {
                std::cerr << "Error: Could not calculate hash of file " << file_path << std::endl;
                return 1;
            }
            std::cout << "[CLIENT] New firmware hash: " << *new_firmware_hash_opt << std::endl;

            std::ifstream file(file_path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                std::cerr << "Error: Cannot open file " << file_path << std::endl;
                return 1;
            }
            uint32_t file_size = file.tellg();
            file.seekg(0, std::ios::beg);

            // 1. Request Download
            std::vector<uint8_t> req_download_payload;
            req_download_payload.push_back(UDS_REQUEST_DOWNLOAD);
            req_download_payload.push_back(0x00);
            req_download_payload.push_back(0x44);
            req_download_payload.insert(req_download_payload.end(), {0x00, 0x00, 0x00, 0x00});
            req_download_payload.push_back((file_size >> 24) & 0xFF);
            req_download_payload.push_back((file_size >> 16) & 0xFF);
            req_download_payload.push_back((file_size >> 8) & 0xFF);
            req_download_payload.push_back(file_size & 0xFF);

            if (!send_and_receive(socket, 0x8001, req_download_payload, response_payload)) return 1;

            // 2. Transfer Data
            const size_t CHUNK_SIZE = 4096;
            std::vector<char> buffer(CHUNK_SIZE);
            uint8_t block_counter = 1;
            while (file.read(buffer.data(), CHUNK_SIZE) || file.gcount() > 0) {
                size_t bytes_read = file.gcount();
                std::cout << "[CLIENT] Transferring chunk " << (int)block_counter << " (" << bytes_read << " bytes)..." << std::endl;

                std::vector<uint8_t> transfer_payload;
                transfer_payload.push_back(UDS_TRANSFER_DATA);
                transfer_payload.push_back(block_counter++);
                transfer_payload.insert(transfer_payload.end(), buffer.begin(), buffer.begin() + bytes_read);
                
                if (!send_and_receive(socket, 0x8001, transfer_payload, response_payload)) return 1;
            }
            std::cout << "[CLIENT] File transfer complete." << std::endl;

            // 3. Request Transfer Exit
            std::cout << "[CLIENT] Sending Transfer Exit request..." << std::endl;
            std::vector<uint8_t> exit_payload;
            exit_payload.push_back(UDS_REQUEST_TRANSFER_EXIT);
            exit_payload.insert(exit_payload.end(), new_firmware_hash_opt->begin(), new_firmware_hash_opt->end());
            if (!send_and_receive(socket, 0x8001, exit_payload, response_payload)) return 1;

        } else {
            std::cerr << "Invalid command: " << command << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Client Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

bool send_and_receive(tcp::socket& socket, uint16_t type, const std::vector<uint8_t>& payload, std::vector<uint8_t>& response_payload) {
    DoIPHeader header;
    header.protocol_version = 0x02;
    header.inverse_protocol_version = ~header.protocol_version;
    header.payload_type = type;
    header.payload_length = payload.size();

    DoIPHeader network_header = header;
    network_header.payload_type = htons(header.payload_type);
    network_header.payload_length = htonl(header.payload_length);

    std::vector<boost::asio::const_buffer> request_buffers;
    request_buffers.push_back(boost::asio::buffer(&network_header, sizeof(network_header)));
    if (!payload.empty()) {
        request_buffers.push_back(boost::asio::buffer(payload));
    }
    boost::asio::write(socket, request_buffers);

    DoIPHeader response_header;
    boost::asio::read(socket, boost::asio::buffer(&response_header, sizeof(response_header)));
    response_header.payload_type = ntohs(response_header.payload_type);
    response_header.payload_length = ntohl(response_header.payload_length);
    
    response_payload.resize(response_header.payload_length);
    if (response_header.payload_length > 0) {
        boost::asio::read(socket, boost::asio::buffer(response_payload));
    }
    
    std::cout << "\n--- [CLIENT] Response Received ---" << std::endl;
    printf("  Response Type: 0x%04X, Length: %u\n", response_header.payload_type);

    if (response_header.payload_type == 0x8002) {
        std::cerr << "--- Verification FAILED: ECU returned an error. ---" << std::endl;
        return false;
    } else {
        // Specific checks for UDS positive/negative responses
        if (type == 0x8001 && !response_payload.empty() && response_payload[0] == 0x7F) {
            std::cerr << "--- Verification FAILED: ECU returned a Negative Response. ---" << std::endl;
            return false;
        }
        std::cout << "--- Verification SUCCESS ---" << std::endl;
    }
    return true;
}

std::optional<std::string> calculate_file_hash(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    EVP_MD_CTX* md_context = EVP_MD_CTX_new();
    if (!md_context || 1 != EVP_DigestInit_ex(md_context, EVP_sha256(), NULL)) {
        if(md_context) EVP_MD_CTX_free(md_context);
        return std::nullopt;
    }

    char buffer[1024];
    while (file.read(buffer, sizeof(buffer))) {
        if (1 != EVP_DigestUpdate(md_context, buffer, file.gcount())) {
            EVP_MD_CTX_free(md_context);
            return std::nullopt;
        }
    }
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

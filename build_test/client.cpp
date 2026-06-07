/**
 * @file client.cpp
 * @brief DoIP/UDS diagnostic client for the Virtual ECU simulation.
 *
 * Commands:
 *   --identify                    Vehicle ID Request (DoIP 0x0004)
 *   --program                     Enter Programming Session (UDS $31 / 0xFF00)
 *   --update <file> [--sig <sig>]  Full OTA firmware update sequence ($34/$36/$37)
 *                                   Without --sig: legacy SHA-256 hash mode
 *                                   With    --sig: ECDSA P-256 signature mode
 *   --read-dtcs                   Read all active DTCs (UDS $19 sub-fn 0x02)
 *   --clear-dtcs                  Clear all DTCs (UDS $14)
 *   --read-data <did_hex>         Read a Data Identifier (UDS $22)
 *                                   Known DIDs:
 *                                     F400  Engine temperature (°C, 2-byte signed)
 *                                     F401  Fan status (0=OFF, 1=ON)
 *                                     F189  Firmware version string
 *                                     F18C  ECU serial number
 */

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <boost/asio.hpp>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

using boost::asio::ip::tcp;

// ---------------------------------------------------------------------------
// DoIP framing
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
// UDS constants
// ---------------------------------------------------------------------------
const uint8_t  UDS_CLEAR_DTC             = 0x14;
const uint8_t  UDS_READ_DTC              = 0x19;
const uint8_t  UDS_READ_DATA_BY_ID       = 0x22;
const uint8_t  UDS_ROUTINE_CONTROL       = 0x31;
const uint8_t  UDS_REQUEST_DOWNLOAD      = 0x34;
const uint8_t  UDS_TRANSFER_DATA         = 0x36;
const uint8_t  UDS_REQUEST_TRANSFER_EXIT = 0x37;

const uint16_t ROUTINE_ENTER_PROG        = 0xFF00;

// ---------------------------------------------------------------------------
// Helper: pretty-print a byte vector as hex
// ---------------------------------------------------------------------------
static void print_hex(const std::vector<uint8_t>& data, const std::string& label = "") {
    if (!label.empty()) std::cout << label << " ";
    for (auto b : data)
        printf("%02X ", b);
    std::cout << std::endl;
}

// ---------------------------------------------------------------------------
// send_and_receive: send one DoIP message, read back the response.
// Returns false on network error or UDS negative response.
// ---------------------------------------------------------------------------
static bool send_and_receive(tcp::socket& socket,
                              uint16_t type,
                              const std::vector<uint8_t>& payload,
                              std::vector<uint8_t>& response_payload) {
    // Build and send header + payload
    DoIPHeader hdr;
    hdr.protocol_version         = 0x02;
    hdr.inverse_protocol_version = ~hdr.protocol_version;
    hdr.payload_type             = htons(type);
    hdr.payload_length           = htonl(static_cast<uint32_t>(payload.size()));

    std::vector<boost::asio::const_buffer> bufs;
    bufs.push_back(boost::asio::buffer(&hdr, sizeof(hdr)));
    if (!payload.empty())
        bufs.push_back(boost::asio::buffer(payload));
    boost::asio::write(socket, bufs);

    // Read response header
    DoIPHeader rsp_hdr;
    boost::asio::read(socket, boost::asio::buffer(&rsp_hdr, sizeof(rsp_hdr)));
    rsp_hdr.payload_type   = ntohs(rsp_hdr.payload_type);
    rsp_hdr.payload_length = ntohl(rsp_hdr.payload_length);

    // Read response payload
    response_payload.resize(rsp_hdr.payload_length);
    if (rsp_hdr.payload_length > 0)
        boost::asio::read(socket, boost::asio::buffer(response_payload));

    printf("\n[CLIENT] Response <- Type: 0x%04X, Len: %u\n",
           rsp_hdr.payload_type, rsp_hdr.payload_length);

    // Check for DoIP-level error
    if (rsp_hdr.payload_type == 0x8002) {
        std::cerr << "[CLIENT] ECU returned DoIP error response." << std::endl;
        return false;
    }

    // Check UDS negative response (SID = 0x7F)
    if (type == 0x8001 && !response_payload.empty() && response_payload[0] == 0x7F) {
        printf("[CLIENT] Negative Response — NRC: 0x%02X\n",
               response_payload.size() >= 3 ? response_payload[2] : 0xFF);
        return false;
    }

    std::cout << "[CLIENT] Positive response received." << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
// SHA-256 of a file (for OTA --update)
// ---------------------------------------------------------------------------
static std::optional<std::string> calculate_file_hash(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || 1 != EVP_DigestInit_ex(ctx, EVP_sha256(), NULL)) {
        if (ctx) EVP_MD_CTX_free(ctx);
        return std::nullopt;
    }
    char buf[4096];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0)
        EVP_DigestUpdate(ctx, buf, file.gcount());

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hash_len = 0;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    return ss.str();
}

// ---------------------------------------------------------------------------
// DTC helpers: decode the $59 response payload
// ---------------------------------------------------------------------------
static void print_dtc_response(const std::vector<uint8_t>& payload) {
    // Expected: [0x59, 0x02, availMask, DTC_H, DTC_M, DTC_L, status, ...]
    if (payload.size() < 3) {
        std::cout << "[CLIENT] No DTC data in response." << std::endl;
        return;
    }

    size_t dtc_count = (payload.size() - 3) / 4;
    std::cout << "[CLIENT] DTC Status Availability Mask: 0x"
              << std::hex << (int)payload[2] << std::dec << std::endl;

    if (dtc_count == 0) {
        std::cout << "[CLIENT] No DTCs stored." << std::endl;
        return;
    }

    std::cout << "[CLIENT] " << dtc_count << " DTC(s) found:" << std::endl;
    for (size_t i = 0; i < dtc_count; ++i) {
        size_t base = 3 + i * 4;
        uint32_t code = ((uint32_t)payload[base]   << 16)
                      | ((uint32_t)payload[base+1] <<  8)
                      |  (uint32_t)payload[base+2];
        uint8_t status = payload[base+3];
        printf("  DTC[%zu]: Code=0x%06X  Status=0x%02X  [%s%s%s]\n",
               i, code, status,
               (status & 0x01) ? "TEST_FAILED " : "",
               (status & 0x08) ? "CONFIRMED "   : "",
               (status & 0x20) ? "PENDING "     : "");
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " --identify | --program | --update <file> [--sig <sig_file>]"
                     " | --read-dtcs | --clear-dtcs | --read-data <did_hex>"
                  << std::endl;
        return 1;
    }

    try {
        boost::asio::io_context io_context;
        tcp::socket             socket(io_context);
        tcp::resolver           resolver(io_context);
        boost::asio::connect(socket, resolver.resolve("localhost", "13400"));
        std::cout << "[CLIENT] Connected to TargetECU on localhost:13400" << std::endl;

        std::string             command = argv[1];
        std::vector<uint8_t>    response;

        // ------------------------------------------------------------------
        // --identify
        // ------------------------------------------------------------------
        if (command == "--identify") {
            if (!send_and_receive(socket, 0x0004, {}, response)) return 1;
            if (!response.empty()) {
                std::string vin(response.begin(), response.end());
                std::cout << "[CLIENT] VIN: " << vin << std::endl;
            }

        // ------------------------------------------------------------------
        // --program   (enter programming session)
        // ------------------------------------------------------------------
        } else if (command == "--program") {
            std::vector<uint8_t> payload = {
                UDS_ROUTINE_CONTROL,
                0x01,  // startRoutine
                static_cast<uint8_t>((ROUTINE_ENTER_PROG >> 8) & 0xFF),
                static_cast<uint8_t>( ROUTINE_ENTER_PROG       & 0xFF)
            };
            if (!send_and_receive(socket, 0x8001, payload, response)) return 1;
            std::cout << "[CLIENT] ECU is now in UPDATE_PENDING state." << std::endl;

        // ------------------------------------------------------------------
        // --update <file>   (full OTA flow)
        // ------------------------------------------------------------------
        } else if (command == "--update") {
            if (argc < 3) {
                std::cerr << "Usage: " << argv[0]
                          << " --update <file> [--sig <sig_file>]" << std::endl;
                return 1;
            }
            const std::string file_path = argv[2];

            // Optional: --sig <signature_file>
            std::string sig_path;
            for (int i = 3; i < argc - 1; ++i) {
                if (std::string(argv[i]) == "--sig") {
                    sig_path = argv[i + 1];
                    break;
                }
            }

            // Load signature file if provided
            std::vector<uint8_t> signature;
            if (!sig_path.empty()) {
                std::ifstream sigfile(sig_path, std::ios::binary);
                if (!sigfile.is_open()) {
                    std::cerr << "[CLIENT] Cannot open signature file: " << sig_path << std::endl;
                    return 1;
                }
                signature.assign(
                    std::istreambuf_iterator<char>(sigfile),
                    std::istreambuf_iterator<char>()
                );
                std::cout << "[CLIENT] Loaded ECDSA signature: " << signature.size()
                          << " bytes from " << sig_path << std::endl;
            } else {
                std::cout << "[CLIENT] No --sig provided. Using legacy SHA-256 hash mode." << std::endl;
            }

            // Always compute SHA-256 hash (used in legacy mode)
            auto hash_opt = calculate_file_hash(file_path);
            if (!hash_opt) {
                std::cerr << "[CLIENT] Cannot hash file: " << file_path << std::endl;
                return 1;
            }
            std::cout << "[CLIENT] Firmware hash: " << *hash_opt << std::endl;

            std::ifstream file(file_path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                std::cerr << "[CLIENT] Cannot open: " << file_path << std::endl;
                return 1;
            }
            uint32_t file_size = static_cast<uint32_t>(file.tellg());
            file.seekg(0, std::ios::beg);
            std::cout << "[CLIENT] Firmware size: " << file_size << " bytes." << std::endl;

            // 1. Request Download ($34)
            std::vector<uint8_t> req_dl = {
                UDS_REQUEST_DOWNLOAD,
                0x00, 0x44,
                0x00, 0x00, 0x00, 0x00,
                static_cast<uint8_t>((file_size >> 24) & 0xFF),
                static_cast<uint8_t>((file_size >> 16) & 0xFF),
                static_cast<uint8_t>((file_size >>  8) & 0xFF),
                static_cast<uint8_t>( file_size        & 0xFF)
            };
            if (!send_and_receive(socket, 0x8001, req_dl, response)) return 1;

            // 2. Transfer Data ($36) — 4 KB chunks
            constexpr size_t CHUNK = 4096;
            std::vector<char> buf(CHUNK);
            uint8_t block = 1;
            while (file.read(buf.data(), CHUNK) || file.gcount() > 0) {
                size_t n = file.gcount();
                std::cout << "[CLIENT] Chunk " << (int)block << " — " << n << " bytes..." << std::endl;
                std::vector<uint8_t> chunk_pld;
                chunk_pld.push_back(UDS_TRANSFER_DATA);
                chunk_pld.push_back(block++);
                chunk_pld.insert(chunk_pld.end(), buf.begin(), buf.begin() + n);
                if (!send_and_receive(socket, 0x8001, chunk_pld, response)) return 1;
            }
            std::cout << "[CLIENT] Transfer complete." << std::endl;

            // 3. Request Transfer Exit ($37)
            // Payload: [0x37 | sig_len_H | sig_len_L | <sig_bytes OR hash_string>]
            std::vector<uint8_t> exit_pld;
            exit_pld.push_back(UDS_REQUEST_TRANSFER_EXIT);

            if (!signature.empty()) {
                // ECDSA mode
                uint16_t sig_len = static_cast<uint16_t>(signature.size());
                exit_pld.push_back((sig_len >> 8) & 0xFF);
                exit_pld.push_back( sig_len       & 0xFF);
                exit_pld.insert(exit_pld.end(), signature.begin(), signature.end());
                std::cout << "[CLIENT] $37 ECDSA mode — sig_len=" << sig_len << std::endl;
            } else {
                // Legacy hash mode: sig_len = 0, followed by hash string
                exit_pld.push_back(0x00);
                exit_pld.push_back(0x00);
                exit_pld.insert(exit_pld.end(), hash_opt->begin(), hash_opt->end());
                std::cout << "[CLIENT] $37 legacy hash mode." << std::endl;
            }

            if (!send_and_receive(socket, 0x8001, exit_pld, response)) return 1;
            std::cout << "[CLIENT] OTA update completed. ECU is rebooting." << std::endl;

        // ------------------------------------------------------------------
        // --read-dtcs
        // ------------------------------------------------------------------
        } else if (command == "--read-dtcs") {
            // $19 sub-function 0x02, mask 0xFF = all DTCs
            std::vector<uint8_t> payload = {UDS_READ_DTC, 0x02, 0xFF};
            if (!send_and_receive(socket, 0x8001, payload, response)) return 1;
            print_dtc_response(response);

        // ------------------------------------------------------------------
        // --clear-dtcs
        // ------------------------------------------------------------------
        } else if (command == "--clear-dtcs") {
            // $14, group 0xFFFFFF = clear all
            std::vector<uint8_t> payload = {UDS_CLEAR_DTC, 0xFF, 0xFF, 0xFF};
            if (!send_and_receive(socket, 0x8001, payload, response)) return 1;
            std::cout << "[CLIENT] All DTCs cleared." << std::endl;

        // ------------------------------------------------------------------
        // --read-data <did_hex>
        // ------------------------------------------------------------------
        } else if (command == "--read-data") {
            if (argc != 3) {
                std::cerr << "Usage: " << argv[0] << " --read-data <did_hex>"
                          << "  (e.g. F400 for engine temp)" << std::endl;
                return 1;
            }
            uint16_t did = static_cast<uint16_t>(std::stoul(argv[2], nullptr, 16));
            std::vector<uint8_t> payload = {
                UDS_READ_DATA_BY_ID,
                static_cast<uint8_t>((did >> 8) & 0xFF),
                static_cast<uint8_t>( did        & 0xFF)
            };
            if (!send_and_receive(socket, 0x8001, payload, response)) return 1;

            // Parse and display based on DID
            if (response.size() >= 5) {
                uint16_t resp_did = ((uint16_t)response[1] << 8) | response[2];
                switch (resp_did) {
                    case 0xF400: {
                        int16_t temp = (int16_t)(((uint16_t)response[3] << 8) | response[4]);
                        std::cout << "[CLIENT] ENGINE_TEMP = " << temp << " °C" << std::endl;
                        break;
                    }
                    case 0xF401: {
                        std::cout << "[CLIENT] FAN_STATUS = "
                                  << (response[3] ? "ON" : "OFF") << std::endl;
                        break;
                    }
                    default:
                        print_hex(std::vector<uint8_t>(response.begin() + 3, response.end()),
                                  "[CLIENT] Raw data:");
                        break;
                }
            } else if (response.size() >= 3) {
                // Short response (e.g. FW version string)
                std::string val(response.begin() + 3, response.end());
                std::cout << "[CLIENT] Value = \"" << val << "\"" << std::endl;
            }

        // ------------------------------------------------------------------
        // Unknown
        // ------------------------------------------------------------------
        } else {
            std::cerr << "[CLIENT] Unknown command: " << command << std::endl;
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "[CLIENT] Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}

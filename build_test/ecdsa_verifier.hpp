#pragma once

/**
 * @file ecdsa_verifier.hpp
 * @brief ECDSA P-256 firmware signature verification.
 *
 * The ECU holds only the PUBLIC key (firmware_signing_pub.pem).
 * The private key lives offline with the firmware signer.
 *
 * Signing workflow (offline, with private key):
 *   openssl dgst -sha256 -sign firmware_signing_key.pem \
 *               -out firmware.sig TargetECU_v2.bin
 *
 * The doip_client reads both the firmware file and firmware.sig,
 * then sends them in the $37 RequestTransferExit payload:
 *   [0x37 | sig_len_H | sig_len_L | <sig bytes>]
 *
 * The ECU (this module) verifies the signature against the
 * SHA-256 digest of update.bin using the embedded public key.
 */

#include <iostream>
#include <fstream>
#include <iterator>
#include <vector>
#include <string>

#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/err.h>

class ECDSAVerifier {
public:
    /**
     * @brief Load the public key from a PEM file.
     * @param pubkey_path  Path to the PEM-encoded EC public key.
     * @return true on success.
     */
    bool load_public_key(const std::string& pubkey_path) {
        FILE* fp = fopen(pubkey_path.c_str(), "r");
        if (!fp) {
            std::cerr << "[ECDSA] Cannot open public key: " << pubkey_path << std::endl;
            return false;
        }
        m_pkey = PEM_read_PUBKEY(fp, nullptr, nullptr, nullptr);
        fclose(fp);
        if (!m_pkey) {
            std::cerr << "[ECDSA] Failed to parse public key." << std::endl;
            print_openssl_error();
            return false;
        }
        std::cout << "[ECDSA] Public key loaded from: " << pubkey_path << std::endl;
        return true;
    }

    /**
     * @brief Verify an ECDSA signature over the SHA-256 digest of a file.
     *
     * @param file_path       Path to the data file (e.g. "update.bin").
     * @param signature       Raw DER-encoded ECDSA signature bytes.
     * @return true if the signature is valid.
     */
    bool verify_file(const std::string&          file_path,
                     const std::vector<uint8_t>& signature) const {
        if (!m_pkey) {
            std::cerr << "[ECDSA] No public key loaded." << std::endl;
            return false;
        }

        // Read file contents
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[ECDSA] Cannot open file: " << file_path << std::endl;
            return false;
        }
        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(file)),
             std::istreambuf_iterator<char>()
        );

        // Create digest context
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) return false;

        int rc = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, m_pkey);
        if (rc != 1) {
            EVP_MD_CTX_free(ctx);
            print_openssl_error();
            return false;
        }

        rc = EVP_DigestVerifyUpdate(ctx, data.data(), data.size());
        if (rc != 1) {
            EVP_MD_CTX_free(ctx);
            print_openssl_error();
            return false;
        }

        rc = EVP_DigestVerifyFinal(ctx,
                                   signature.data(),
                                   signature.size());
        EVP_MD_CTX_free(ctx);

        if (rc == 1) {
            std::cout << "[ECDSA] Signature VALID." << std::endl;
            return true;
        } else {
            std::cerr << "[ECDSA] Signature INVALID." << std::endl;
            print_openssl_error();
            return false;
        }
    }

    ~ECDSAVerifier() {
        if (m_pkey) {
            EVP_PKEY_free(m_pkey);
            m_pkey = nullptr;
        }
    }

private:
    EVP_PKEY* m_pkey = nullptr;

    static void print_openssl_error() {
        unsigned long err = ERR_get_error();
        if (err) {
            char buf[256];
            ERR_error_string_n(err, buf, sizeof(buf));
            std::cerr << "[ECDSA] OpenSSL: " << buf << std::endl;
        }
    }
};

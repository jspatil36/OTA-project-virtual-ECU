#!/usr/bin/env bash
# generate_keys.sh
# Generates an ECDSA P-256 key pair for firmware signing.
#
# Outputs:
#   firmware_signing_key.pem   -- private key  (keep secret, use offline)
#   firmware_signing_pub.pem   -- public key   (embed in ECU)
#
# Usage:
#   chmod +x generate_keys.sh
#   ./generate_keys.sh

set -euo pipefail

PRIVATE_KEY="firmware_signing_key.pem"
PUBLIC_KEY="firmware_signing_pub.pem"

echo "[KEYGEN] Generating ECDSA P-256 private key..."
openssl ecparam -name prime256v1 -genkey -noout -out "$PRIVATE_KEY"
echo "[KEYGEN] Private key written to: $PRIVATE_KEY"

echo "[KEYGEN] Extracting public key..."
openssl ec -in "$PRIVATE_KEY" -pubout -out "$PUBLIC_KEY"
echo "[KEYGEN] Public key written to: $PUBLIC_KEY"

echo ""
echo "[KEYGEN] Done. To sign a firmware file:"
echo "  openssl dgst -sha256 -sign $PRIVATE_KEY -out firmware.sig firmware.bin"
echo ""
echo "[KEYGEN] To verify manually:"
echo "  openssl dgst -sha256 -verify $PUBLIC_KEY -signature firmware.sig firmware.bin"

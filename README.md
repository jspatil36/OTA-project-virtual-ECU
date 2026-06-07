# Virtual Automotive ECU with Secure OTA Updates

# **Project Report and Manual**

**Version:** 2.0
**Date:** June 6, 2026

---

### **Table of Contents**
1. [**Project Report**](#1-project-report)
   - [Introduction](#11-introduction)
   - [System Architecture](#12-system-architecture)
   - [Core Features](#13-core-features)
2. [**Project Development History**](#2-project-development-history)
   - [Phase 1: Basic Communication & Control](#21-phase-1-basic-communication--control)
   - [Phase 2: Protocol Implementation](#22-phase-2-protocol-implementation)
   - [Phase 3: Secure Boot](#23-phase-3-secure-boot)
   - [Phase 4: Over-the-Air (OTA) Update](#24-phase-4-over-the-air-ota-update)
   - [Phase 5: DTC Subsystem](#25-phase-5-dtc-subsystem)
   - [Phase 6: Sensor Control Loop & RDBI](#26-phase-6-sensor-control-loop--rdbi)
   - [Phase 7: ECDSA Firmware Signing](#27-phase-7-ecdsa-firmware-signing)
3. [**User & Developer Manual**](#3-user--developer-manual)
   - [Prerequisites](#31-prerequisites)
   - [Project File Structure](#32-project-file-structure)
   - [Compilation](#33-compilation)
   - [Full Usage Walkthrough: Performing an OTA Update](#34-full-usage-walkthrough-performing-an-ota-update)
   - [Diagnostics: Reading and Clearing DTCs](#35-diagnostics-reading-and-clearing-dtcs)
   - [Reading Live ECU Data (RDBI)](#36-reading-live-ecu-data-rdbi)
   - [Secure OTA with ECDSA Firmware Signing](#37-secure-ota-with-ecdsa-firmware-signing)
   - [Alternative Setup: Simulating a Virtual CAN Bus](#38-alternative-setup-simulating-a-virtual-can-bus)

---

## **1. Project Report**

### **1.1. Introduction**
This document details a C++ project that simulates an automotive Electronic Control Unit (ECU) with production-grade features. The primary goal was to build a realistic, command-line ECU simulation capable of **Secure Boot**, receiving **Over-the-Air (OTA) firmware updates** with **ECDSA signature verification**, storing and reporting **Diagnostic Trouble Codes (DTCs)**, and running a **simulated sensor control loop** queryable over the network via standard UDS services.

The project uses a client-server architecture over TCP/IP, emulating the interaction between a diagnostic/flashing tool and an ECU in a vehicle network.

### **1.2. System Architecture**

The simulation comprises two executables and several supporting header modules, operating on a client-server model over TCP/IP (port 13400).

**`TargetECU` (Server):** The ECU simulation. Runs an asynchronous TCP server (Boost.Asio, port 13400) emulating DoIP. Multi-threaded: network I/O runs on a dedicated thread; the main application logic and state machine run on the main thread.

**`doip_client` (Client):** A CLI diagnostic/flashing tool. Connects to `TargetECU` and sends structured UDS messages wrapped in DoIP frames.

**State Machine:** `TargetECU` is governed by `EcuState`:
- **`BOOT`** — Integrity check, NVRAM load, DTC restore, peripheral init.
- **`APPLICATION`** — Normal mode: runs the sensor control loop.
- **`UPDATE_PENDING`** — Ready to receive firmware via UDS transfer services.
- **`BRICKED`** — Terminal fault state. Triggered by hash mismatch, NVRAM failure, or network init failure.

### **1.3. Core Features**

**Secure Boot (Phase 3):** At startup, `TargetECU` SHA-256 hashes its own executable and compares it to the "golden hash" stored in `nvram.dat`. A mismatch sets DTC `0x000001` (SECURE_BOOT_FAILURE) and enters `BRICKED`.

**Persistent Storage (NVRAM):** `NVRAMManager` provides a key-value store persisted to `nvram.dat`. Stores firmware hash, version, serial number, and the active DTC list.

**Diagnostic Trouble Codes (Phase 5):** `DTCManager` manages a list of `DTCEntry` records (24-bit code + status byte) in NVRAM. DTCs are set automatically on faults (secure boot failure, OTA hash mismatch, out-of-sequence UDS, sensor overtemp). Supports UDS `$14` ClearDiagnosticInformation and `$19` ReadDTCInformation (sub-function 0x02).

**Sensor Control Loop (Phase 6):** `run_application_mode()` simulates an engine thermal model every 2 seconds. Engine temperature rises 1°C/tick when the fan is off. The fan activates at ≥ 90°C, deactivates at ≤ 70°C (hysteresis). DTCs `ENGINE_OVERTEMP` and `FAN_CONTROL_FAULT` are set on threshold violations. Live data is readable via UDS `$22` ReadDataByIdentifier.

**Asynchronous DoIP/UDS Communication:** Fully asynchronous Boost.Asio pipeline: `DoIPServer` accepts connections; `DoIPSession` handles the per-client UDS request-response lifecycle. Supported UDS services:

| SID  | Service                     | Notes                                          |
|------|-----------------------------|------------------------------------------------|
| $14  | ClearDiagnosticInformation  | Group 0xFFFFFF = clear all                     |
| $19  | ReadDTCInformation          | Sub-function 0x02 (reportDTCByStatusMask)      |
| $22  | ReadDataByIdentifier        | DIDs: F400 (temp), F401 (fan), F189, F18C      |
| $31  | RoutineControl              | 0xFF00 = enter programming session             |
| $34  | RequestDownload             | Initiates firmware transfer                    |
| $36  | TransferData                | 4 KB chunks, block counter                     |
| $37  | RequestTransferExit         | ECDSA verification (or legacy SHA-256 fallback)|

**ECDSA Firmware Signing (Phase 7):** The `$37` handler now supports two modes:
- **ECDSA mode** (recommended): Client sends the DER-encoded ECDSA P-256 signature of the firmware's SHA-256 digest. ECU verifies using the embedded `firmware_signing_pub.pem`. Proves both integrity (what) and authenticity (who).
- **Legacy mode** (fallback): Client sends `sig_len=0` followed by the hex SHA-256 hash string. Same as the original Phase 4 behavior, included for backward compatibility.

**OTA Update Mechanism:** Complete multi-stage flow: Routine Control ($31) → Request Download ($34) → Transfer Data ($36, 4 KB chunks) → Request Transfer Exit ($37, signature/hash) → `std::rename` + `std::filesystem::permissions` → graceful shutdown (reboot simulation).

---

## **2. Project Development History**

### **2.1. Phase 1: Basic Communication & Control**
- Single-file `main.cpp`, synchronous TCP server via Boost.Asio.
- Process control: `SIGINT` handler for graceful `Ctrl+C` shutdown.
- Connection testing via `netcat`.

### **2.2. Phase 2: Protocol Implementation**
- Refactored to `DoIPServer` + `DoIPSession` async classes.
- Defined `DoIPHeader` struct (8 bytes, `#pragma pack`).
- Created `doip_client` executable.
- Resolved: `use of undeclared identifier 'tcp'`, `htons` missing include, `EcuState` redefinition → moved to `ecu_state.hpp`.

### **2.3. Phase 3: Secure Boot**
- Added `calculate_file_hash()` using OpenSSL `EVP_DigestInit/Update/Final`.
- Golden hash workflow: `shasum -a 256 TargetECU` → stored in `nvram.dat`.
- Boot sequence halts with `BRICKED` on hash mismatch.

### **2.4. Phase 4: Over-the-Air (OTA) Update**
- `--program` flag: sends `$31` Routine Control to enter `UPDATE_PENDING`.
- `--update <file>`: `$34` handshake → `$36` 4 KB chunk loop → `$37` finalization.
- `apply_update()`: `std::rename("update.bin", executable_path)` + C++17 `std::filesystem::permissions`.

### **2.5. Phase 5: DTC Subsystem**
- New file: `dtc_manager.hpp` (`DTCManager` class + `DTC` namespace of fault codes).
- DTCs are stored in NVRAM as a packed hex string: `"HHMMLLSS,..."`.
- Added `$14` ClearDiagnosticInformation and `$19` ReadDTCInformation (sub-fn 0x02) handlers to `DoIPSession`.
- DTCs set automatically: `SECURE_BOOT_FAILURE`, `NVRAM_LOAD_FAILURE`, `OTA_HASH_MISMATCH`, `OTA_FILE_WRITE_ERROR`, `INVALID_UDS_SEQUENCE`, `ENGINE_OVERTEMP`, `FAN_CONTROL_FAULT`.
- Client: added `--read-dtcs` and `--clear-dtcs` commands with decoded human-readable output.

### **2.6. Phase 6: Sensor Control Loop & RDBI**
- Replaced the placeholder `run_application_mode()` with a thermal simulation loop.
- Global atomics `g_engine_temp_c` and `g_fan_active` are readable from both the main thread and the network thread (no lock needed on atomics).
- Added `$22` ReadDataByIdentifier handler. Supported DIDs:
  - `0xF400` — Engine temperature (2-byte signed int16, °C)
  - `0xF401` — Fan status (1 byte: 0x00=OFF, 0x01=ON)
  - `0xF189` — Firmware version string
  - `0xF18C` — ECU serial number
- Client: added `--read-data <did_hex>` command with auto-decoded output per DID.
- Added `g_console_mutex` to prevent log interleaving between main and server threads.

### **2.7. Phase 7: ECDSA Firmware Signing**
- New files: `ecdsa_verifier.hpp`, `generate_keys.sh`.
- Key pair: ECDSA P-256 (`prime256v1`). Private key stays offline; ECU only has the public key.
- `$37` payload format changed: `[0x37 | sig_len_H | sig_len_L | <DER signature>]`. When `sig_len=0`, falls back to legacy hash mode.
- `ECDSAVerifier::verify_file()` uses `EVP_DigestVerifyInit/Update/Final` — the modern OpenSSL EVP API.
- Client: `--update <file> --sig <sig_file>` reads the DER signature file and embeds it in the `$37` payload.

---

## **3. User & Developer Manual**

### **3.1. Prerequisites**
- C++17 compliant compiler (Clang on macOS, GCC on Linux).
- CMake ≥ 3.15.
- **OpenSSL** (≥ 1.1.1) library and headers.
- **Boost** library and headers (Boost.Asio, header-only for Asio itself).

On macOS with Homebrew:
```bash
brew install cmake openssl boost
```

### **3.2. Project File Structure**
```
vECU_project/
├── CMakeLists.txt          Build script
├── ecu_state.hpp           EcuState enum
├── nvram_manager.hpp       Key-value NVRAM persistence
├── dtc_manager.hpp         DTC storage, set/clear/serialize  [NEW v2.0]
├── ecdsa_verifier.hpp      ECDSA P-256 signature verification [NEW v2.0]
├── doip_server.hpp         Async TCP acceptor
├── doip_session.hpp        Per-connection UDS handler
├── main.cpp                TargetECU entry point + control loop
├── client.cpp              doip_client CLI tool
└── generate_keys.sh        Key pair generation script         [NEW v2.0]

build/
├── TargetECU               ECU server executable
├── doip_client             Diagnostic client executable
├── nvram.dat               Persisted NVRAM (hash, version, DTCs)
└── firmware_signing_pub.pem  ECU public key (copy here after keygen)
```

### **3.3. Compilation**
```bash
mkdir build && cd build
cmake ..
make
```
Both `TargetECU` and `doip_client` will be created in `build/`.

### **3.4. Full Usage Walkthrough: Performing an OTA Update**

All commands are run from the `build/` directory.

#### **Step 1 — Initial Setup (Version 1)**
```bash
make                            # Compile TargetECU
shasum -a 256 TargetECU         # Note the hash
```
Create `build/nvram.dat`:
```
FIRMWARE_VERSION=1.0.0
ECU_SERIAL_NUMBER=VECU-2023-001
FIRMWARE_HASH_GOLDEN=<hash from above>
ACTIVE_DTCS=NONE
```

#### **Step 2 — Create Version 2 firmware**
```bash
# Edit main.cpp: change "V2 Started" in the startup banner
make
cp TargetECU TargetECU_v2.bin
# Revert main.cpp and rebuild V1
make
```

#### **Step 3 — Execute the update (two terminals)**
Terminal 1:
```bash
./TargetECU
```
Terminal 2:
```bash
./doip_client --program
./doip_client --update TargetECU_v2.bin   # legacy hash mode
# OR with ECDSA (see §3.7):
./doip_client --update TargetECU_v2.bin --sig TargetECU_v2.sig
```

#### **Step 4 — Verify**
The ECU applies the update, logs success, and shuts down. Run `./TargetECU` again; the V2 banner appears. Secure Boot will fail until you update `FIRMWARE_HASH_GOLDEN` in `nvram.dat` to the V2 hash.

---

### **3.5. Diagnostics: Reading and Clearing DTCs**

The ECU stores DTCs in NVRAM automatically when faults occur. Use the following commands (ECU must be running):

**Read all active DTCs:**
```bash
./doip_client --read-dtcs
```
Output example:
```
[CLIENT] 2 DTC(s) found:
  DTC[0]: Code=0x000001  Status=0x09  [TEST_FAILED CONFIRMED ]
  DTC[1]: Code=0x000030  Status=0x09  [TEST_FAILED CONFIRMED ]
```

**DTC Code Reference:**

| Code     | Meaning                  | When Set                              |
|----------|--------------------------|---------------------------------------|
| 0x000001 | SECURE_BOOT_FAILURE      | Hash mismatch or NVRAM missing hash   |
| 0x000002 | NVRAM_LOAD_FAILURE       | NVRAM file unreadable                 |
| 0x000010 | OTA_HASH_MISMATCH        | Firmware hash/signature check failed  |
| 0x000011 | OTA_FILE_WRITE_ERROR     | Cannot open update.bin                |
| 0x000020 | INVALID_UDS_SEQUENCE     | UDS command received out of state     |
| 0x000030 | ENGINE_OVERTEMP          | Simulated temp ≥ 110°C                |
| 0x000031 | FAN_CONTROL_FAULT        | Temp ≥ 100°C but fan did not activate |

**Clear all DTCs:**
```bash
./doip_client --clear-dtcs
```

---

### **3.6. Reading Live ECU Data (RDBI)**

UDS `$22` ReadDataByIdentifier lets you query the ECU's live sensor state. The ECU must be in `APPLICATION` state.

```bash
./doip_client --read-data F400    # Engine temperature in °C
./doip_client --read-data F401    # Fan status (ON/OFF)
./doip_client --read-data F189    # Firmware version string
./doip_client --read-data F18C    # ECU serial number
```

Output example:
```
[CLIENT] ENGINE_TEMP = 87 °C
[CLIENT] FAN_STATUS = OFF
```

**Sensor model behaviour:**
- Temperature rises 1°C/tick (2s) when fan is off, falls 2°C/tick when fan is on.
- Fan ON threshold: ≥ 90°C. Fan OFF threshold: ≤ 70°C (hysteresis prevents chatter).
- `ENGINE_OVERTEMP` DTC set at ≥ 110°C.
- `FAN_CONTROL_FAULT` DTC set if temp ≥ 100°C and fan is still off.

---

### **3.7. Secure OTA with ECDSA Firmware Signing**

This replaces hash-only integrity checks with **authenticity verification** — proving the firmware came from the holder of the private key.

#### **Step 1 — Generate key pair (once, offline)**
```bash
cd vECU_project
chmod +x generate_keys.sh
./generate_keys.sh
```
This creates:
- `firmware_signing_key.pem` — **Private key. Keep secret. Never deploy to ECU.**
- `firmware_signing_pub.pem` — Public key. Copy to `build/` alongside `TargetECU`.

```bash
cp firmware_signing_pub.pem ../build/
```

#### **Step 2 — Sign the firmware (offline, with private key)**
```bash
cd build
openssl dgst -sha256 \
    -sign ../vECU_project/firmware_signing_key.pem \
    -out TargetECU_v2.sig \
    TargetECU_v2.bin
```

#### **Step 3 — Flash with signature verification**
```bash
./doip_client --program
./doip_client --update TargetECU_v2.bin --sig TargetECU_v2.sig
```

The ECU loads `firmware_signing_pub.pem`, verifies the ECDSA P-256 signature over the SHA-256 digest of `update.bin`, and only applies the firmware if the signature is valid.

**Why ECDSA over hash-only?**
SHA-256 alone proves the file was not corrupted in transit, but anyone who can intercept the channel can compute a valid hash for a malicious binary. ECDSA proves the binary was signed by the holder of the private key — even if an attacker fully controls the network channel.

---

### **3.8. Alternative Setup: Simulating a Virtual CAN Bus**

While this project uses direct TCP/IP sockets, an alternative on Linux uses a virtual CAN (`vcan`) interface.

```bash
sudo apt install can-utils
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

Test:
- Terminal 1: `candump vcan0`
- Terminal 2: `cangen vcan0`

This allows tools like `python-can` to interact with `vcan0` as if it were real hardware.

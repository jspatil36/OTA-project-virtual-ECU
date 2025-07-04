# Virtual Automotive ECU with Secure OTA Updates with verification

# **Virtual Automotive ECU with Secure OTA Updates: Project Report and Manual**

**Version:** 1.0
**Date:** July 4, 2025

---
### **Table of Contents**
1.  [**Project Report**](#1-project-report)
    - [Introduction](#11-introduction)
    - [System Architecture](#12-system-architecture)
    - [Core Features](#13-core-features)
2.  [**Project Development History**](#2-project-development-history)
    - [Phase 1: Basic Communication & Control](#21-phase-1-basic-communication--control)
    - [Phase 2: Protocol Implementation](#22-phase-2-protocol-implementation)
    - [Phase 3: Secure Boot](#23-phase-3-secure-boot)
    - [Phase 4: Over-the-Air (OTA) Update](#24-phase-4-over-the-air-ota-update)
3.  [**User & Developer Manual**](#3-user--developer-manual)
    - [Prerequisites](#31-prerequisites)
    - [Project File Structure](#32-project-file-structure)
    - [Compilation](#33-compilation)
    - [Full Usage Walkthrough: Performing an OTA Update](#34-full-usage-walkthrough-performing-an-ota-update)
    - [Alternative Setup: Simulating a Virtual CAN Bus](#35-alternative-setup-simulating-a-virtual-can-bus)

---
## **1. Project Report**

### **1.1. Introduction**
This document details a C++ project that simulates an automotive Electronic Control Unit (ECU) with advanced features. The primary goal was to create a realistic, command-line-based simulation of an ECU capable of **Secure Boot** and receiving Over-the-Air (**OTA**) firmware updates. The project utilizes a client-server architecture to emulate the interaction between a diagnostic/flashing tool and an ECU within a vehicle network.

### **1.2. System Architecture**
The simulation is comprised of two main executables and several supporting modules, operating on a client-server model over TCP/IP.

* **`TargetECU` (The Server):** This is the main ECU simulation. It runs an asynchronous TCP server that listens for incoming connections on port 13400, emulating the DoIP (Diagnostics over IP) protocol. It is multi-threaded, with the main application logic and state machine running on the main thread, while all network operations are handled by a dedicated Boost.Asio thread.
* **`doip_client` (The Client):** A command-line tool that acts as a diagnostic tester or flashing tool. It connects to the `TargetECU` and sends structured UDS (Unified Diagnostic Services) messages to request information or initiate programming sequences.
* **State Machine:** The `TargetECU` is governed by a state machine (`EcuState`) that dictates its behavior. The primary states are:
    * **`BOOT`**: The initial state where firmware integrity is checked.
    * **`APPLICATION`**: Normal operating mode.
    * **`UPDATE_PENDING`**: A special mode entered after a command, where the ECU is ready to receive a new firmware file.
    * **`BRICKED`**: A terminal state entered if a critical error (like a failed integrity check) occurs.

### **1.3. Core Features**

* **Secure Boot:** At startup, the `TargetECU` performs a SHA-256 hash of its own executable file. It then compares this calculated hash against a trusted "golden" hash stored in its non-volatile memory (`nvram.dat`). If the hashes do not match, the ECU considers itself tampered with and enters a `BRICKED` state, ceasing operation.
* **Persistent Storage (NVRAM):** A `NVRAMManager` class provides a simple key-value store that persists to a file (`nvram.dat`). This simulates the non-volatile memory of an ECU, used here to store the golden firmware hash and version information.
* **Asynchronous DoIP/UDS Communication:** The project uses a simplified UDS-over-DoIP protocol for all communication.
    * The `DoIPServer` class accepts incoming TCP connections.
    * Each connection is handed off to a `DoIPSession` object, which manages the entire request-response lifecycle for that client.
    * The communication is fully asynchronous, allowing the ECU's main logic to continue running without being blocked by network I/O.
* **Over-the-Air (OTA) Update Mechanism:** The project implements a complete, multi-stage OTA update flow:
    1.  **Initiation:** The client sends a `Routine Control ($31)` command to switch the ECU into `UPDATE_PENDING` mode.
    2.  **Handshake:** The client sends a `Request Download ($34)` command containing the size of the new firmware file. The server prepares for the transfer by opening a temporary file (`update.bin`).
    3.  **Data Transfer:** The client reads the new firmware file in 4KB chunks and sends each one using a `Transfer Data ($36)` command. The server receives these chunks and writes them sequentially to `update.bin`.
    4.  **Verification:** After the transfer is complete, the client sends a `Request Transfer Exit ($37)` command. The payload of this message contains the SHA-256 hash of the new firmware. The server closes `update.bin`, calculates its hash, and compares it to the hash received from the client.
    5.  **Application:** If the hashes match, the server atomically replaces its own executable file with `update.bin` using `std::rename` and sets the correct execute permissions using `std::filesystem::permissions`. It then gracefully shuts down, simulating a reboot.

---
## **2. Project Development History**
This section chronicles the iterative development process, including commands used and problems solved.

### **2.1. Phase 1: Basic Communication & Control**
The project began by establishing a basic, runnable server and ensuring it could be controlled.

1.  **Initial Server:** A single-file `main.cpp` was created with a simple synchronous TCP server using Boost.Asio.
2.  **Process Control:** We immediately addressed an issue where the application couldn't be terminated with `Ctrl+C`. Alternative methods were established:
    * `Ctrl+Z` to suspend the process, followed by `kill %1`.
    * Opening a new terminal to use `ps aux | grep` (on macOS/Linux) to find the Process ID (PID) and terminate it with `kill`.
3.  **Connection Testing:** The initial connection was tested using the `netcat` tool: `nc localhost 13400`.
4.  **Signal Handling:** A proper signal handler for `SIGINT` was added in `main.cpp` to allow for graceful shutdown via `Ctrl+C`.

### **2.2. Phase 2: Protocol Implementation**
This phase moved from a raw TCP connection to a structured, asynchronous protocol.

1.  **Asynchronous Refactor:** The server logic was refactored into two classes: `DoIPServer` to accept connections and `DoIPSession` to manage the lifecycle of each connected client.
2.  **DoIP Header:** The `DoIPHeader` C++ struct was defined to represent the 8-byte header for our protocol.
3.  **Client Development:** The `doip_client` executable was created to construct and send valid DoIP messages.
4.  **Compiler Errors Solved:** We resolved several compilation errors:
    * `use of undeclared identifier 'tcp'`: Fixed by adding `using boost::asio::ip::tcp;`.
    * `use of undeclared identifier 'htons'`: Fixed by adding `#include <arpa/inet.h>`.
    * `redefinition of 'EcuState'`: Solved by creating a central `ecu_state.hpp` header.
5.  **Request-Response:** Logic was completed for the server to reply to a "Vehicle ID Request" and for the client to validate the response.

### **2.3. Phase 3: Secure Boot**
This phase implemented a critical security feature.

1.  **Hashing Function:** A C++ function, `calculate_file_hash`, was added using the OpenSSL library to compute the SHA-256 hash of a file.
2.  **Golden Hash Generation:** The workflow was established to generate a trusted hash of the `TargetECU` executable using `shasum -a 256 TargetECU`.
3.  **NVRAM Integration:** This golden hash was stored in `nvram.dat`.
4.  **Boot Sequence Integration:** The `run_boot_sequence` was modified to perform the integrity check at startup. A mismatch results in the ECU entering the `BRICKED` state.

### **2.4. Phase 4: Over-the-Air (OTA) Update**
This was the final and most complex phase, implementing the full firmware update logic.

1.  **Enter Programming Mode:** The `doip_client` was enhanced with a `--program` flag to send a UDS `Routine Control ($31)` message.
2.  **Update Handshake:** A `--update <file>` command was added to initiate a UDS `Request Download ($34)`.
3.  **Data Transfer Loop:** A loop was implemented to send the firmware file in 4KB chunks using UDS `Transfer Data ($36)`.
4.  **Finalization & Verification:** A `Request Transfer Exit ($37)` message was added, containing the new firmware's SHA-256 hash for server-side verification.
5.  **Applying the Update:** Upon successful verification, `apply_update` uses `std::rename` to replace the old executable.
6.  **Permissions Fix:** We solved a final `permission denied` error by using C++17's `<filesystem>` library to set execute permissions on the newly applied firmware.

---
## **3. User & Developer Manual**

### **3.1. Prerequisites**
To compile and run this project, you will need:
* A C++17 compliant compiler (e.g., Clang on macOS, GCC on Linux).
* CMake (version 3.15 or newer).
* The **OpenSSL** library and headers.
* The **Boost** library and headers (specifically the `system` component).

On macOS with Homebrew, these can be installed with: `brew install cmake openssl boost`

### **3.2. Project File Structure**
The project consists of the following files, all located in the root directory:
* **`CMakeLists.txt`**: The build script for CMake.
* **`ecu_state.hpp`**: Defines the `EcuState` enum.
* **`nvram_manager.hpp`**: Defines the class for managing the `nvram.dat` file.
* **`doip_server.hpp`**: Defines the main TCP server class.
* **`doip_session.hpp`**: Defines the class that handles the logic for a single client session.
* **`main.cpp`**: The main entry point for the `TargetECU` server application.
* **`client.cpp`**: The source code for the `doip_client` command-line tool.

### **3.3. Compilation**
From the project's root directory, follow these steps to compile both the ECU server and the client tool:

1.  Create and navigate into a build directory:
    ```bash
    mkdir build && cd build
    ```
2.  Run CMake to configure the project:
    ```bash
    cmake ..
    ```
3.  Run make to compile the code:
    ```bash
    make
    ```
Two executables, `TargetECU` and `doip_client`, will be created in the `build` directory.

### **3.4. Full Usage Walkthrough: Performing an OTA Update**
This walkthrough demonstrates the entire project lifecycle. **All commands are run from the `build` directory.**

#### **1. Initial Setup (Version 1)**
First, establish a trusted "Version 1" of the ECU.

1.  **Compile:** Run `make` to create the initial `TargetECU`.
2.  **Generate Golden Hash:** Calculate the SHA-256 hash of the compiled ECU.
    ```bash
    shasum -a 256 TargetECU
    ```
3.  **Create NVRAM:** Create a file named `nvram.dat` in the `build` directory. Paste the hash from the previous step as the value for `FIRMWARE_HASH_GOLDEN`. The file should look like this (using your actual hash):
    ```
    FIRMWARE_VERSION=1.0.0
    ECU_SERIAL_NUMBER=VECU-2023-001
    FIRMWARE_HASH_GOLDEN=f2d8b5a8e6f16c4f0bde84d56d3a8db209a54e29a6b16d8c4d2d4c0f8e9a1b4c
    ```

#### **2. Create the Update Package (Version 2)**
Next, create a new, modified version of the firmware.

1.  **Modify:** Open `main.cpp` and make a visible change, such as changing the startup message from "V1" to "V2".
2.  **Compile V2:** Run `make` again. The `TargetECU` executable is now Version 2.
3.  **Create Package:** Copy the V2 executable to a new file.
    ```bash
    cp TargetECU TargetECU_v2.bin
    ```
4.  **Revert to V1:** **This is critical.** Revert the change in `main.cpp` and run `make` one last time. This ensures the running `TargetECU` is Version 1.

#### **3. Execute the OTA Update**
You will need two separate terminal windows, both in the `build` directory.

1.  **Terminal 1 (Run Server):**
    ```bash
    ./TargetECU
    ```
2.  **Terminal 2 (Run Client):**
    a. Enter programming mode:
    ```bash
    ./doip_client --program
    ```
    b. Initiate the update with the V2 package:
    ```bash
    ./doip_client --update TargetECU_v2.bin
    ```

#### **4. Verification**
1.  **Observe Shutdown:** Watch Terminal 1. The server will verify the hash, log the successful update, and shut down.
2.  **Check Executable:** The `TargetECU` file has been overwritten with the V2 code.
3.  **Run the New Version:** In Terminal 1, run the ECU again:
    ```bash
    ./TargetECU
    ```
    The V2 startup message will appear, but the **Secure Boot check will fail**. This is **correct** because the hash in `nvram.dat` is still for V1. To make V2 boot, you would update the hash in `nvram.dat`.

### **3.5. Alternative Setup: Simulating a Virtual CAN Bus**
While this project uses direct TCP/IP sockets, an alternative method for simulating an ECU network on Linux involves using a virtual CAN (vcan) interface.

1.  **Install Tools:**
    ```bash
    sudo apt update
    sudo apt install can-utils
    ```
2.  **Create Virtual Bus:**
    ```bash
    sudo modprobe vcan
    sudo ip link add dev vcan0 type vcan
    sudo ip link set up vcan0
    ```
3.  **Test Virtual Bus:**
    * In Terminal 1, listen for traffic: `candump vcan0`
    * In Terminal 2, generate traffic: `cangen vcan0`
This setup allows tools like `python-can` to interact with the `vcan0` interface as if it were real hardware, providing a different but powerful simulation environment.


Next Steps:

## 1. Implement True Cryptographic Security

This is the most significant and realistic upgrade you can make. While your current hash-based check proves integrity, it doesn't prove authenticity.

Firmware Signing (Asymmetric Cryptography): Instead of just comparing SHA-256 hashes, you can use public/private key cryptography (like RSA or ECDSA).

Offline: Use a private key to create a cryptographic signature of the new firmware's hash.

Client: The client sends the firmware file and the signature as the payload.

ECU: The ECU would have the corresponding public key embedded in its code. It calculates the hash of the received firmware and then uses the public key to verify that the signature is valid. This proves the update came from a trusted source (the holder of the private key).

Encrypted Communication (TLS): Your entire DoIP communication channel is currently unencrypted. You can wrap it in Transport Layer Security (TLS) to prevent eavesdropping or man-in-the-middle attacks. Boost.Asio has direct support for this through boost::asio::ssl. You would create an ssl::context, load certificates, and use an ssl::stream<tcp::socket> instead of a plain tcp::socket.

## 2. Add Advanced Diagnostics (DTCs)

A core function of any real ECU is storing and reporting diagnostic information.

Diagnostic Trouble Codes (DTCs): You can implement a system for managing DTCs.

Set a Fault: Have the ECU create and store a DTC in its NVRAM when something goes wrong (e.g., if the Secure Boot check fails or an invalid UDS command is received).

Implement New UDS Services: Add handlers for UDS services like ReadDTCInformation ($19) and ClearDiagnosticInformation ($14).

Update Client: Add commands to your doip_client like --read-dtcs and --clear-dtcs to interact with the ECU's fault memory.

## 3. Simulate a Realistic Control System

Right now, your ECU's "main application logic" is just a print statement. You can make it perform a simulated control task.

Create a Control Loop: Replace the std::cout in your run_application_mode() with a simple control system simulation.

Simulated Sensor: Create a variable that represents a sensor reading, like engine_temperature, and make it change over time (e.g., slowly increase).

Control Logic: Implement a simple control law. For example, if engine_temperature goes above a certain threshold, set a boolean flag like is_fan_on to true.

Read ECU State: Implement the ReadDataByIdentifier ($22) UDS service. This would allow your doip_client to query the ECU for the current value of engine_temperature or the status of is_fan_on, effectively monitoring the ECU's internal state.

These additions would get me from a communication framework to a more complete and industry-relevant ECU simulation.
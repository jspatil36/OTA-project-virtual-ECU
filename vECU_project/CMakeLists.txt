# --- Project Definition ---
# Specify the minimum version of CMake required.
cmake_minimum_required(VERSION 3.15)

# Define the project name, version, and language.
project(VirtualECU VERSION 1.0 LANGUAGES CXX)

# --- Standard and Properties ---
# Set the C++ standard to C++17. This gives us modern features.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies ---
# Find the OpenSSL library, which we will use for SHA-256 hashing.
# It's required, and the build will fail if it's not found.
find_package(OpenSSL REQUIRED)

# Find the Boost library. We will use Boost.Asio for networking (DoIP).
# We specify the 'asio' component and that it's required.
find_package(Boost REQUIRED COMPONENTS asio)

# --- Target Definition ---
# Add our main executable target named "TargetECU".
# It will be built from the "main.cpp" source file.
add_executable(TargetECU main.cpp)

# --- Linking Dependencies ---
# Link the found libraries to our executable.
# This makes the functions from OpenSSL and Boost available in our code.
target_link_libraries(TargetECU
    PRIVATE
    # Link the cryptography library from OpenSSL
    OpenSSL::Crypto
    # Link the Boost libraries we found
    Boost::asio
)

# --- Installation ---
# Optional: Define where to install the executable if someone runs "make install".
install(TARGETS TargetECU DESTINATION bin)

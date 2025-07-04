# --- Project Definition ---
cmake_minimum_required(VERSION 3.15)
project(VirtualECU VERSION 1.0 LANGUAGES CXX)

# --- Standard and Properties ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies ---
find_package(OpenSSL REQUIRED)
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost REQUIRED COMPONENTS system)

# --- Target Definitions ---
add_executable(TargetECU main.cpp)
add_executable(doip_client client.cpp)

# --- Linking Dependencies for the ECU ---
target_include_directories(TargetECU PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(TargetECU
    PRIVATE
    OpenSSL::Crypto
    Boost::system
)

# --- Linking Dependencies for the Client ---
target_include_directories(doip_client PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(doip_client
    PRIVATE
    OpenSSL::Crypto
    Boost::system
)

# --- Installation ---
install(TARGETS TargetECU doip_client DESTINATION bin)

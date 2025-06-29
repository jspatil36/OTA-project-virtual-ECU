# --- Project Definition ---
# Specify the minimum version of CMake required.
cmake_minimum_required(VERSION 3.15)

# Define the project name, version, and language.
project(VirtualECU VERSION 1.0 LANGUAGES CXX)

# --- Standard and Properties ---
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies ---
find_package(OpenSSL REQUIRED)

# Find the Boost library. We will keep this as it finds the necessary paths.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost REQUIRED COMPONENTS system)


# --- Target Definition ---
# It will be built from the "main.cpp" source file.
add_executable(TargetECU main.cpp)


# --- Linking Dependencies (NEW DIRECT STRATEGY) ---

# 1. Manually add the Boost include directory to our target.
#    Since Asio is mostly a header-only library, this is the most critical step.
#    The ${Boost_INCLUDE_DIRS} variable is automatically set by a successful find_package(Boost) call.
target_include_directories(TargetECU
    PRIVATE
    ${Boost_INCLUDE_DIRS}
)

# 2. Link the libraries we actually found.
#    We are ONLY linking the 'system' component, which we know it found successfully.
#    We are NOT linking 'Boost::asio' because that specific target was not created.
#    The Asio code will be pulled in from the headers included above.
target_link_libraries(TargetECU
    PRIVATE
    OpenSSL::Crypto
    Boost::system
)

# --- Installation ---
install(TARGETS TargetECU DESTINATION bin)

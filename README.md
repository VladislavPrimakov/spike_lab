# Spike Lab

## Requirements

To build this project, you need the following tools:

*   **CMake**: Version **3.20** or higher.
*   **Clang**: Version **16+** (Versions 17 or 18 are recommended) for full C++23 support.
*   **libc++**: The LLVM C++ Standard Library (required by the `-stdlib=libc++` flag).
*   **Ninja** (Optional): For faster build times.

## 1. Installing Dependencies (Ubuntu/Debian)

### Step 1: Install Basic Utilities and CMake

```bash
# Update package lists
sudo apt update
sudo apt install -y build-essential git wget software-properties-common gnupg lsb-release

# Install CMake.
sudo apt install -y cmake

# Install Ninja (build system)
sudo apt install -y ninja-build
```

### Step 2: Install Clang 18 and libc++

We will use the official LLVM script to get the latest stable version:

```bash
# Download the installation script
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh

# Install Clang 18
sudo ./llvm.sh 18 all

# Install libc++ (Critical for this project)
sudo apt install -y libc++-18-dev libc++abi-18-dev

# Set Clang-18 as the default compiler
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100
```

## 2. Building the Project

The project is configured to automatically find all `.c` and `.cpp` files and create separate executables for them.

```bash
# 1. Configuration
cmake --preset linux-debug

# 2. Compilation
cmake --build --preset linux-debug

# 3. Compile all targets
ninja
```

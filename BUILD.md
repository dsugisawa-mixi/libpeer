# Building libpeer

This document describes how to build libpeer, including the FAT library that combines all dependencies into a single static library.

## Prerequisites

- CMake 3.16 or higher
- C compiler (GCC or Clang)
- Git
- Bash shell

### Installing dependencies on Ubuntu/Debian

```bash
sudo apt -y install git cmake build-essential
```

### Installing dependencies on macOS

```bash
brew install cmake
```

## Clone the Repository

Clone the repository with all submodules:

```bash
git clone --recursive git@github.com:dsugisawa-mixi/libpeer.git
cd libpeer
```

If you already cloned without `--recursive`, initialize submodules:

```bash
git submodule update --init --recursive
```

## Build Steps

### 1. Configure CMake

```bash
cmake -S . -B build
```

#### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_TESTS` | OFF | Enable building tests |
| `BUILD_SHARED_LIBS` | OFF | Build shared libraries |
| `ADDRESS_SANITIZER` | OFF | Build with AddressSanitizer |
| `MEMORY_SANITIZER` | OFF | Build with MemorySanitizer |
| `THREAD_SANITIZER` | OFF | Build with ThreadSanitizer |
| `UNDEFINED_BEHAVIOR_SANITIZER` | OFF | Build with UndefinedBehaviorSanitizer |


### 2. Build the Project

```bash
cmake --build build
```

This will:
- Build all third-party dependencies (mbedtls, libsrtp, usrsctp, cJSON)
- Build the main `libpeer` library
- Build the example applications

### 3. Build the FAT Library

The FAT library combines all static libraries into a single archive file, making it easier to link your application.

```bash
cmake --build build --target fat_library
```

The FAT library will be created at:

```
build/dist/lib/libpeer_fat.a
```

### Libraries Included in FAT Library

The FAT library (`libpeer_fat.a`) includes:

- `libpeer.a` - Main WebRTC library
- `libusrsctp.a` - SCTP implementation for DataChannel
- `libsrtp2.a` - Secure RTP
- `libmbedtls.a` - TLS library
- `libmbedx509.a` - X.509 certificate handling
- `libmbedcrypto.a` - Cryptographic functions
- `libcjson.a` - JSON parsing

## Using the FAT Library

Link against the FAT library in your project:

```bash
gcc -o myapp myapp.c -Ibuild/dist/include -Lbuild/dist/lib -lpeer_fat -lpthread
```

Or in CMake:

```cmake
add_executable(myapp myapp.c)
target_include_directories(myapp PRIVATE ${LIBPEER_BUILD_DIR}/dist/include)
target_link_libraries(myapp ${LIBPEER_BUILD_DIR}/dist/lib/libpeer_fat.a pthread)
```

## Cross-Compilation

To cross-compile for another platform, specify a CMake toolchain file:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake
cmake --build build
cmake --build build --target fat_library
```

## Build Output Structure

After building, the output directory structure is:

```
build/
├── dist/
│   ├── include/          # Header files
│   │   ├── cjson/
│   │   ├── mbedtls/
│   │   ├── srtp2/
│   │   └── usrsctp/
│   └── lib/
│       ├── libcjson.a
│       ├── libmbedcrypto.a
│       ├── libmbedtls.a
│       ├── libmbedx509.a
│       ├── libpeer_fat.a  # Combined FAT library
│       ├── libsrtp2.a
│       └── libusrsctp.a
├── src/
│   └── libpeer.a         # Main library
└── examples/
    └── generic/
        └── sample        # Example application
```

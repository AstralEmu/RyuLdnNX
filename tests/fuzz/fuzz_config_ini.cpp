/**
 * @file fuzz_config_ini.cpp
 * @brief libFuzzer target for the configuration INI parser
 *
 * The SD card file is user-editable, so the parser must survive any content:
 * the input is written to a temporary file and loaded through load_config(),
 * the same entry point the sysmodule uses at startup.
 *
 * @copyright Copyright (c) 2026 ryu_ldn_nx contributors
 * @license GPL-2.0-or-later
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

#include "config/config.hpp"

/**
 * @brief libFuzzer entry point
 * @param data Fuzzer-provided input
 * @param size Size of the input in bytes
 * @return Always 0 (libFuzzer convention)
 */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    char path[] = "/tmp/fuzz_config_ini_XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0) {
        std::abort();
    }
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n <= 0) {
            ::close(fd);
            ::unlink(path);
            std::abort();
        }
        written += static_cast<size_t>(n);
    }
    ::close(fd);

    ryu_ldn::config::Config config = ryu_ldn::config::get_default_config();
    static_cast<void>(ryu_ldn::config::load_config(path, config));

    ::unlink(path);
    return 0;
}

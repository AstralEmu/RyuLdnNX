/**
 * @file netdb.h
 * @brief Host stand-in giving <netdb.h> the error-code convention of the target
 *
 * On the Switch toolchain (newlib/libnx) the EAI_* codes are small POSITIVE
 * integers, and dns_wrap.cpp relies on that: it reports an internal failure as
 * `-EAI_xxx` and a success as a positive address count. glibc defines them as
 * negative numbers, which would turn `-EAI_AGAIN` into a "success". The host
 * tests therefore see the target's values; everything else is the system
 * header. On the -isystem path of tests/Makefile only (stubs/system).
 */

#pragma once

#include_next <netdb.h>

#undef EAI_AGAIN
#undef EAI_FAIL
#undef EAI_FAMILY
#undef EAI_MEMORY
#undef EAI_NONAME
#undef EAI_OVERFLOW
#undef EAI_SYSTEM

/// Temporary failure in name resolution (target value).
#define EAI_AGAIN 2
/// Non-recoverable resolver failure (target value).
#define EAI_FAIL 4
/// Address family not supported (target value).
#define EAI_FAMILY 5
/// Resolver out of memory (target value).
#define EAI_MEMORY 6
/// Name or service not known (target value).
#define EAI_NONAME 8
/// System error, see errno (target value).
#define EAI_SYSTEM 11
/// Argument buffer overflow (target value).
#define EAI_OVERFLOW 14

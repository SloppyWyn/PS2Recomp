#pragma once

#include "ps2_stubs.h"
#include "ps2_log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "Unimplemented.h"

#include <iostream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <limits>

// Typed stub ABI layer (ps2_stub_abi.h): handlers declared here register as stubs.
#define PS2_STUB_KIND ::ps2_abi::Kind::Stub
#include "ps2_stub_abi.h"

#include "ps2_host_backend.h"
#include "Helpers/Support.h"

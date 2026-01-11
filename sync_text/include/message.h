#pragma once
#include <cstdint>
#include <cstddef>
#include "registry.h"

// Operation types for updates
enum class OpType : uint8_t { Insert = 1, Delete = 2, Replace = 3 };

// Fixed-size message to fit typical POSIX mqueue (default msgsize often 8192)
// Keep it small. ~600 bytes
constexpr std::size_t TEXT_SEG_MAX = 256;

struct UpdateMessage {
  char sender[USER_ID_MAX];
  uint64_t timestamp_ns; // monotonic or wallclock ns
  uint32_t line;
  int32_t col_start;
  int32_t col_end;
  OpType op;
  char old_text[TEXT_SEG_MAX];
  char new_text[TEXT_SEG_MAX];
};

// POSIX queue name helper: "/queue_<user_id>"
// Ensure user_id is safe (no slashes). Caller guarantees.

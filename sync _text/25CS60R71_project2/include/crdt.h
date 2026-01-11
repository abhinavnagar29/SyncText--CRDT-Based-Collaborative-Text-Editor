#pragma once
#include "message.h"
#include <string>
#include <vector>
#include <cstdint>

// Extended update structure for CRDT merge
struct UpdateExt {
  uint64_t ts;        // timestamp
  std::string uid;    // user_id
  uint32_t line;
  int cs, ce;         // col_start, col_end
  OpType op;
  std::string old_text;
  std::string new_text;
};

// CRDT merge functions
bool overlaps(const UpdateExt &a, const UpdateExt &b);
bool newer_wins(const UpdateExt &a, const UpdateExt &b);
std::string apply_update_to_line(const std::string &cur, const UpdateExt &u);
bool do_merge_apply(std::vector<std::string> &lines, 
                    std::vector<UpdateExt> &local_unmerged,
                    std::vector<UpdateExt> &recv_unmerged,
                    const std::string &self_uid);

#include "../include/crdt.h"
#include <algorithm>
#include <map>

// Check if two updates overlap (conflict)
bool overlaps(const UpdateExt &a, const UpdateExt &b) {
  if (a.line != b.line) return false;
  
  // Special case: two inserts at the same position conflict
  if (a.old_text.empty() && b.old_text.empty() && a.cs == b.cs) {
    return true;
  }
  
  // Check column overlap using computed end based on old_text length
  // Two updates overlap if their column ranges intersect
  int a_end = a.cs + static_cast<int>(a.old_text.size());
  int b_end = b.cs + static_cast<int>(b.old_text.size());
  return !(a_end <= b.cs || b_end <= a.cs);
}

// LWW: newer timestamp wins, tie-break by smaller user_id
bool newer_wins(const UpdateExt &a, const UpdateExt &b) {
  if (a.ts != b.ts) return a.ts > b.ts;
  return a.uid < b.uid;
}

// Apply a single update to a line
// For LWW CRDT, we reconstruct the line by applying the update's transformation
std::string apply_update_to_line(const std::string &cur, const UpdateExt &u) {
  // Handle empty line case
  if (cur.empty()) {
    return u.new_text;
  }
  
  // Ensure positions are within bounds
  int start = std::max(0, static_cast<int>(u.cs));
  int end = std::min(static_cast<int>(u.ce), static_cast<int>(cur.length()) - 1);
  
  // Handle invalid range (start > end)
  if (start > end) {
    return cur;
  }
  
  // Apply the update
  std::string result = cur.substr(0, start) + u.new_text;
  if (end >= 0 && static_cast<size_t>(end + 1) < cur.size()) {
    result += cur.substr(end + 1);
  }
  return result;
}

// CRDT merge algorithm with LWW conflict resolution
// Per assignment: detect conflicts (same line + overlapping columns), resolve via LWW,
// then apply ALL surviving updates. Non-conflicting updates commute.
bool do_merge_apply(std::vector<std::string> &lines, 
                    std::vector<UpdateExt> &local_unmerged,
                    std::vector<UpdateExt> &recv_unmerged,
                    const std::string &self_uid) {
  (void)self_uid;
  if (local_unmerged.empty() && recv_unmerged.empty()) return false;

  // Step 1: Combine all updates (local + remote)
  std::vector<UpdateExt> all;
  all.reserve(local_unmerged.size() + recv_unmerged.size());
  all.insert(all.end(), local_unmerged.begin(), local_unmerged.end());
  all.insert(all.end(), recv_unmerged.begin(), recv_unmerged.end());

  // Step 2: Merge chained updates from same user, then resolve conflicts via LWW
  // First, merge chained updates: if update B's old_text == update A's new_text, merge them
  for (size_t i = 0; i < all.size(); ++i) {
    for (size_t j = i + 1; j < all.size(); ++j) {
      // Check if j is a continuation of i (same line, same user, j's old == i's new)
      if (all[i].line == all[j].line && 
          all[i].uid == all[j].uid &&
          all[i].new_text == all[j].old_text &&
          all[i].cs == all[j].cs) {
        // Merge j into i: keep i's old_text, use j's new_text
        all[i].new_text = all[j].new_text;
        all[i].ts = all[j].ts; // Use later timestamp
        // Mark j as merged (will be skipped)
        all[j].old_text = "###MERGED###";
      }
    }
  }
  
  // Now resolve conflicts via LWW using overlaps()
  std::vector<char> alive(all.size(), 1);
  int conflicts_resolved = 0;
  for (size_t i = 0; i < all.size(); ++i) {
    if (!alive[i]) continue;
    if (all[i].old_text == "###MERGED###") { alive[i] = 0; continue; } // Skip merged
    for (size_t j = i + 1; j < all.size(); ++j) {
      if (!alive[j]) continue;
      if (all[j].old_text == "###MERGED###") { alive[j] = 0; continue; } // Skip merged
      if (overlaps(all[i], all[j])) {
        conflicts_resolved++;
        if (newer_wins(all[i], all[j])) {
          alive[j] = 0;
        } else {
          alive[i] = 0;
          break;
        }
      }
    }
  }
  // Step 3: Collect survivors
  std::vector<UpdateExt> winners;
  winners.reserve(all.size());
  for (size_t i = 0; i < all.size(); ++i) {
    if (alive[i]) winners.push_back(all[i]);
  }

  // Step 4: Group by line and sort by timestamp (newest first)
  std::map<uint32_t, std::vector<UpdateExt>> updates_per_line;
  for (const auto &u : winners) updates_per_line[u.line].push_back(u);

  // Step 5: Apply all survivors per line with offset tracking
  for (auto &kv : updates_per_line) {
    uint32_t line_num = kv.first;
    auto &vec = kv.second;
    while (lines.size() <= line_num) lines.emplace_back("");

    // Sort by column (ascending) and timestamp (descending) to apply newer updates last
    std::sort(vec.begin(), vec.end(), [](const UpdateExt &a, const UpdateExt &b) {
      if (a.cs != b.cs) return a.cs < b.cs;
      return a.ts > b.ts; // Newer timestamps come first for same position
    });

    // Apply updates in order, tracking offsets
    std::string cur = lines[line_num];
    int offset = 0;
    
    for (const auto &u : vec) {
      // Adjust position by accumulated offset
      int adjusted_cs = u.cs + offset;
      int adjusted_ce = u.ce + offset;
      
      // Ensure adjusted positions are within bounds
      adjusted_cs = std::max(0, adjusted_cs);
      adjusted_ce = std::min(adjusted_ce, static_cast<int>(cur.size()) - 1);
      
      // Apply the update
      std::string new_line = cur.substr(0, adjusted_cs) + u.new_text;
      if (adjusted_ce >= 0 && static_cast<size_t>(adjusted_ce + 1) < cur.size()) {
        new_line += cur.substr(adjusted_ce + 1);
      }
      
      // Update offset for subsequent operations
      offset += (static_cast<int>(u.new_text.size()) - (adjusted_ce - adjusted_cs + 1));
      cur = std::move(new_line);
    }
    
    lines[line_num] = std::move(cur);
  }

  local_unmerged.clear();
  recv_unmerged.clear();
  return !winners.empty();
}

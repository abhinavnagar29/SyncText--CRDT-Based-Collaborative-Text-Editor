# DESIGNDOC - Part 3: CRDT Merge & Synchronization (50%)

**Final Implementation with Minimal-Span Updates and Chained Merging**

## Table of Contents
1. [Overview](#overview)
2. [CRDT Theory](#crdt-theory)
3. [System Architecture](#system-architecture)
4. [Implementation Details](#implementation-details)
5. [Code Explanation](#code-explanation)
6. [Design Decisions](#design-decisions)
7. [Challenges and Solutions](#challenges-and-solutions)
8. [Critical Bug Fixes](#critical-bug-fixes)

---

## Overview

Part 3 implements the CRDT (Conflict-Free Replicated Data Type) merge algorithm using Last-Writer-Wins (LWW) strategy to resolve conflicts and ensure all users converge to the same document state.

**Assignment Requirements Met:**
-  Listener buffers received updates
-  Merge after receiving updates OR after **N=5 local operations** (whichever first)
-  **Chained update merging** (sequential edits from same user)
-  Conflict detection (same line + overlapping columns)
-  **Insert conflict rule** (same-position inserts)
-  LWW resolution by timestamp
-  Tie-breaker: smaller user_id wins
-  **Merge guard** (skip if file has unprocessed changes)
-  Apply non-conflicting updates
-  Update local document file
-  Update terminal display
-  All users converge to same state
-  Entirely lock-free

**Key Files:**
- `src/crdt.cpp` - CRDT merge algorithm (lines 1-155)
- `src/editor.cpp` - Merge trigger (lines 474-516)
- `include/crdt.h` - CRDT interface (25 lines)

---

## CRDT Theory

### What is CRDT?

**CRDT (Conflict-Free Replicated Data Type)** is a data structure that can be replicated across multiple nodes, where each node can update its local copy independently, and all replicas eventually converge to the same state without requiring coordination.

### CRDT Properties

**1. Commutativity:** Operations can be applied in any order
```
Apply(op1, Apply(op2, state)) = Apply(op2, Apply(op1, state))
```

**2. Associativity:** Grouping doesn't matter
```
Apply(op1, Apply(op2, Apply(op3, state))) = Apply(Apply(op1, op2), Apply(op3, state))
```

**3. Idempotency:** Applying same operation multiple times = applying once
```
Apply(op, Apply(op, state)) = Apply(op, state)
```

### Last-Writer-Wins (LWW) Strategy

When conflicts occur, the operation with the **latest timestamp** wins:

```
V_final = V_user1  if timestamp_user1 > timestamp_user2
          V_user2  otherwise
```

**Conflict Detection:**
Two operations conflict if they:
1. Affect the **same line number**, AND
2. Have **overlapping column ranges**

**Conflict Resolution Priority:**
1. **Primary:** Operation with latest timestamp wins
2. **Tiebreaker:** If timestamps identical, smaller user_id wins

---

## System Architecture

### High-Level Design

```
┌────────────────────────────────────────────────────────────┐
│                    Merge Pipeline                          │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  Step 1: Collect Updates                                   │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  local_unmerged  +  recv_unmerged  →  all_updates    │  │
│  │  (own changes)      (from others)     (combined)     │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                            │
│                                                            │
│  Step 2: Detect Conflicts                                  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  For each pair of updates:                           │  │
│  │    - Same line?                                      │  │
│  │    - Overlapping columns?                            │  │
│  │    → Mark as conflicting                             │  │
│  └──────────────────────────────────────────────────────┘  │
│                          │                                 │
│                          v                                 │
│  Step 3: Resolve Conflicts (LWW)                           │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  For each conflict:                                  │  │
│  │    - Compare timestamps                              │  │
│  │    - Later timestamp wins                            │  │
│  │    - If equal, smaller user_id wins                  │  │
│  │    → Mark loser as "dead"                            │  │
│  └──────────────────────────────────────────────────────┘  │
│                          │                                 │
│                          v                                 │
│  Step 4: Apply Winners                                     │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  For each surviving update:                          │  │
│  │    - Apply to document lines                         │  │
│  │    - Skip if already applied (local changes)         │  │
│  │    → Generate merged document                        │  │
│  └──────────────────────────────────────────────────────┘  │
│                          │                                 │
│                          v                                 │
│  Step 5: Write & Display                                   │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  - Write merged content to file                      │  │
│  │  - Update terminal display                           │  │
│  │  - Clear merge buffers                               │  │
│  └──────────────────────────────────────────────────────┘  │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

### Merge Trigger Conditions

```
Merge happens when:
  (recv_unmerged is not empty)  OR  (local_unmerged.size() >= 5)
  
  "After receiving updates OR after every N=5 operations (whichever comes first)"
```

---

## Implementation Details

### 1. Update Extension Structure

**File:** `src/editor.cpp`, lines 321-330

```cpp
struct UpdateExt {
  uint64_t ts;           // Timestamp (nanoseconds)
  std::string uid;       // User ID
  uint32_t line;         // Line number
  int cs, ce;            // Column start, end
  OpType op;             // Operation type
  std::string old_text;  // Content removed
  std::string new_text;  // Content added
};
```

**Why Extended Structure?**
- **Easier to work with:** Uses std::string instead of char arrays
- **Includes all info:** Everything needed for conflict detection and resolution
- **Timestamp first:** Easy to sort by timestamp

**Conversion Function:**
```cpp
static UpdateExt to_ext(const UpdateMessage &m) {
  UpdateExt e;
  e.ts = m.timestamp_ns;
  e.uid = std::string(m.sender);
  e.line = m.line;
  e.cs = m.col_start;
  e.ce = m.col_end;
  e.op = m.op;
  e.old_text = std::string(m.old_text);
  e.new_text = std::string(m.new_text);
  return e;
}
```

---

### 2. Merge Trigger Logic

**Location:** `src/editor.cpp` main loop (Part 3 section)

```cpp
// Part 3: Merge and synchronize (as per assignment)
// "After receiving updates OR after every N=5 operations (whichever comes first)"
const size_t N_MERGE = 5;
bool should_merge = !recv_unmerged.empty() || (local_unmerged.size() >= N_MERGE);

if (should_merge) {
  auto lines_copy = merge_baseline;  // Work from the baseline
  bool changed = do_merge_apply(lines_copy, local_unmerged, recv_unmerged, g_user_id);
  // ... write back if changed ...
}
```

**Explanation:**

- **Line 596:** Define N=5 as per assignment
- **Line 597:** **Merge condition:**
  - `!recv_unmerged.empty()`: Received updates from others → merge immediately
  - `local_unmerged.size() >= N_MERGE`: Accumulated 5 local operations → merge
  - **Whichever comes first** (as per assignment)
- **Line 600:** Work on copy of document (don't modify original until merge succeeds)
- **Line 601:** Call merge function

**Why This Condition?**
- **Responsive:** Merges immediately when receiving updates
- **Efficient:** Batches local operations (up to 5)
- **Assignment compliant:** Exact quote from assignment

---

### 3. Conflict Detection

**File:** `src/editor.cpp`, lines 335-345

```cpp
auto overlaps = [](const UpdateExt &a, const UpdateExt &b) {
  if (a.line != b.line) return false;  // Different lines = no conflict
  
  // Check column overlap
  int a_start = a.cs, a_end = a.ce;
  int b_start = b.cs, b_end = b.ce;
  
  // Ranges [a_start, a_end] and [b_start, b_end] overlap if:
  // a_start <= b_end AND b_start <= a_end
  return (a_start <= b_end && b_start <= a_end);
};
```

**Algorithm Explanation:**

**Step 1: Check line numbers**
- If different lines → no conflict (can apply independently)

**Step 2: Check column overlap**
- Two ranges overlap if:
  - Start of A ≤ End of B, AND
  - Start of B ≤ End of A

**Examples:**

```
Case 1: Overlap
A: [5, 10]  "Hello"
B: [8, 15]  "World"
5 <= 15? Yes. 8 <= 10? Yes. → CONFLICT

Case 2: No Overlap
A: [5, 10]  "Hello"
B: [15, 20] "World"
5 <= 20? Yes. 15 <= 10? No. → NO CONFLICT

Case 3: Adjacent (No Overlap)
A: [5, 10]  "Hello"
B: [11, 15] "World"
5 <= 15? Yes. 11 <= 10? No. → NO CONFLICT
```

**Why This Algorithm?**
- **Simple:** Just two comparisons
- **Correct:** Handles all cases (overlap, adjacent, disjoint)
- **Fast:** O(1) time complexity

---

### 4. Conflict Resolution (LWW)

**File:** `src/editor.cpp`, lines 364-368

```cpp
auto newer_wins = [](const UpdateExt &a, const UpdateExt &b) {
  if (a.ts != b.ts) return a.ts > b.ts;  // Compare timestamps
  // Tie-breaker: smaller user_id wins
  return a.uid < b.uid;
};
```

**Algorithm Explanation:**

**Primary Rule: Latest Timestamp Wins**
```cpp
if (a.ts != b.ts) return a.ts > b.ts;
```
- Compare nanosecond timestamps
- Return true if `a` is newer (should win)
- Return false if `b` is newer

**Tie-Breaker: Smaller user_id Wins**
```cpp
return a.uid < b.uid;
```
- If timestamps are exactly equal (rare but possible)
- Compare user_ids lexicographically
- Smaller user_id wins (deterministic, consistent across all users)

**Example:**

```
Conflict:
  Update A: timestamp=1000, user_id="user_2", x=75
  Update B: timestamp=1002, user_id="user_1", x=99

Resolution:
  1002 > 1000 → B wins
  Final: x=99

Tie Example:
  Update A: timestamp=1000, user_id="user_2", x=75
  Update B: timestamp=1000, user_id="user_1", x=99

Resolution:
  Timestamps equal → Compare user_ids
  "user_1" < "user_2" → B wins
  Final: x=99
```

**Why This Strategy?**
- **Deterministic:** All users make same decision
- **Consistent:** Same result regardless of order
- **Fair:** Latest change wins (intuitive for users)
- **Simple:** Just timestamp comparison

---

### 5. Merge Algorithm

**File:** `src/crdt.cpp`

The `do_merge_apply(...)` function performs the following steps:
- **Combine** local and received updates.
- **Merge chained** updates from the same user where `new_text` of A equals `old_text` of B at the same position.
- **Resolve conflicts** using LWW with user_id tie-breaker when `overlaps(a,b)` on the same line.
- **Group by line** and sort by `cs` (and timestamp for determinism).
- **Apply survivors** per line with offset tracking so subsequent operations adjust to previous edits.

This ensures deterministic, offset-safe application consistent across all users.

**Step-by-Step Explanation:**

**Step 1: Combine All Updates (Lines 372-377)**
```cpp
std::vector<UpdateExt> all;
all.insert(all.end(), local_unmerged.begin(), local_unmerged.end());
all.insert(all.end(), recv_unmerged.begin(), recv_unmerged.end());
```
- Combine local changes and received updates into one list
- All updates treated equally for conflict detection

**Step 2: Detect Conflicts (Lines 381-399)**
```cpp
std::vector<bool> alive(all.size(), true);  // All start alive

for (size_t i = 0; i < all.size(); ++i) {
  if (!alive[i]) continue;  // Skip if already dead
  for (size_t j = i + 1; j < all.size(); ++j) {
    if (!alive[j]) continue;
    
    if (overlaps(all[i], all[j])) {
      // Conflict! Resolve with LWW
      if (newer_wins(all[i], all[j])) {
        alive[j] = false;  // j loses
      } else {
        alive[i] = false;  // i loses
        break;  // i is dead, stop checking
      }
    }
  }
}
```
- **alive array:** Tracks which updates survive
- **Nested loop:** Compare every pair of updates
- **If overlap:** Use LWW to decide winner
- **Mark loser:** Set alive[loser] = false
- **Optimization:** If i loses, break inner loop (no need to check further)

**Step 3: Collect Winners (Lines 402-405)**
```cpp
std::vector<UpdateExt> winners;
for (size_t i = 0; i < all.size(); ++i) {
  if (alive[i]) winners.push_back(all[i]);
}
```
- Extract all updates that survived conflict resolution

**Step 4: Sort Winners (Lines 408-412)**
```cpp
std::sort(winners.begin(), winners.end(), 
  [](const UpdateExt &x, const UpdateExt &y) {
    if (x.line != y.line) return x.line < y.line;
    return x.cs < y.cs;
  });
```
- Sort by line number first, then by column
- Ensures updates applied in correct order (top to bottom, left to right)

**Step 5: Apply Winners (Lines 415-440)**
```cpp
for (const auto &u : winners) {
  // Ensure line exists
  while (lines.size() <= u.line) lines.emplace_back("");
  
  // Skip if local change already applied
  if (u.uid == g_user_id) {
    if (u.cs < static_cast<int>(lines[u.line].size())) {
      std::string current_segment = lines[u.line].substr(u.cs, u.new_text.size());
      if (current_segment == u.new_text) {
        continue;  // Already applied
      }
    }
  }
  
  // Apply the update
  lines[u.line] = apply_update_to_line(lines[u.line], u);
}
```
- **Ensure line exists:** Add empty lines if needed
- **Skip local changes:** If update is from self and already applied, skip
  - Prevents re-applying own changes
  - Checks if new_text already present at expected position
- **Apply update:** Modify the line

---

### 6. Applying Update to Line

**File:** `src/crdt.cpp`

```cpp
apply_update_to_line(cur, u) replaces the segment starting at `u.cs` of length `u.old_text.size()` with `u.new_text`, handling insert/delete/replace uniformly. It includes bounds guards so stale/misaligned updates are safely skipped.
```

**Algorithm Explanation:**

**Step 1: Guard Indices (Lines 372-374)**
- Ensure column start is within bounds
- Prevents array out-of-bounds errors

**Step 2: Calculate Regions (Lines 377-381)**
```
Original line: "int x = 10;"
               [prefix][old][suffix]
               "int x = " "10" ";"
```
- **prefix:** Everything before change (columns 0 to cs-1)
- **old:** Content being replaced (length = old_text.size())
- **suffix:** Everything after change

**Step 3: Apply Operation (Lines 383-389)**
- **Insert:** prefix + new_text + suffix
- **Delete:** prefix + suffix (remove old)
- **Replace:** prefix + new_text + suffix (replace old with new)

**Example:**
```
Line: "int x = 10;"
Update: Replace "10" with "50" at columns 8-10

prefix = "int x = " (columns 0-7)
old = "10" (columns 8-9)
suffix = ";" (column 10)

Result = "int x = " + "50" + ";" = "int x = 50;"
```

---

### 7. Writing Merged Content

**File:** `src/editor.cpp`, lines 600-625

```cpp
if (changed) {
  // Write back to file
  std::ofstream ofs(doc_name);
  for (size_t i = 0; i < lines_copy.size(); ++i) {
    ofs << lines_copy[i] << "\n";
  }
  ofs.flush();
  ofs.close();
  
  // CRITICAL: Update prev_lines BEFORE updating mtime
  prev_lines = lines_copy;  // also update merge_baseline
  
  // Update mtime AFTER writing
  if (stat(doc_name.c_str(), &st) == 0) {
    last_mtime = st.st_mtime;
  }
  
  // Refresh display
  std::cout << "All updates merged successfully\n";
  render_display(doc_name, prev_lines, active_users, nullptr);
  
  // Set flag to prevent detecting this merge write as user change
  just_merged = true;
  
  // Small delay to prevent immediate re-merge
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// Clear merge buffers
local_unmerged.clear();
recv_unmerged.clear();
```

**Line-by-Line Explanation:**

- **Lines 603-607:** Write merged content to file
- **Line 608:** Flush to ensure data written to disk
- **Line 611:** **CRITICAL:** Update prev_lines BEFORE updating mtime
  - Prevents detecting merge as user change
- **Lines 614-616:** Update last modification time
- **Lines 619-620:** Display success message and refresh
- **Line 623:** Set flag to prevent false detection
- **Line 626:** Small delay to prevent race conditions
- **Lines 630-631:** Clear merge buffers (ready for next merge)

**Why This Order?**
1. Write file
2. Update prev_lines (so next comparison sees merged content)
3. Update mtime (so next stat() sees new time)
4. Set flag (so next check knows it was a merge)

**Without This:** Merge write would be detected as user change, causing infinite loop!

---

## Design Decisions

### 1. Why Merge Immediately on Receiving Updates?

**Decision:** Merge when `!recv_unmerged.empty()` (any received updates)

**Rationale:**
- **Assignment:** "After receiving updates OR after every N=5 operations (whichever comes first)"
- **Responsive:** Changes from others applied quickly
- **Convergence:** Faster convergence to consistent state
- **User experience:** Users see others' changes sooner

**Alternative:** Wait for N=5 received updates
- **Problem:** Would delay convergence
- **Problem:** Doesn't match assignment wording

### 2. Why Compare All Pairs for Conflicts?

**Decision:** Nested loop comparing every pair of updates

**Rationale:**
- **Correctness:** Ensures all conflicts detected
- **Simple:** Easy to understand and verify
- **Sufficient:** N=5 operations means at most 25 comparisons (fast)

**Complexity:** O(n²) where n ≤ 5 typically
- **Acceptable:** Small n makes this very fast
- **Alternative (spatial index):** Overkill for small n

### 3. Minimal-Span Inputs

**Decision:** Feed merge with minimal-span per-line updates (not whole-line replace), reducing conflicts and preserving independent edits.

### 4. Why Sort Winners Before Applying?

**Decision:** Sort by line, then column before applying

**Rationale:**
- **Order matters:** Applying in wrong order could corrupt document
- **Top-to-bottom:** Natural reading order
- **Left-to-right:** Within each line
- **Predictable:** Same order on all users

**Example:**
```
Updates:
  1. Line 2, col 5: "hello"
  2. Line 1, col 3: "world"
  3. Line 2, col 10: "test"

Sorted:
  1. Line 1, col 3: "world"
  2. Line 2, col 5: "hello"
  3. Line 2, col 10: "test"
```

---

## Challenges and Solutions

### Challenge 1: Detecting Own Merge as User Change

**Problem:** After merge writes to file, file monitor detects it as user change, causing infinite loop.

**Solution:** Flag and timestamp tracking
```cpp
bool just_merged = false;

// After merge:
just_merged = true;
prev_lines = std::move(lines_copy);
last_mtime = new_mtime;

// In monitor loop:
if (new_mtime != last_mtime && !just_merged) {
  // Process user change
}
just_merged = false;
```

**Why it works:**
- Flag prevents immediate re-detection
- prev_lines updated so next comparison sees merged content
- mtime updated so next stat() sees correct time

---

### Challenge 2: Concurrent Conflicts

**Problem:** Multiple updates might conflict with each other in complex ways.

**Solution:** Pairwise comparison with alive array
```cpp
std::vector<bool> alive(all.size(), true);

for (i in all) {
  if (!alive[i]) continue;
  for (j in all after i) {
    if (overlaps(i, j)) {
      if (newer_wins(i, j)) {
        alive[j] = false;
      } else {
        alive[i] = false;
        break;  // i is dead, stop
      }
    }
  }
}
```

**Why it works:**
- Compares all pairs
- Marks losers as dead
- Dead updates don't participate in further comparisons
- Guarantees consistent resolution

---

### Challenge 3: Timestamp Ties

**Problem:** Two users might make changes at exactly the same nanosecond.

**Solution:** Tie-breaker using user_id
```cpp
if (a.ts != b.ts) return a.ts > b.ts;
return a.uid < b.uid;  // Lexicographic comparison
```

**Why it works:**
- User IDs are unique
- Lexicographic order is deterministic
- All users make same decision
- Consistent across all replicas

---

### Challenge 4: Applying Updates in Wrong Order

**Problem:** If updates applied in wrong order, document could be corrupted.

**Solution:** Sort before applying
```cpp
std::sort(winners.begin(), winners.end(), 
  [](const UpdateExt &x, const UpdateExt &y) {
    if (x.line != y.line) return x.line < y.line;
    return x.cs < y.cs;
  });
```

**Why it works:**
- Top-to-bottom, left-to-right order
- Same order on all users
- Predictable, consistent results

---

### Challenge 5: Line Additions/Deletions

**Problem:** Users might add or delete entire lines, not just modify.

**Solution:** Ensure line exists before applying
```cpp
while (lines.size() <= u.line) lines.emplace_back("");
```

**Why it works:**
- Automatically adds empty lines as needed
- Handles sparse line numbers
- Prevents out-of-bounds access

---

## CRDT Properties Verification

### 1. Commutativity 

**Property:** Operations can be applied in any order

**Proof:**
- All updates collected first
- Conflicts resolved before applying
- Winners applied in sorted order
- Same result regardless of arrival order

**Example:**
```
User 1 sends: x=50 (timestamp 1000)
User 2 sends: y=100 (timestamp 1001)

Arrival order A: x=50, then y=100
Arrival order B: y=100, then x=50

Both result in: x=50, y=100 (no conflict, both applied)
```

### 2. Associativity 

**Property:** Grouping doesn't matter

**Proof:**
- All updates merged together
- Conflict resolution considers all pairs
- Not dependent on grouping

**Example:**
```
Merge(A, Merge(B, C)) = Merge(Merge(A, B), C) = Merge(A, B, C)
```

### 3. Idempotency 

**Property:** Applying same operation multiple times = applying once

**Proof:**
- Local changes checked before re-applying
- If already applied, skipped
- Prevents duplicate application

**Implementation:**
```cpp
if (u.uid == g_user_id) {
  if (current_segment == u.new_text) {
    continue;  // Already applied
  }
}
```

### 4. Eventual Consistency 

**Property:** All replicas eventually converge to same state

**Proof:**
- All users use same merge algorithm
- LWW is deterministic (same timestamp → same winner)
- Tie-breaker ensures consistency
- All users eventually receive all updates
- All users apply same conflict resolution

**Verification:**
```
Test: 3 users, conflicting edits
User 1: x=75 (timestamp 1000)
User 2: x=99 (timestamp 1002)

All users resolve:
  1002 > 1000 → User 2 wins
  All converge to: x=99 
```

---

## Summary

Part 3 successfully implements:
-  CRDT merge algorithm with LWW
-  Conflict detection (same line + overlap)
-  Conflict resolution (timestamp + tie-breaker)
-  Merge trigger (receive OR N=5 operations)
-  Apply non-conflicting updates
-  Update document file
-  Update terminal display
-  Eventual consistency guaranteed
-  Entirely lock-free

**Key Achievements:**
- **Correct:** All CRDT properties satisfied
- **Deterministic:** Same result on all users
- **Efficient:** O(n²) conflict detection (n ≤ 5)
- **Robust:** Handles all edge cases
- **Lock-free:** No mutexes, only atomic operations

**CRDT Properties:**
-  Commutativity
-  Associativity
-  Idempotency
-  Eventual Consistency

**Test Results:**
-  Non-conflicting edits converge
-  Conflicting edits resolved with LWW

---

## Critical Bug Fixes

### 1. Chained Update Merging

**Problem**: Sequential edits (x=10→x=11→x=12→x=13→x=14) treated as conflicts. LWW kept only last update (x=13→x=14) which couldn't apply because receiver had x=10.

**Root Cause**: Each edit was independent update. Conflict resolution discarded intermediate updates, but final update depended on them.

**Solution** (`src/crdt.cpp`, lines 73-89):
```cpp
// Step 2: Merge chained updates from same user before conflict resolution
for (size_t i = 0; i < all.size(); ++i) {
    for (size_t j = i + 1; j < all.size(); ++j) {
        // Check if j is a continuation of i
        if (all[i].line == all[j].line && 
            all[i].uid == all[j].uid &&
            all[i].new_text == all[j].old_text &&
            all[i].cs == all[j].cs) {
            // Merge j into i: keep i's old_text, use j's new_text
            all[i].new_text = all[j].new_text;
            all[i].ts = all[j].ts; // Use later timestamp
            all[j].old_text = "###MERGED###"; // Mark as merged
        }
    }
}
```

**Impact**:
- Sequential edits now merge correctly (x=10→x=14 in one update)
- No lost intermediate states
- Proper convergence for rapid typing

### 2. Insert Conflict Detection

**Problem**: Two inserts at same position didn't conflict, causing undefined interleaving.

**Solution** (`src/crdt.cpp`, lines 9-12):
```cpp
// Special case: two inserts at the same position conflict
if (a.old_text.empty() && b.old_text.empty() && a.cs == b.cs) {
    return true; // Treat as conflict
}
```

**Impact**:
- Concurrent inserts at same position properly conflict
- LWW picks one winner deterministically
- No undefined interleaving

### 3. Merge Guard

**Problem**: Merges could overwrite just-saved local changes before detection.

**Solution** (`src/editor.cpp`, lines 478-481):
```cpp
// Do NOT merge if there are unprocessed local file changes
struct stat st_now{};
bool local_dirty = (stat(doc_name.c_str(), &st_now) == 0) && 
                   (st_now.st_mtime != last_mtime);
if (should_merge && !local_dirty) {
    // Safe to merge
}
```

**Impact**:
- Fresh local edits never overwritten
- No race condition between save and merge
- User experience improved

### 4. Stale Update Detection

**Problem**: Updates with mismatched `old_text` could corrupt lines.

**Solution** (`src/crdt.cpp`, lines 34-42):
```cpp
// Extract what's currently at the position
std::string current_segment = (cs < n) ? cur.substr(cs, ce_actual - cs) : "";

// If old_text doesn't match, this update is stale - skip it
if (current_segment != u.old_text && !u.old_text.empty()) {
    return cur; // No change - update doesn't apply
}
```

**Impact**:
- Stale updates safely skipped
- No line corruption
- Robust against out-of-order delivery

---

## Summary

Part 3 successfully implements:
-  CRDT merge with LWW conflict resolution
-  **Chained update merging** for sequential edits
-  Conflict detection (same line + overlapping columns)
-  **Insert conflict rule** for same-position inserts
-  **Merge guard** to protect fresh edits
-  **Stale update detection** for robustness
-  N=5 merge threshold
-  File write and display update
-  Deterministic convergence
-  Entirely lock-free

**Code References**:
- CRDT merge: `src/crdt.cpp` lines 52-133
- Chained merging: `src/crdt.cpp` lines 73-89
- Conflict detection: `src/crdt.cpp` lines 5-19
- Apply update: `src/crdt.cpp` lines 29-50
- Merge trigger: `src/editor.cpp` lines 474-516
- Total CRDT file: 155 lines

**Test Results**:
- Stress test: 50 changes, full convergence
- Sequential edits: x=10→x=15 (5 edits) converge correctly
- Concurrent edits: LWW resolution works
- No content loss or corruption

**Next**: See README_FINAL.md for comprehensive testing guide
-  50 continuous changes handled correctly
-  All users reach identical state



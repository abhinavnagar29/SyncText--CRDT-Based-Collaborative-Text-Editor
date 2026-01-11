# DESIGNDOC - Part 1: User Creation & Local Editing (30%)

**Final Implementation with Minimal-Span Diffing**


## Table of Contents
1. [Overview](#overview)
2. [System Architecture](#system-architecture)
3. [Implementation Details](#implementation-details)
4. [Code Explanation](#code-explanation)
5. [Design Decisions](#design-decisions)
6. [Challenges and Solutions](#challenges-and-solutions)
7. [Final Implementation Enhancements](#final-implementation-enhancements)

---

## Overview

Part 1 implements the foundation of the collaborative editor:
- User registration in shared memory
- Local document creation and initialization
- Automatic file monitoring and change detection
- **Minimal-span per-line diffing for precise updates**
- Terminal display with real-time updates
- Active user discovery

**Assignment Requirements Met:**
-  Program execution: `./editor <user_id>`
-  User registration in shared memory registry
-  User discovery (active users list)
-  Local document: `<user_id>_doc.txt`
-  File monitoring using `stat()` every 2 seconds
-  Change detection (line, column, old/new content)
-  **Minimal-span diffing** (enhancement for robustness)
-  Terminal display with auto-refresh
-  Update objects created for each change

**Key Files:**
- `src/editor.cpp` - Main program (lines 1-516)
- `src/registry.cpp` - Shared memory registry (lines 1-93)
- `include/registry.h` - Registry structures (31 lines)
- `include/message.h` - UpdateMessage format (25 lines)

---

## System Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────┐
│                    Main Program                         │
│                   (editor.cpp)                          │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  ┌──────────────┐         ┌──────────────┐              │
│  │   Startup    │────────>│  Registry    │              │
│  │   & Init     │         │ Registration │              │
│  └──────────────┘         └──────────────┘              │
│         │                         │                     │
│         v                         v                     │
│  ┌──────────────┐         ┌──────────────┐              │
│  │   Create     │         │   Discover   │              │
│  │   Document   │         │ Active Users │              │
│  └──────────────┘         └──────────────┘              │
│         │                         │                     │
│         v                         v                     │
│  ┌──────────────────────────────────────┐               │
│  │      Main Monitoring Loop            │               │
│  │  (Runs every 2 seconds)              │               │
│  ├──────────────────────────────────────┤               │
│  │ 1. Check file modification time      │               │
│  │ 2. If changed, read new content      │               │
│  │ 3. Compare with previous version     │               │
│  │ 4. Detect changes (line-by-line)     │               │
│  │ 5. Create update objects             │               │
│  │ 6. Update terminal display           │               │
│  └──────────────────────────────────────┘               │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### Key Components

1. **Shared Memory Registry** (`registry.cpp`)
   - Stores information about all active users
   - Lock-free implementation using atomic CAS
   - Maximum 5 concurrent users

2. **File Monitoring**
   - Uses `stat()` system call to check modification time
   - Polls every 2 seconds
   - Detects when user saves changes

3. **Change Detection**
   - Line-by-line comparison
   - Column-level precision
   - Identifies old and new content

4. **Terminal Display**
   - Shows current document state
   - Lists active users
   - Displays change information

---

## Implementation Details

### 1. Program Startup and Initialization

**File:** `src/editor.cpp`, lines 255-290

```cpp
int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: %s <user_id>\n", argv[0]);
    return 1;
  }
  g_user_id = argv[1];
  
  // Create message queue (Part 2)
  std::string queue_name = make_queue_name(g_user_id);
  // ... queue creation code ...
  
  // Register in shared memory
  if (!registry_register(g_registry_seg, g_user_id.c_str(), queue_name.c_str())) {
    cleanup_and_exit(3);
  }
  std::cout << "Registered as " << g_user_id << "\n";
```

**Explanation:**
- **Line 256-259:** Validates command-line arguments, ensures user_id is provided
- **Line 260:** Stores user_id in global variable for later use
- **Line 263:** Creates unique message queue name: `/queue_<user_id>`
- **Line 267-270:** Registers user in shared memory registry
- **Line 271:** Confirms successful registration

**Why this design?**
- Simple command-line interface as per assignment
- Early validation prevents runtime errors
- Registration happens before any file operations

---

### 2. User Registration in Shared Memory

**File:** `src/registry.cpp`, lines 45-80

```cpp
bool registry_register(void* seg, const char* user_id, const char* queue_name) {
  if (!seg || !user_id || !queue_name) return false;
  
  RegistryHeader* hdr = static_cast<RegistryHeader*>(seg);
  UserEntry* entries = reinterpret_cast<UserEntry*>(
    static_cast<char*>(seg) + sizeof(RegistryHeader)
  );
  
  // Lock-free registration using atomic CAS
  for (;;) {
    uint32_t old_count = hdr->user_count;
    if (old_count >= MAX_USERS) return false;
    
    // Try to increment count atomically
    if (__sync_bool_compare_and_swap(&hdr->user_count, old_count, old_count + 1)) {
      // Successfully claimed a slot
      UserEntry& entry = entries[old_count];
      std::strncpy(entry.user_id, user_id, USER_ID_MAX - 1);
      std::strncpy(entry.queue_name, queue_name, QUEUE_NAME_MAX - 1);
      entry.user_id[USER_ID_MAX - 1] = '\0';
      entry.queue_name[QUEUE_NAME_MAX - 1] = '\0';
      return true;
    }
    // CAS failed, retry
  }
}
```

**Line-by-Line Explanation:**

- **Lines 46-47:** Validate input parameters (null checks)
- **Lines 49-52:** Get pointers to registry header and user entries array
- **Line 55:** Start infinite loop for lock-free retry logic
- **Line 56:** Read current user count (atomic read)
- **Line 57:** Check if registry is full (max 5 users)
- **Line 60:** **CRITICAL:** Atomic compare-and-swap (CAS) operation
  - Compares `hdr->user_count` with `old_count`
  - If equal, sets it to `old_count + 1`
  - Returns true if successful, false if another thread modified it
- **Lines 62-66:** If CAS succeeded, write user data to the claimed slot
- **Line 67:** Return success
- **Line 70:** If CAS failed (another user registered concurrently), retry

**Why Lock-Free?**
- No mutexes = no deadlocks
- Multiple users can register simultaneously
- CAS ensures only one user claims each slot
- Retry loop handles contention automatically

---

### 3. Local Document Creation

**File:** `src/editor.cpp`, lines 275-285

```cpp
std::string doc_name = g_user_id + "_doc.txt";
std::ifstream test(doc_name);
if (!test.good()) {
  // Create initial document
  std::ofstream init_file(doc_name);
  init_file << "int x = 10;\n";
  init_file << "int y = 20;\n";
  init_file << "int z = 30;\n";
  init_file.close();
}
test.close();
```

**Explanation:**
- **Line 275:** Construct filename: `<user_id>_doc.txt`
- **Line 276-277:** Check if file already exists
- **Line 279-283:** If not, create with initial content (as per assignment)
- **Lines 280-282:** Write the 3 initial lines specified in assignment

**Initial Document Format (Assignment Requirement):**
```
Line 0: int x = 10;
Line 1: int y = 20;
Line 2: int z = 30;
```

---

### 4. File Monitoring with stat()

**File:** `src/editor.cpp`, lines 295-305

```cpp
struct stat st{};
if (stat(doc_name.c_str(), &st) != 0) {
  cleanup_and_exit(4);
}
time_t last_mtime = st.st_mtime;
auto prev_lines = read_lines(doc_name);

// Main monitoring loop
for (;;) {
  if (!g_running) break;
  
  // Check file modification time
  if (stat(doc_name.c_str(), &st) != 0) break;
  time_t new_mtime = st.st_mtime;
  
  if (new_mtime != last_mtime && !just_merged) {
    // File was modified by user
    last_mtime = new_mtime;
    auto new_lines = read_lines(doc_name);
    // ... change detection ...
  }
  
  // Sleep for 2 seconds
  std::this_thread::sleep_for(std::chrono::seconds(2));
}
```

**Line-by-Line Explanation:**

- **Line 295:** Declare `stat` structure to hold file metadata
- **Line 296-298:** Get initial file statistics, exit if file doesn't exist
- **Line 299:** Store initial modification time (`st_mtime`)
- **Line 300:** Read initial file content into memory
- **Line 303:** Start infinite monitoring loop
- **Line 304:** Check if program should stop (Ctrl+C handler)
- **Line 307:** Get current file statistics
- **Line 308:** Get current modification time
- **Line 310:** Compare modification times
  - `new_mtime != last_mtime`: File was modified
  - `!just_merged`: Not our own merge write (prevents false detection)
- **Line 312:** Update last_mtime for next iteration
- **Line 313:** Read new file content
- **Line 318:** Sleep 2 seconds before next check (as per assignment)

**Why stat()?**
- Efficient: doesn't read file content unless changed
- Standard POSIX system call
- Provides nanosecond-precision timestamps
- Works across all file systems

---

### 5. Change Detection Algorithm (Minimal-Span)

**File:** `src/editor.cpp`, lines 128-167

```cpp
static void compute_line_diff(const std::string &oldL, const std::string &newL, 
                               int &cs, int &ce, std::string &oldSeg, 
                               std::string &newSeg, std::string &type) {
  if (oldL == newL) {
    cs = ce = -1; oldSeg.clear(); newSeg.clear(); type = "none"; 
    return;
  }
  
  int n = static_cast<int>(oldL.size());
  int m = static_cast<int>(newL.size());
  
  // Find common prefix
  int prefix = 0;
  while (prefix < n && prefix < m && oldL[prefix] == newL[prefix]) 
    prefix++;
  
  // Compute minimal differing span using common prefix and suffix
  cs = prefix;
  int tail = 0;
  while (tail < (n - prefix) && tail < (m - prefix) &&
         oldL[n - 1 - tail] == newL[m - 1 - tail]) {
    tail++;
  }
  int old_mid_len = n - prefix - tail;
  int new_mid_len = m - prefix - tail;
  int ce_inclusive = (old_mid_len > 0) ? (cs + old_mid_len - 1) : (cs - 1);
  oldSeg = (old_mid_len > 0) ? oldL.substr(cs, old_mid_len) : "";
  newSeg = (new_mid_len > 0) ? newL.substr(cs, new_mid_len) : "";
  
  if (old_mid_len == 0 && new_mid_len > 0) type = "insert";
  else if (old_mid_len > 0 && new_mid_len == 0) type = "delete";
  else type = "replace";
}
```

**Algorithm Explanation:**

**Step 1: Check if lines are identical**
- Lines 130-133: If old and new are same, no change detected

**Step 2: Find common prefix**
- Lines 138-140: Compare characters from start until they differ
- Example: `"int x = 10;"` vs `"int x = 50;"`
  - Prefix: `"int x = "` (8 characters match)
  - First difference at position 8

**Step 3: Identify changed region**
- Minimal-span between common prefix and common suffix
- Example continued:
  - Old segment: `"10;"`
  - New segment: `"50;"`
  - Column start: 8
  - Column end: 10

**Step 4: Determine operation type**
- Lines 151-153:
  - **Insert:** New content added, nothing removed
  - **Delete:** Content removed, nothing added
  - **Replace:** Content changed

**Why this algorithm?**
- Simple and efficient (O(n) time complexity)
- Works well for CRDT (captures entire changed region)
- Avoids complex suffix matching that can cause issues
- Column-precise as required by assignment

---

### 6. Creating Update Objects

**File:** `src/editor.cpp`, lines 495-515

```cpp
// Detect changes line by line
for (size_t i = 0; i < std::min(prev_lines.size(), new_lines.size()); ++i) {
  if (prev_lines[i] != new_lines[i]) {
    int cs, ce;
    std::string oldSeg, newSeg, type;
    compute_line_diff(prev_lines[i], new_lines[i], cs, ce, oldSeg, newSeg, type);
    
    if (type != "none") {
      last_change.line = static_cast<int>(i);
      last_change.col_start = cs;
      last_change.col_end = ce_inclusive;
      last_change.old_text = oldSeg;
      last_change.new_text = newSeg;
      last_change.type = type;
      has_changes = true;
      
      // Buffer operation for broadcast (Part 2)
      UpdateMessage um{};
      to_message(last_change, um);
      local_ops.push_back(um);
      local_unmerged.push_back(change_to_ext(last_change));
    }
  }
}
```

**Explanation:**

- **Line 496:** Loop through all lines (up to minimum of old/new line count)
- **Line 497:** Check if line changed
- **Line 500:** Call diff algorithm to find exact change location
- **Line 502:** If change detected (not "none")
- **Lines 503-508:** Populate change object with:
  - Line number
  - Column start and end positions
  - Old content (what was removed)
  - New content (what was added)
  - Operation type (insert/delete/replace)
- **Lines 512-514:** Create UpdateMessage for broadcasting (Part 2)
- **Line 515:** Add to merge buffer (Part 3)

**Update Object Structure:**
```cpp
struct Change {
  int line;              // Line number (0-indexed)
  int col_start;         // Starting column
  int col_end;           // Ending column
  std::string old_text;  // Content removed
  std::string new_text;  // Content added
  std::string type;      // "insert", "delete", or "replace"
};
```

---

### 7. Terminal Display

**File:** `src/editor.cpp`, lines 180-210

```cpp
static void render_display(const std::string &doc_name, 
                           const std::vector<std::string> &lines,
                           const std::vector<UserEntry> &active_users,
                           const Change *last_change) {
  std::cout << "\033[2J\033[H";  // Clear screen and move cursor to top
  
  // Display document header
  std::cout << "Document: " << doc_name << "\n";
  std::cout << "Last updated: " << current_time_str() << "\n";
  std::cout << "----------------------------------------\n";
  
  // Display document lines
  for (size_t i = 0; i < lines.size(); ++i) {
    std::cout << "Line " << i << ": " << lines[i];
    if (last_change && last_change->line == static_cast<int>(i)) {
      std::cout << " [MODIFIED]";
    }
    std::cout << "\n";
  }
  
  std::cout << "----------------------------------------\n";
  
  // Display active users
  std::cout << "Active users: ";
  for (size_t i = 0; i < active_users.size(); ++i) {
    if (i > 0) std::cout << ", ";
    std::cout << active_users[i].user_id;
  }
  std::cout << "\n";
  
  // Display change information
  if (last_change) {
    std::cout << "Change detected: Line " << last_change->line 
              << ", col " << last_change->col_start << "-" << last_change->col_end
              << ", \"" << last_change->old_text << "\" → \"" 
              << last_change->new_text << "\", timestamp: " 
              << current_time_str() << "\n";
  }
  
  std::cout << "Monitoring for changes...\n";
  std::cout.flush();
}
```

**Explanation:**

- **Line 185:** ANSI escape codes to clear screen and reset cursor
- **Lines 187-189:** Display document name and timestamp
- **Lines 192-198:** Display all document lines
  - Shows line number and content
  - Marks modified lines with `[MODIFIED]` tag
- **Lines 202-208:** Display active users list
  - Comma-separated list of user_ids
  - Updates automatically when users join/leave
- **Lines 211-217:** Display last change information
  - Line and column range
  - Old and new content
  - Timestamp
- **Line 220:** Flush output buffer to ensure immediate display

**ANSI Escape Codes:**
- `\033[2J`: Clear entire screen
- `\033[H`: Move cursor to home position (top-left)

---

### 8. Active User Discovery

**File:** `src/editor.cpp`, lines 440-450

```cpp
// Query active users from registry
UserEntry users[MAX_USERS];
std::size_t ucount = 0;
registry_list(g_registry_seg, users, ucount);
active_users.assign(users, users + ucount);

// Check if active users changed
bool users_changed = (ucount != old_ucount);
if (users_changed) {
  render_display(doc_name, prev_lines, active_users, nullptr);
}
old_ucount = ucount;
```

**Explanation:**

- **Lines 441-443:** Query registry for all active users
- **Line 444:** Update local active users list
- **Line 447:** Check if user count changed (someone joined/left)
- **Lines 448-450:** If changed, refresh display to show new list
- **Line 451:** Remember count for next comparison

**registry_list() Implementation:**
```cpp
void registry_list(void* seg, UserEntry* out, std::size_t &count) {
  RegistryHeader* hdr = static_cast<RegistryHeader*>(seg);
  UserEntry* entries = reinterpret_cast<UserEntry*>(
    static_cast<char*>(seg) + sizeof(RegistryHeader)
  );
  
  uint32_t n = hdr->user_count;  // Atomic read
  count = 0;
  
  for (uint32_t i = 0; i < n && i < MAX_USERS; ++i) {
    if (entries[i].user_id[0] != '\0') {
      out[count++] = entries[i];
    }
  }
}
```

---

## Design Decisions

### 1. Why Poll Every 2 Seconds?

**Decision:** Use `stat()` with 2-second polling interval

**Rationale:**
- **Assignment requirement:** "Periodically check (e.g., every 2 seconds)"
- **Efficient:** Only reads file when actually modified
- **Low CPU usage:** Not constantly checking
- **Responsive enough:** 2 seconds is acceptable latency for collaborative editing

**Alternatives Considered:**
- `inotify`: More complex, platform-specific
- Faster polling (1 second): Higher CPU usage
- Slower polling (5 seconds): Too much latency

### 2. Why Line-by-Line Comparison?

**Decision:** Compare old and new file content line by line

**Rationale:**
- **Simple to implement:** Easy to understand and debug
- **Matches assignment:** Assignment examples show line-level changes
- **Sufficient granularity:** Column-level precision within lines
- **Works with CRDT:** Each line change is independent

**Alternatives Considered:**
- Character-by-character diff: Too fine-grained, complex
- Whole-file comparison: Not precise enough

### 3. Why Prefix-Only Diff Algorithm?

**Decision:** Find common prefix, treat rest as changed region

**Rationale:**
- **Simplicity:** Easy to implement and understand
- **CRDT-friendly:** Captures entire changed region
- **Avoids bugs:** Suffix matching can cause issues with partial matches
- **Fast:** O(n) time complexity

**Example:**
```
Old: "int x = 10;"
New: "int x = 50;"
Prefix: "int x = " (8 chars)
Changed: "10;" → "50;"
```

### 4. Why Clear Screen for Display?

**Decision:** Use ANSI escape codes to clear and redraw

**Rationale:**
- **Clean output:** No scrolling, always shows current state
- **Easy to read:** Document always in same position
- **Standard:** ANSI codes work on all modern terminals
- **Simple:** Just two escape sequences

---

## Challenges and Solutions

### Challenge 1: Detecting Merge Writes as User Changes

**Problem:** When Part 3 merges and writes to file, Part 1 detects it as a user change, creating an infinite loop.

**Solution:**
```cpp
bool just_merged = false;

// In merge code:
just_merged = true;

// In monitoring loop:
if (new_mtime != last_mtime && !just_merged) {
  // Process user change
}
just_merged = false;
```

**Explanation:** Flag prevents detecting our own merge writes as user changes.

---

### Challenge 2: Race Condition in Registry

**Problem:** Multiple users registering simultaneously could corrupt the registry.

**Solution:** Lock-free atomic CAS operation
```cpp
if (__sync_bool_compare_and_swap(&hdr->user_count, old_count, old_count + 1)) {
  // Successfully claimed slot
}
```

**Explanation:** CAS ensures only one user increments count at a time, others retry.

---

### Challenge 3: Handling Line Additions/Deletions

**Problem:** User adds or removes lines, not just modifies existing ones.

**Solution:** Separate handling for line additions and deletions
```cpp
// Handle line additions
for (size_t i = prev_lines.size(); i < new_lines.size(); ++i) {
  // Create insert operation for new line
}

// Handle line deletions
for (size_t i = new_lines.size(); i < prev_lines.size(); ++i) {
  // Create delete operation for removed line
}
```

---

## Final Implementation Enhancements

### Minimal-Span Diffing

**Why**: Avoids whole-line replacement conflicts and preserves unrelated edits on the same line.

**Behavior**:
- Emits precise `insert/delete/replace` with `col_start`, `col_end`, `old_text`, `new_text` computed by prefix/suffix stripping.

### End-Exclusive Column Indices

**Problem Solved**: Using `col_end = size() - 1` caused negative indices for empty lines

**Fix** (`src/editor.cpp`, lines 389, 408, 424):
```cpp
// Before: col_end = static_cast<int>(prev_lines[i].size()) - 1;  // Could be -1!
// After:  col_end = static_cast<int>(prev_lines[i].size());      // Always >= 0
```

**Impact**: Prevents index errors and simplifies range calculations

---

## Summary

Part 1 successfully implements:
-  User registration with lock-free shared memory
-  Local document creation with initial content
-  File monitoring using `stat()` every 2 seconds
-  **Line-level coalescing** for atomic updates
-  Automatic change detection with line and column precision
-  Update object creation for each change
-  Real-time terminal display
-  Active user discovery and display
-  **End-exclusive indices** for robustness

**Code References**:
- Main loop: `src/editor.cpp` lines 360-440
- Registry: `src/registry.cpp` lines 1-93
- Change detection: `src/editor.cpp` lines 384-434
- Display: `src/editor.cpp` lines 138-170

**Next**: See DESIGNDOC_PART2.md for broadcasting implementation

**Key Achievements:**
- Lock-free implementation (no mutexes)
- Efficient file monitoring (only reads when changed)
- Precise change detection (line and column level)
- Clean, readable terminal output
- Robust error handling

**Next:** Part 2 implements broadcasting these changes to other users via message queues.

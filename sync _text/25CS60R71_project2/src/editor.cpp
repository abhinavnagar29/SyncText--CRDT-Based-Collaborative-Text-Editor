#include "../include/registry.h"
#include "../include/message.h"
#include "../include/crdt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mqueue.h>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

static int g_registry_fd = -1;
static RegistrySegment *g_registry_seg = nullptr;
static std::string g_user_id;
static std::string g_queue_name; // e.g., /queue_user_1
static mqd_t g_mq = (mqd_t)-1;
static bool g_running = true;
static std::atomic<uint64_t> g_recv_total{0};
static char g_last_sender[USER_ID_MAX] = {0};
static std::atomic<uint64_t> g_sent_total{0};
static char g_last_target[USER_ID_MAX] = {0};

// Lock-free SPSC ring buffer for received updates (listener -> main)
template <typename T, size_t CAP>
struct RingBuffer {
  std::atomic<size_t> head{0};
  std::atomic<size_t> tail{0};
  T data[CAP];
  bool push(const T &v) {
    size_t h = head.load(std::memory_order_relaxed);
    size_t n = (h + 1) % CAP;
    if (n == tail.load(std::memory_order_acquire)) return false; // full
    data[h] = v;
    head.store(n, std::memory_order_release);
    return true;
  }
  bool pop(T &out) {
    size_t t = tail.load(std::memory_order_relaxed);
    if (t == head.load(std::memory_order_acquire)) return false; // empty
    out = data[t];
    tail.store((t + 1) % CAP, std::memory_order_release);
    return true;
  }
};

static RingBuffer<UpdateMessage, 128> g_recv_buf;

static void cleanup_and_exit(int code) {
  g_running = false;
  if (!g_user_id.empty() && g_registry_seg) {
    registry_unregister(g_registry_seg, g_user_id.c_str());
  }
  if (!g_queue_name.empty()) {
    if (g_mq != (mqd_t)-1) {
      mq_close(g_mq);
      g_mq = (mqd_t)-1;
    }
    mq_unlink(g_queue_name.c_str());
  }
  if (g_registry_seg) {
    munmap(g_registry_seg, sizeof(RegistrySegment));
    g_registry_seg = nullptr;
  }
  if (g_registry_fd >= 0) {
    close(g_registry_fd);
    g_registry_fd = -1;
  }
  std::_Exit(code);
}

static void handle_signal(int) {
  cleanup_and_exit(0);
}

static std::string now_time_str() {
  std::time_t t = std::time(nullptr);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
  return std::string(buf);
}

static void ensure_initial_doc(const std::string &path) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    return; // exists
  }
  std::ofstream ofs(path);
  ofs << "int x = 10;\n";
  ofs << "int y = 20;\n";
  ofs << "int z = 30;\n";
}

static std::vector<std::string> read_lines(const std::string &path) {
  std::ifstream ifs(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(ifs, line)) {
    lines.push_back(line);
  }
  // Normalize: drop trailing empty lines to avoid phantom blank-line diffs
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

struct Change {
  int line;             // line number
  int col_start;        // inclusive
  int col_end;          // inclusive end index in old/new span region
  std::string old_text; // segment replaced
  std::string new_text; // segment inserted
  std::string timestamp;
  std::string user_id;
  std::string type;     // insert/delete/replace
};

// Helper to verify if a user's queue actually exists
static bool queue_exists(const char *queue_name) {
  if (queue_name[0] == '\0') return false;
  mqd_t test = mq_open(queue_name, O_WRONLY | O_NONBLOCK);
  if (test == (mqd_t)-1) return false;
  mq_close(test);
  return true;
}

static void render_display(const std::string &doc_name, const std::vector<std::string> &lines, const std::vector<UserEntry> &active_users, const Change *last_change) {
  // Clear screen
  std::cout << "\033[2J\033[H";
  std::cout << "Document: " << doc_name << "\n";
  std::cout << "Last updated: " << now_time_str() << "\n";
  std::cout << "----------------------------------------\n";
  for (size_t i = 0; i < lines.size(); ++i) {
    std::cout << "Line " << i << ": " << lines[i];
    if (last_change && last_change->line == static_cast<int>(i)) {
      std::cout << " [MODIFIED]";
    }
    std::cout << "\n";
  }
  std::cout << "----------------------------------------\n";
  std::cout << "Active users: ";
  bool first = true;
  // Only show users whose queues actually exist
  for (const auto &u : active_users) {
    if (queue_exists(u.queue_name)) {
      if (!first) std::cout << ", ";
      std::cout << u.user_id;
      first = false;
    }
  }
  if (first) std::cout << "(none)";
  std::cout << "\n";
  if (last_change && last_change->col_start >= 0) {
    std::cout << "Change detected: Line " << last_change->line << ", col "
              << last_change->col_start << "-" << last_change->col_end << ", \""
              << last_change->old_text << "\" \u2192 \""
              << last_change->new_text
              << "\", timestamp: " << last_change->timestamp << "\n";
  }
  // Show received updates from other users
  uint64_t cnt = g_recv_total.load(std::memory_order_relaxed);
  if (cnt > 0 && g_last_sender[0] != '\0') {
    std::cout << "Received update from " << g_last_sender << "\n";
  }
  std::cout << "Monitoring for changes...\n";
  std::cout.flush();
}

static inline uint64_t now_ns() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static void to_message(const Change &c, UpdateMessage &m) {
  std::snprintf(m.sender, USER_ID_MAX, "%s", g_user_id.c_str());
  m.timestamp_ns = now_ns();
  m.line = static_cast<uint32_t>(c.line);
  m.col_start = c.col_start;
  m.col_end = c.col_end;
  if (c.type == "insert") m.op = OpType::Insert;
  else if (c.type == "delete") m.op = OpType::Delete;
  else m.op = OpType::Replace;
  std::snprintf(m.old_text, TEXT_SEG_MAX, "%s", c.old_text.c_str());
  std::snprintf(m.new_text, TEXT_SEG_MAX, "%s", c.new_text.c_str());
}

static std::string make_queue_name(const std::string &uid) {
  return std::string("/queue_") + uid;
}

static void listener_thread_fn() {
  // Query queue attributes to size buffer correctly
  struct mq_attr attr{};
  if (mq_getattr(g_mq, &attr) != 0) {
    attr.mq_msgsize = sizeof(UpdateMessage);
  }
  std::vector<char> buf(static_cast<size_t>(attr.mq_msgsize));
  // Non-blocking receive loop
  while (g_running) {
    ssize_t r = mq_receive(g_mq, buf.data(), buf.size(), nullptr);
    if (r >= 0) {
      UpdateMessage msg{};
      std::memcpy(&msg, buf.data(), std::min(sizeof(UpdateMessage), static_cast<size_t>(r)));
      g_recv_buf.push(msg);
      std::snprintf(g_last_sender, USER_ID_MAX, "%s", msg.sender);
      g_recv_total.fetch_add(1, std::memory_order_relaxed);
    } else {
      if (errno == EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: %s <user_id>\n", argv[0]);
    return 1;
  }
  g_user_id = argv[1];
  g_queue_name = make_queue_name(g_user_id);

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  if (registry_open_or_create(g_registry_fd, g_registry_seg) != 0) {
    std::fprintf(stderr, "Failed to open registry shared memory\n");
    return 2;
  }

  int slot = -1;
  // Create our message queue before registering so we can store the name
  // Use attributes within kernel limits (msg_max=10) and msgsize exactly sizeof(UpdateMessage)
  mq_unlink(g_queue_name.c_str());
  struct mq_attr attr{};
  attr.mq_flags = 0; // flags ignored on create
  attr.mq_maxmsg = 10;
  attr.mq_msgsize = sizeof(UpdateMessage);
  attr.mq_curmsgs = 0;
  g_mq = mq_open(g_queue_name.c_str(), O_CREAT | O_RDONLY | O_NONBLOCK, 0666, &attr);
  if (g_mq == (mqd_t)-1) {
    std::perror("mq_open (self)");
    cleanup_and_exit(2);
  }
  std::printf("Message queue created: %s\n", g_queue_name.c_str());

  if (registry_register(g_registry_seg, g_user_id.c_str(), g_queue_name.c_str(), slot) != 0) {
    std::fprintf(stderr, "Failed to register user (max %zu)\n", MAX_USERS);
    cleanup_and_exit(3);
  }
  std::printf("Registered as %s\n", g_user_id.c_str());

  std::string doc_name = g_user_id + std::string("_doc.txt");
  ensure_initial_doc(doc_name);

  // Load initial content and mtime
  struct stat st{};
  if (stat(doc_name.c_str(), &st) != 0) {
    std::fprintf(stderr, "Cannot stat %s\n", doc_name.c_str());
    cleanup_and_exit(4);
  }
  time_t last_mtime = st.st_mtime;
  auto prev_lines = read_lines(doc_name);

  // Initial display
  UserEntry users[MAX_USERS];
  std::size_t ucount = 0;
  registry_list(g_registry_seg, users, ucount);
  std::vector<UserEntry> active_users(users, users + ucount);
  render_display(doc_name, prev_lines, active_users, nullptr);

  // Start listener thread
  std::thread listener(listener_thread_fn);
  listener.detach();

  // Local op buffer for broadcast
  std::vector<UpdateMessage> local_ops;
  local_ops.reserve(8);

  // Part 3: buffers for merging (UpdateExt defined in crdt.h)
  std::vector<UpdateExt> local_unmerged;
  std::vector<UpdateExt> recv_unmerged;
  std::vector<std::string> merge_baseline = prev_lines; // Baseline for computing deltas
  // Track time of last local operation to support idle-time broadcast flush
  [[maybe_unused]] uint64_t last_local_op_ns = 0;
  auto to_ext = [](const UpdateMessage &m) {
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
  };
  auto change_to_ext = [&](const Change &c) {
    UpdateExt e;
    e.ts = now_ns();
    e.uid = g_user_id;
    e.line = static_cast<uint32_t>(c.line);
    e.cs = c.col_start;
    e.ce = c.col_end;
    if (c.type == "insert") e.op = OpType::Insert; else if (c.type == "delete") e.op = OpType::Delete; else e.op = OpType::Replace;
    e.old_text = c.old_text;
    e.new_text = c.new_text;
    return e;
  };

  // CRDT functions are now in crdt.cpp

  bool just_merged = false;  // Flag to prevent detecting merge writes as user changes
  
  while (true) {
    // Refresh active users every iteration
    size_t old_ucount = ucount;
    ucount = 0;
    registry_list(g_registry_seg, users, ucount);
    active_users.assign(users, users + ucount);
    
    // If active users changed, refresh display
    bool users_changed = (ucount != old_ucount);

    // Defer sleeping to the end of the iteration so we can immediately
    // process any received updates and merges without waiting.

    // Drain received messages into recv_unmerged (filter out self)
    bool got_remote_updates = false;
    for (;;) {
      UpdateMessage tmp;
      if (!g_recv_buf.pop(tmp)) break;
      // Skip messages from self
      if (std::strncmp(tmp.sender, g_user_id.c_str(), USER_ID_MAX) == 0) continue;
      recv_unmerged.push_back(to_ext(tmp));
      got_remote_updates = true;
      // Track last sender
      std::snprintf(g_last_sender, USER_ID_MAX, "%s", tmp.sender);
    }
    
    // Show received updates message and refresh display
    if (got_remote_updates && g_last_sender[0] != '\0') {
      std::cout << "Received update from " << g_last_sender << "\n";
      render_display(doc_name, prev_lines, active_users, nullptr);
    }
    
    // If users changed, refresh display
    if (users_changed && !got_remote_updates) {
      render_display(doc_name, prev_lines, active_users, nullptr);
    }
    if (stat(doc_name.c_str(), &st) != 0) {
      continue;
    }
    
    // If we just merged, simply reset the flag; do not skip processing of received updates
    if (just_merged) {
      just_merged = false;
    }
    
    if (st.st_mtime != last_mtime) {
      last_mtime = st.st_mtime;
      auto new_lines = read_lines(doc_name);

      // Detect changes line-by-line (minimal-span diff per line)
      Change last_change{ -1, -1, -1, "", "", now_time_str(), g_user_id, "none" };
      bool has_changes = false;

      // Compare line by line against prev_lines (last known state)
      size_t common_lines = std::min(prev_lines.size(), new_lines.size());
      for (size_t i = 0; i < common_lines; ++i) {
        const std::string &oldL = prev_lines[i];
        const std::string &newL = new_lines[i];
        if (oldL == newL) continue;

        // Compute minimal differing span: cs (first diff), tail (common suffix)
        int old_len = static_cast<int>(oldL.size());
        int new_len = static_cast<int>(newL.size());
        int cs = 0;
        int max_common_left = std::min(old_len, new_len);
        while (cs < max_common_left && oldL[cs] == newL[cs]) cs++;

        int tail = 0;
        while (tail < (old_len - cs) && tail < (new_len - cs) &&
               oldL[old_len - 1 - tail] == newL[new_len - 1 - tail]) {
          tail++;
        }

        int old_mid_len = old_len - cs - tail;
        int new_mid_len = new_len - cs - tail;
        std::string old_seg = (old_mid_len > 0) ? oldL.substr(cs, old_mid_len) : std::string();
        std::string new_seg = (new_mid_len > 0) ? newL.substr(cs, new_mid_len) : std::string();

        // Skip no-op
        if (old_seg == new_seg) continue;

        // Determine operation type
        std::string op_type;
        if (old_seg.empty() && !new_seg.empty()) op_type = "insert";
        else if (!old_seg.empty() && new_seg.empty()) op_type = "delete";
        else op_type = "replace";

        // Populate precise change
        last_change.line = static_cast<int>(i);
        last_change.col_start = cs;
        last_change.col_end = old_seg.empty() ? cs : (cs + static_cast<int>(old_seg.size()) - 1);
        last_change.old_text = old_seg;
        last_change.new_text = new_seg;
        last_change.type = op_type;
        has_changes = true;

        // Buffer operation for broadcast and merge
        UpdateMessage um{};
        to_message(last_change, um);
        local_ops.push_back(um);
        local_unmerged.push_back(change_to_ext(last_change));
        last_local_op_ns = now_ns();
      }
      
      // Handle line additions (new lines added at end)
      for (size_t i = prev_lines.size(); i < new_lines.size(); ++i) {
        // Ignore trailing empty line insertions
        if (new_lines[i].empty()) continue;
        last_change.line = static_cast<int>(i);
        last_change.col_start = 0;
        last_change.col_end = 0; // insert at start for whole new line
        last_change.old_text = "";
        last_change.new_text = new_lines[i];
        last_change.type = "insert";
        has_changes = true;

        UpdateMessage um{};
        to_message(last_change, um);
        local_ops.push_back(um);
        local_unmerged.push_back(change_to_ext(last_change));
        last_local_op_ns = now_ns();
      }
      
      // Handle line deletions (lines removed from end)
      for (size_t i = new_lines.size(); i < prev_lines.size(); ++i) {
        // Ignore trailing empty line deletions
        if (prev_lines[i].empty()) continue;
        last_change.line = static_cast<int>(i);
        last_change.col_start = 0;
        last_change.col_end = static_cast<int>(prev_lines[i].size()) - 1; // inclusive end
        last_change.old_text = prev_lines[i];
        last_change.new_text = "";
        last_change.type = "delete";
        has_changes = true;

        UpdateMessage um{};
        to_message(last_change, um);
        local_ops.push_back(um);
        local_unmerged.push_back(change_to_ext(last_change));
        last_local_op_ns = now_ns();
      }

      prev_lines = std::move(new_lines);
      if (has_changes) {
        render_display(doc_name, prev_lines, active_users, &last_change);
      }
    }

    // Part 3: Merge and synchronize BEFORE broadcasting
    // "After receiving updates OR after every N=5 operations (whichever comes first)"
    const size_t N_MERGE = 5;
    bool should_merge = !recv_unmerged.empty() || (local_unmerged.size() >= N_MERGE);
    // Do NOT merge if there are unprocessed local file changes
    struct stat st_now{};
    bool local_dirty = (stat(doc_name.c_str(), &st_now) == 0) && (st_now.st_mtime != last_mtime);
    if (should_merge && !local_dirty) {
      auto lines_copy = merge_baseline; // Start from merge baseline (pre-local-changes)
      bool changed = do_merge_apply(lines_copy, local_unmerged, recv_unmerged, g_user_id);
      if (changed) {
        // Trim trailing empty lines prior to write to avoid phantom blanks
        while (!lines_copy.empty() && lines_copy.back().empty()) lines_copy.pop_back();
        // Write back to file
        std::ofstream ofs(doc_name);
        for (size_t i = 0; i < lines_copy.size(); ++i) {
          ofs << lines_copy[i] << "\n";
        }
        ofs.flush();
        ofs.close();
        
        // Update local states before mtime refresh
        prev_lines = lines_copy;
        merge_baseline = lines_copy; // Reset baseline after merge
        
        // Update mtime AFTER writing to prevent re-detection of merge
        if (stat(doc_name.c_str(), &st) == 0) {
          last_mtime = st.st_mtime;
        }
        
        std::cout << "All updates merged successfully\n";
        render_display(doc_name, prev_lines, active_users, nullptr);
        
        just_merged = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    }

    // Quick re-drain after merge to catch late arrivals and merge again
    bool got_more_after_merge = false;
    for (;;) {
      UpdateMessage tmp2;
      if (!g_recv_buf.pop(tmp2)) break;
      if (std::strncmp(tmp2.sender, g_user_id.c_str(), USER_ID_MAX) == 0) continue;
      recv_unmerged.push_back(to_ext(tmp2));
      got_more_after_merge = true;
      std::snprintf(g_last_sender, USER_ID_MAX, "%s", tmp2.sender);
    }
    if (got_more_after_merge && !local_dirty) {
      auto lines_copy2 = merge_baseline;
      bool changed2 = do_merge_apply(lines_copy2, local_unmerged, recv_unmerged, g_user_id);
      if (changed2) {
        while (!lines_copy2.empty() && lines_copy2.back().empty()) lines_copy2.pop_back();
        std::ofstream ofs2(doc_name);
        for (size_t i = 0; i < lines_copy2.size(); ++i) {
          ofs2 << lines_copy2[i] << "\n";
        }
        ofs2.flush();
        ofs2.close();
        prev_lines = lines_copy2;
        merge_baseline = lines_copy2;
        if (stat(doc_name.c_str(), &st) == 0) {
          last_mtime = st.st_mtime;
        }
        std::cout << "All updates merged successfully\n";
        render_display(doc_name, prev_lines, active_users, nullptr);
      }
    }

    // Part 2: Broadcast after accumulating exactly 5 operations (as per assignment)
    const size_t N_BROADCAST = 5;
    if (local_ops.size() >= N_BROADCAST) {
        std::cout << "Broadcasting " << N_BROADCAST << " operations...\n";
        // Refresh active users list before broadcasting
        ucount = 0;
        registry_list(g_registry_seg, users, ucount);
        
        for (size_t idx = 0; idx < ucount; ++idx) {
          const auto &u = users[idx];
          if (std::strncmp(u.user_id, g_user_id.c_str(), USER_ID_MAX) == 0) continue; // skip self
          if (u.queue_name[0] == '\0') continue;
          mqd_t mq_other = mq_open(u.queue_name, O_WRONLY | O_NONBLOCK);
          if (mq_other == (mqd_t)-1) continue;
          
          // Send exactly N_BROADCAST operations
          size_t count = 0;
          for (const auto& op : local_ops) {
            if (count >= N_BROADCAST) break;
            if (mq_send(mq_other, reinterpret_cast<const char*>(&op), sizeof(UpdateMessage), 0) == 0) {
              g_sent_total.fetch_add(1, std::memory_order_relaxed);
              std::snprintf(g_last_target, USER_ID_MAX, "%s", u.user_id);
              count++;
            }
          }
          mq_close(mq_other);
        }
        
        // Remove the sent operations from local_ops
        if (local_ops.size() > N_BROADCAST) {
          local_ops.erase(local_ops.begin(), local_ops.begin() + N_BROADCAST);
        } else {
          local_ops.clear();
        }
    }

    // Fixed 2-second polling interval as per assignment
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

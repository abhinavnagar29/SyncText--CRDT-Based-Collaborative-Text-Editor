#include "../include/registry.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

static constexpr uint32_t REGISTRY_MAGIC = 0x53595854; // 'SYXT'
static constexpr uint32_t REGISTRY_VERSION = 1;
static constexpr std::size_t REGISTRY_SIZE = sizeof(RegistrySegment);

static void initialize_segment(RegistrySegment *seg) {
  seg->magic = REGISTRY_MAGIC;
  seg->version = REGISTRY_VERSION;
  for (std::size_t i = 0; i < MAX_USERS; ++i) {
    seg->users[i].active = 0;
    seg->users[i].user_id[0] = '\0';
    seg->users[i].queue_name[0] = '\0';
  }
}

int registry_open_or_create(int &fd, RegistrySegment *&seg) {
  fd = shm_open(REGISTRY_SHM_NAME, O_RDWR | O_CREAT, 0666);
  if (fd < 0) return -1;

  // Ensure size
  if (ftruncate(fd, REGISTRY_SIZE) != 0) {
    close(fd);
    return -2;
  }

  void *addr = mmap(nullptr, REGISTRY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    return -3;
  }
  seg = reinterpret_cast<RegistrySegment *>(addr);

  // If not initialized, set up
  if (seg->magic != REGISTRY_MAGIC) {
    initialize_segment(seg);
  }
  return 0;
}

int registry_register(RegistrySegment *seg, const char *user_id, const char *queue_name, int &assigned_index) {
  assigned_index = -1;
  // First, if user_id already exists, mark active and return same slot
  for (std::size_t i = 0; i < MAX_USERS; ++i) {
    if (seg->users[i].active == 1 && std::strncmp(seg->users[i].user_id, user_id, USER_ID_MAX) == 0) {
      // Update queue name in case
      std::snprintf(seg->users[i].queue_name, QUEUE_NAME_MAX, "%s", queue_name ? queue_name : "");
      assigned_index = static_cast<int>(i);
      return 0;
    }
  }
  // Try to claim a free slot using CAS on 'active'
  for (std::size_t i = 0; i < MAX_USERS; ++i) {
    volatile int *active_ptr = &seg->users[i].active;
    // GCC builtin CAS: if *active_ptr == 0, set to 1
    if (__sync_bool_compare_and_swap(active_ptr, 0, 1)) {
      std::snprintf(seg->users[i].user_id, USER_ID_MAX, "%s", user_id);
      std::snprintf(seg->users[i].queue_name, QUEUE_NAME_MAX, "%s", queue_name ? queue_name : "");
      assigned_index = static_cast<int>(i);
      return 0;
    }
  }
  return -4; // no slots
}

int registry_unregister(RegistrySegment *seg, const char *user_id) {
  for (std::size_t i = 0; i < MAX_USERS; ++i) {
    if (seg->users[i].active == 1 && std::strncmp(seg->users[i].user_id, user_id, USER_ID_MAX) == 0) {
      seg->users[i].user_id[0] = '\0';
      seg->users[i].queue_name[0] = '\0';
      seg->users[i].active = 0; // release
      return 0;
    }
  }
  return -1;
}

int registry_list(RegistrySegment *seg, UserEntry out_users[MAX_USERS], std::size_t &count) {
  count = 0;
  for (std::size_t i = 0; i < MAX_USERS; ++i) {
    if (seg->users[i].active == 1) {
      out_users[count++] = seg->users[i];
    }
  }
  return 0;
}

#pragma once
#include <cstddef>
#include <cstdint>

// POSIX shared memory name for user registry
#define REGISTRY_SHM_NAME "/synctext_registry"

// Limits
constexpr std::size_t MAX_USERS = 5;
constexpr std::size_t USER_ID_MAX = 32;
constexpr std::size_t QUEUE_NAME_MAX = 64;

// A single user entry kept in shared memory. Designed to be trivially copyable.
struct UserEntry {
  volatile int active;                 // 0 = free, 1 = taken
  char user_id[USER_ID_MAX];           // null-terminated
  char queue_name[QUEUE_NAME_MAX];     // null-terminated (for Part 2)
};

// The registry segment layout. No locks; we rely on atomic CAS on 'active'.
struct RegistrySegment {
  uint32_t magic;      // to detect initialization
  uint32_t version;    // future compatibility
  UserEntry users[MAX_USERS];
};

// API
int registry_open_or_create(int &fd, RegistrySegment *&seg);
int registry_register(RegistrySegment *seg, const char *user_id, const char *queue_name, int &assigned_index);
int registry_unregister(RegistrySegment *seg, const char *user_id);
int registry_list(RegistrySegment *seg, UserEntry out_users[MAX_USERS], std::size_t &count);

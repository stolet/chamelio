#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#include "cham_lib.h"
#include "test_utils.h"

/* Test configurations */
#define TEST_CHAM_IP "192.168.10.14/24"
#define TEST_NIC_ID "d8:00.0"
#define TEST_SHM_SIZE 1024 * 1024 * 1024
#define TEST_QUEUE_NELEMS 16
#define TEST_QUEUE_ELSIZE 64
#define TEST_MAP_NELEMS 32
#define TEST_MAP_ELSIZE 128
#define TEST_CORE_ID 0

static pid_t start_chamelio()
{
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork failed");
    exit(1);
  }

  if (pid == 0) {
    char ip_arg[64];
    char dpdk_arg[64];

    int n1 = snprintf(ip_arg, sizeof(ip_arg), 
        "--ip-addr=%s", TEST_CHAM_IP);
    int n2 = snprintf(dpdk_arg, sizeof(dpdk_arg), 
        "--dpdk-extra=-a%s", TEST_NIC_ID);
        
    if (n1 < 0 || n1 >= (int)sizeof(ip_arg) || 
        n2 < 0 || n2 >= (int)sizeof(dpdk_arg)) 
    {
      fprintf(stderr, "argument too long\n");
      exit(1);
    }

    execl("../build/chamelio/chamelio", "chamelio",
          ip_arg,
          "--fp-cores-max=1",
          dpdk_arg,
          (char *)NULL);

    perror("execl failed");
    exit(1);
  }

  /* Give Chamelio time to start */
  sleep(3);
  printf("Chamelio started with PID %d\n\n", pid);
  return pid;
}

static void test_connect_guest()
{
  printf(ANSI_COLOR_BLUE "Testing cham_connect_guest..." ANSI_COLOR_RESET "\n");
  struct guest_lib *guest = cham_connect_guest();
  TEST_ASSERT(guest != NULL, "guest is NULL");
  TEST_ASSERT(guest->uxsocket_fd >= 0, "invalid uxsocket_fd");
  TEST_ASSERT(guest->shm_fd >= 0, "invalid shm_fd");
  printf(ANSI_COLOR_GREEN "cham_connect_guest test passed" ANSI_COLOR_RESET "\n\n");
}

static void test_new_proto(struct guest_lib *guest)
{
  printf(ANSI_COLOR_BLUE "Testing cham_new_proto..." ANSI_COLOR_RESET "\n");
  struct proto_lib *proto = cham_new_proto(guest, TEST_SHM_SIZE);
  TEST_ASSERT(proto != NULL, "proto is NULL");
  TEST_ASSERT(proto->shm_base != NULL, "shm_base is NULL");
  TEST_ASSERT(proto->shm_size == TEST_SHM_SIZE, "incorrect shm_size");
  TEST_ASSERT(proto->guest == guest, "incorrect guest pointer");
  TEST_ASSERT(proto->nqueues == 0, "nqueues not initialized to 0");
  TEST_ASSERT(proto->nmaps == 0, "nmaps not initialized to 0");
  printf(ANSI_COLOR_GREEN "cham_new_proto test passed" ANSI_COLOR_RESET "\n\n");
}

static void test_new_queue(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_new_queue..." ANSI_COLOR_RESET "\n");
  fflush(stdout);
  struct proto_queue_lib *queue = cham_new_queue(proto, 
      TEST_QUEUE_NELEMS, TEST_QUEUE_ELSIZE);
  TEST_ASSERT(queue != NULL, "queue is NULL");
  TEST_ASSERT(queue->nelems == TEST_QUEUE_NELEMS, "incorrect nelems");
  TEST_ASSERT(queue->elsize == TEST_QUEUE_ELSIZE, "incorrect elsize");
  TEST_ASSERT(queue->proto == proto, "incorrect proto pointer");
  printf(ANSI_COLOR_GREEN "cham_new_queue test passed" ANSI_COLOR_RESET "\n\n");
  fflush(stdout);
}

static void test_new_map(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_new_map..." ANSI_COLOR_RESET "\n");
  fflush(stdout);
  struct proto_map_lib *map = cham_new_map(proto, 
      TEST_MAP_NELEMS, TEST_MAP_ELSIZE);
  TEST_ASSERT(map != NULL, "map is NULL");
  TEST_ASSERT(map->nelems == TEST_MAP_NELEMS, "incorrect nelems");
  TEST_ASSERT(map->elsize == TEST_MAP_ELSIZE, "incorrect elsize");
  TEST_ASSERT(map->proto == proto, "incorrect proto pointer");
  printf(ANSI_COLOR_GREEN "cham_new_map test passed" ANSI_COLOR_RESET "\n\n");
  fflush(stdout);
}

static void test_enable_queue(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_enable_queue..." ANSI_COLOR_RESET "\n");
  int ret = cham_enable_queue(proto, 0, TEST_CORE_ID);
  TEST_ASSERT(ret == 0, "enable_queue failed");
  printf(ANSI_COLOR_GREEN "cham_enable_queue test passed" ANSI_COLOR_RESET "\n\n");
}

static void test_disable_queue(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_disable_queue..." ANSI_COLOR_RESET "\n");
  int ret = cham_disable_queue(proto, 0, TEST_CORE_ID);
  TEST_ASSERT(ret == 0, "disable_queue failed");
  printf(ANSI_COLOR_GREEN "cham_disable_queue test passed" ANSI_COLOR_RESET "\n\n");
}

int main()
{
  // Setup signal handlers for cleanup
  signal(SIGINT, cleanup_handler);
  signal(SIGTERM, cleanup_handler);
  signal(SIGABRT, cleanup_handler);

  printf("Starting Chamelio library tests...\n");

  g_chamelio_pid = start_chamelio();
  
  struct guest_lib *guest = NULL;
  struct proto_lib *proto = NULL;

  test_connect_guest();
  guest = cham_connect_guest();
  TEST_ASSERT(guest != NULL, "guest connection failed");

  test_new_proto(guest);
  proto = cham_new_proto(guest, TEST_SHM_SIZE);
  TEST_ASSERT(proto != NULL, "proto creation failed");

  test_new_queue(proto);
  test_new_map(proto);

  test_enable_queue(proto);
  test_disable_queue(proto);

  printf(ANSI_COLOR_GREEN "All tests passed!" ANSI_COLOR_RESET "\n");

  // Clean shutdown
  kill(g_chamelio_pid, SIGTERM);
  waitpid(g_chamelio_pid, NULL, 0);

  return 0;
}

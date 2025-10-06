#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

#include "cham_lib.h"
#include "test_utils.h"

/* Test configurations */
#define TEST_CHAM_IP "192.168.10.14/24"
#define TEST_NIC_ID "86:00.0"
#define TEST_SHM_SIZE 1024 * 1024 * 1024
#define TEST_QUEUE_NELEMS 16
#define TEST_QUEUE_ELSIZE 64
#define TEST_MAP_NELEMS 32
#define TEST_MAP_ELSIZE 128
#define TEST_CORE_ID 0
#define TEST_EBPF_SIZE 16

static pid_t start_chamelio()
{
  pid_t pid = fork();
  if (pid < 0)
  {
    perror("fork failed");
    exit(1);
  }

  if (pid == 0)
  {
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
  printf("Chamelio started with PID %d\n", pid);
  return pid;
}

static void test_connect_guest()
{
  printf(ANSI_COLOR_BLUE "Testing cham_connect_guest..." ANSI_COLOR_RESET "\n");
  struct guest_lib *guest = cham_connect_guest();
  TEST_ASSERT(guest != NULL, "guest is NULL");
  TEST_ASSERT(guest->uxsocket_fd >= 0, "invalid uxsocket_fd");
  TEST_ASSERT(guest->shm_fd >= 0, "invalid shm_fd");
  printf(ANSI_COLOR_GREEN "cham_connect_guest test passed" ANSI_COLOR_RESET "\n");
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
  printf(ANSI_COLOR_GREEN "cham_new_proto test passed" ANSI_COLOR_RESET "\n");
}

static void test_new_queue(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_new_queue..." ANSI_COLOR_RESET "\n");
  struct proto_queue_lib *queue = cham_new_queue(proto,
                                                 TEST_QUEUE_NELEMS, TEST_QUEUE_ELSIZE);
  TEST_ASSERT(queue != NULL, "queue is NULL");
  TEST_ASSERT(queue->nelems == TEST_QUEUE_NELEMS, "incorrect nelems");
  TEST_ASSERT(queue->elsize == TEST_QUEUE_ELSIZE, "incorrect elsize");
  TEST_ASSERT(queue->proto == proto, "incorrect proto pointer");
  printf(ANSI_COLOR_GREEN "cham_new_queue test passed" ANSI_COLOR_RESET "\n");
}

static void test_new_map(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_new_map..." ANSI_COLOR_RESET "\n");
  struct proto_map_lib *map = cham_new_map(proto,
                                           TEST_MAP_NELEMS, TEST_MAP_ELSIZE);
  TEST_ASSERT(map != NULL, "map is NULL");
  TEST_ASSERT(map->nelems == TEST_MAP_NELEMS, "incorrect nelems");
  TEST_ASSERT(map->elsize == TEST_MAP_ELSIZE, "incorrect elsize");
  TEST_ASSERT(map->proto == proto, "incorrect proto pointer");
  printf(ANSI_COLOR_GREEN "cham_new_map test passed" ANSI_COLOR_RESET "\n");
}

static void test_enable_queue(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_enable_queue..." ANSI_COLOR_RESET "\n");
  int ret = cham_enable_queue(proto, 0, TEST_CORE_ID);
  TEST_ASSERT(ret == 0, "enable_queue failed");
  printf(ANSI_COLOR_GREEN "cham_enable_queue test passed" ANSI_COLOR_RESET "\n");
}

static void test_disable_queue(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_disable_queue..." ANSI_COLOR_RESET "\n");
  int ret = cham_disable_queue(proto, 0, TEST_CORE_ID);
  TEST_ASSERT(ret == 0, "disable_queue failed");
  printf(ANSI_COLOR_GREEN "cham_disable_queue test passed" ANSI_COLOR_RESET "\n");
}

static void test_cham_allocate_ebpf(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_allocate_ebpf..." ANSI_COLOR_RESET "\n");
  struct proto_ebpf_lib *ebpf = cham_allocate_ebpf(proto, TEST_EBPF_SIZE);
  TEST_ASSERT(ebpf != NULL, "ebpf is NULL");
  TEST_ASSERT(ebpf->size == TEST_EBPF_SIZE, "incorrect ebpf size");
  TEST_ASSERT(ebpf->off > 0, "invalid ebpf offset"); // TBC: will it always be necessarily greater than 0?
  printf(ANSI_COLOR_GREEN "cham_allocate_ebpf test passed" ANSI_COLOR_RESET "\n");
}

static void test_cham_upload_ebpf(struct proto_lib *proto)
{
  printf(ANSI_COLOR_BLUE "Testing cham_upload_ebpf..." ANSI_COLOR_RESET "\n");
  TEST_ASSERT(proto->ebpf_program.size == TEST_EBPF_SIZE, "ebpf program not the correct size");
  uint8_t *ebpf_bytecode = malloc(proto->ebpf_program.size);

  static const uint8_t kMinimalEbpfProgram[16] =
      {
          0xB7, 0x00, 0x00, 0x00, // mov64 r0, 0
          0x00, 0x00, 0x00, 0x00,
          0x95, 0x00, 0x00, 0x00, // exit
          0x00, 0x00, 0x00, 0x00};

  memcpy(ebpf_bytecode, kMinimalEbpfProgram, proto->ebpf_program.size);

  int ret = cham_upload_ebpf(proto, ebpf_bytecode);
  TEST_ASSERT(ret == 0, "cham_upload_ebpf failed");
  TEST_ASSERT(proto->ebpf_program.flag > 0, "ebpf upload flag not set correctly");
  // verify contents in shared memory
  uint8_t *shm_addr = (uint8_t *)proto->shm_base + proto->ebpf_program.off;

  int cmp = memcmp(shm_addr, ebpf_bytecode, proto->ebpf_program.size);
  TEST_ASSERT(cmp == 0, "cham_upload_ebpf not at the correct location in shared memory");

  free(ebpf_bytecode);

  printf(ANSI_COLOR_GREEN "cham_upload_ebpf test passed" ANSI_COLOR_RESET "\n");
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

  test_cham_allocate_ebpf(proto);
  test_cham_upload_ebpf(proto);

  printf("All tests passed!\n");

  // Clean shutdown
  kill(g_chamelio_pid, SIGTERM);
  waitpid(g_chamelio_pid, NULL, 0);

  return 0;
}

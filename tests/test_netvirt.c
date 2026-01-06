#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "netvirt.h"
#include "test_utils.h"

/* TODO: Add test for collision*/
/* Test IP addresses */
#define TEST_GID_1 1
#define TEST_GID_2 2
#define TEST_GRE_KEY_1 1
#define TEST_GRE_KEY_2 2
#define TEST_INNER_IP_1 0xC0A80001
#define TEST_INNER_IP_2 0xC0A80002
#define TEST_OUTER_IP_1 0xC0A80101
#define TEST_OUTER_IP_2 0xC0A80102
#define TEST_KEYA_1 TEST_GID_1
#define TEST_KEYA_2 TEST_GID_2
#define TEST_KEYB_1 TEST_INNER_IP_1
#define TEST_KEYB_2 TEST_INNER_IP_2

static void test_netvirt_table_init()
{
  struct netvirt_table nvt = {0};
  
  printf(ANSI_COLOR_BLUE "Testing netvirt table initialization..." ANSI_COLOR_RESET "\n");

  netvirt_table_init(&nvt);
  
  for (int i = 0; i < NETVIRT_LEN; i++)
  {
    TEST_ASSERT(nvt.vals[i].keya == NETVIRT_INVALID, "Key A should be NETVIRT_INVALID after init");
    TEST_ASSERT(nvt.vals[i].keyb == NETVIRT_INVALID, "Key B should be NETVIRT_INVALID after init");
    TEST_ASSERT(nvt.vals[i].gid == NETVIRT_INVALID, "Guest ID should be NETVIRT_INVALID after init gid=%d");
    TEST_ASSERT(nvt.vals[i].gre_key == NETVIRT_INVALID, "GRE key should be NETVIRT_INVALID after init");
    TEST_ASSERT(nvt.vals[i].inner_ip == NETVIRT_INVALID, "Inner IP should be NETVIRT_INVALID after init");
    TEST_ASSERT(nvt.vals[i].outer_ip == NETVIRT_INVALID, "Outer IP should be NETVIRT_INVALID after init");
  }

  printf(ANSI_COLOR_GREEN "PASSED: IP table initialization test" ANSI_COLOR_RESET "\n");
}

static void test_netvirt_table_set()
{
  struct netvirt_table nvt = {0};
  struct netvirt_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing netvirt table set..." ANSI_COLOR_RESET "\n");

  netvirt_table_init(&nvt);
  
  int result = netvirt_table_set(&nvt, TEST_KEYA_1, TEST_KEYB_1,
      TEST_GID_1, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_1);
  TEST_ASSERT(result == 0, "Failed to set netvirt table entry");
  
  entry = netvirt_table_get(&nvt, TEST_KEYA_1, TEST_KEYB_1);
  TEST_ASSERT(entry != NULL, "Failed to get netvirt table entry");
  TEST_ASSERT(entry->gid == TEST_GID_1, "Guest ID mismatch");
  TEST_ASSERT(entry->gre_key == TEST_GRE_KEY_1, "GRE key mismatch");
  TEST_ASSERT(entry->inner_ip == TEST_INNER_IP_1, "Inner IP mismatch");
  TEST_ASSERT(entry->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: netvirt table set test" ANSI_COLOR_RESET "\n");
}

static void test_netvirt_table_set_multiple()
{
  struct netvirt_table nvt = {0};
  struct netvirt_entry *entry1, *entry2;
  
  printf(ANSI_COLOR_BLUE "Testing netvirt table set multiple..." ANSI_COLOR_RESET "\n");

  netvirt_table_init(&nvt);
  
  int result1 = netvirt_table_set(&nvt, TEST_KEYA_1, TEST_KEYB_1,
    TEST_GID_1, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_1);
  TEST_ASSERT(result1 == 0, "Failed to set first netvirt table entry");
  
  int result2 = netvirt_table_set(&nvt, TEST_KEYA_2, TEST_KEYB_2,
    TEST_GID_2, TEST_GRE_KEY_2, TEST_INNER_IP_2, TEST_OUTER_IP_2);
  TEST_ASSERT(result2 == 0, "Failed to set second netvirt table entry");

  entry1 = netvirt_table_get(&nvt, TEST_KEYA_1, TEST_KEYB_1);
  entry2 = netvirt_table_get(&nvt, TEST_KEYA_2, TEST_KEYB_2);

  TEST_ASSERT(entry1 != NULL, "Failed to get first netvirt table entry");
  TEST_ASSERT(entry1->gid == TEST_GID_1, "Guest ID mismatch for first entry");
  TEST_ASSERT(entry1->gre_key == TEST_GRE_KEY_1, "GRE key mismatch for first entry");
  TEST_ASSERT(entry1->inner_ip == TEST_INNER_IP_1, "Inner IP mismatch for first entry");
  TEST_ASSERT(entry1->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch for first entry");

  TEST_ASSERT(entry2 != NULL, "Failed to get second netvirt table entry");
  TEST_ASSERT(entry2->gid == TEST_GID_2, "Guest ID mismatch for second entry");
  TEST_ASSERT(entry2->gre_key == TEST_GRE_KEY_2, "GRE key mismatch for second entry");
  TEST_ASSERT(entry2->inner_ip == TEST_INNER_IP_2, "Inner IP mismatch for second entry");
  TEST_ASSERT(entry2->outer_ip == TEST_OUTER_IP_2, "Outer IP mismatch for second entry");

  printf(ANSI_COLOR_GREEN "PASSED: netvirt table set multiple test" ANSI_COLOR_RESET "\n");
}

static void test_netvirt_table_get_nonexistent()
{
  struct netvirt_table nvt = {0};
  struct netvirt_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing netvirt table get nonexistent..." ANSI_COLOR_RESET "\n");

  netvirt_table_init(&nvt);
  
  entry = netvirt_table_get(&nvt, TEST_KEYA_1, TEST_KEYA_2);
  TEST_ASSERT(entry == NULL, "Got nonexistent netvirt table entry");

  printf(ANSI_COLOR_GREEN "PASSED: netvirt table get nonexistent test" ANSI_COLOR_RESET "\n");
}

static void test_netvirt_table_set_and_overwrite()
{
  struct netvirt_table nvt = {0};
  struct netvirt_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing netvirt table set and overwrite..." ANSI_COLOR_RESET "\n");

  netvirt_table_init(&nvt);
  
  int result1 = netvirt_table_set(&nvt, TEST_KEYA_1, TEST_KEYB_1,
    TEST_GID_1, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_1);
  TEST_ASSERT(result1 == 0, "Failed to set initial entry");
  
  entry = netvirt_table_get(&nvt, TEST_KEYA_1, TEST_KEYB_1);
  TEST_ASSERT(entry != NULL, "Failed to get initial entry");
  TEST_ASSERT(entry->gid == TEST_GID_1, "Guest id mismatch for initial set");
  TEST_ASSERT(entry->gre_key == TEST_GRE_KEY_1, "GRE key mismatch for initial set");
  TEST_ASSERT(entry->inner_ip == TEST_INNER_IP_1, "Inner IP mismatch for initial set");
  TEST_ASSERT(entry->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch for initial set");

  int result2 = netvirt_table_set(&nvt, TEST_KEYA_1, TEST_KEYB_1,
    TEST_GID_2, TEST_GRE_KEY_2, TEST_INNER_IP_2, TEST_OUTER_IP_2);
  TEST_ASSERT(result2 == 0, "Failed to set overwriting entry");
  
  entry = netvirt_table_get(&nvt, TEST_KEYA_1, TEST_KEYB_1);
  TEST_ASSERT(entry != NULL, "Failed to get overwritten entry");
  TEST_ASSERT(entry->gid == TEST_GID_2, "Guest id mismatch after overwrite");
  TEST_ASSERT(entry->gre_key == TEST_GRE_KEY_2, "GRE key mismatch after overwrite");
  TEST_ASSERT(entry->inner_ip == TEST_INNER_IP_2, "Inner IP mismatch after overwrite");
  TEST_ASSERT(entry->outer_ip == TEST_OUTER_IP_2, "Outer IP mismatch after overwrite");

  printf(ANSI_COLOR_GREEN "PASSED: GRE table set and overwrite test" ANSI_COLOR_RESET "\n");
}

static void test_netvirt_parser()
{
  struct netvirt_table nvt_inner = {0};
  struct netvirt_table nvt_gid = {0};
  struct netvirt_entry *inner_entry;
  struct netvirt_entry *gid_entry;
  FILE *fp;
  const char *test_config = "/tmp/test_netvirt_config.csv";
  
  printf(ANSI_COLOR_BLUE "Testing netvirt_parser..." ANSI_COLOR_RESET "\n");

  /* Create temporary config file */
  fp = fopen(test_config, "w");
  TEST_ASSERT(fp != NULL, "Failed to create temporary config file");
  
  fprintf(fp, "GUEST_ID, GRE_KEY, OUTER_IP, INNER_IP\n");
  fprintf(fp, "0, 0, 192.168.10.14, 10.0.0.1\n");
  fprintf(fp, "1, 0, 192.168.10.14, 10.0.0.2\n");
  fprintf(fp, "0, 1, 192.168.10.13, 10.0.0.3\n");
  fclose(fp);

  /* Initialize tables */
  netvirt_table_init(&nvt_inner);
  netvirt_table_init(&nvt_gid);

  /* Parse config file */
  int result = netvirt_parser(&nvt_inner, &nvt_gid, test_config);
  TEST_ASSERT(result == 0, "Failed to parse config file");

  /* Verify Inner IP table entries */
  inner_entry = netvirt_table_get(&nvt_inner, 0, ntohl(inet_addr("10.0.0.1")));
  TEST_ASSERT(inner_entry != NULL, "Failed to find IP entry (gre_key=0, inner_ip=10.0.0.1)");
  TEST_ASSERT(inner_entry->gid == 0, "Guest ID mismatch for first INNER entry");
  TEST_ASSERT(inner_entry->gre_key == 0, "GRE key mismatch for first INNER entry");
  TEST_ASSERT(inner_entry->inner_ip == ntohl(inet_addr("10.0.0.1")), "Inner IP mismatch for first INNER entry");
  TEST_ASSERT(inner_entry->outer_ip == ntohl(inet_addr("192.168.10.14")), "Outer IP mismatch for INNER first entry");

  inner_entry = netvirt_table_get(&nvt_inner, 0, ntohl(inet_addr("10.0.0.2")));
  TEST_ASSERT(inner_entry != NULL, "Failed to find IP entry (gre_key=0, inner_ip=10.0.0.2)");
  TEST_ASSERT(inner_entry->gid == 1, "Guest ID mismatch for second INNER entry");
  TEST_ASSERT(inner_entry->gre_key == 0, "GRE key mismatch for second INNER entry");
  TEST_ASSERT(inner_entry->inner_ip == ntohl(inet_addr("10.0.0.2")), "Inner IP mismatch for second INNER entry");
  TEST_ASSERT(inner_entry->outer_ip == ntohl(inet_addr("192.168.10.14")), "Outer IP mismatch for second INNER entry");

  inner_entry = netvirt_table_get(&nvt_inner, 1, ntohl(inet_addr("10.0.0.3")));
  TEST_ASSERT(inner_entry != NULL, "Failed to find IP entry (gre_key=1, inner_ip=10.0.0.3)");
  TEST_ASSERT(inner_entry->gid == 0, "Guest ID mismatch for third INNER entry");
  TEST_ASSERT(inner_entry->gre_key == 1, "GRE key mismatch for third INNER entry");
  TEST_ASSERT(inner_entry->inner_ip == ntohl(inet_addr("10.0.0.3")), "Inner IP mismatch for third INNER entry");
  TEST_ASSERT(inner_entry->outer_ip == ntohl(inet_addr("192.168.10.13")), "Outer IP mismatch for third INNER entry");

  /* Verify guest ID table entries */
  gid_entry = netvirt_table_get(&nvt_gid, 0, ntohl(inet_addr("192.168.10.14")));
  TEST_ASSERT(gid_entry != NULL, "Failed to find GID entry (gid=0 outer_ip=192.168.10.14,)");
  TEST_ASSERT(gid_entry->gid == 0, "Guest ID mismatch for first GID entry");
  TEST_ASSERT(gid_entry->gre_key == 0, "GRE key mismatch for first GID entry");
  TEST_ASSERT(gid_entry->inner_ip == ntohl(inet_addr("10.0.0.1")), "Inner IP mismatch for first GID entry");
  TEST_ASSERT(gid_entry->outer_ip == ntohl(inet_addr("192.168.10.14")), "Outer IP mismatch for first GID entry");

  gid_entry = netvirt_table_get(&nvt_gid, 1, ntohl(inet_addr("192.168.10.14")));
  TEST_ASSERT(gid_entry != NULL, "Failed to find GRE entry (gid=1, outer_ip=192.168.10.14)");
  TEST_ASSERT(gid_entry->gid == 1, "GRE key mismatch for second GID entry");
  TEST_ASSERT(gid_entry->gre_key == 0, "GRE key mismatch for second GID entry");
  TEST_ASSERT(gid_entry->inner_ip == ntohl(inet_addr("10.0.0.2")), "Inner IP mismatch for second GID entry");
  TEST_ASSERT(gid_entry->outer_ip == ntohl(inet_addr("192.168.10.14")), "Outer IP mismatch for second GID entry");

  gid_entry = netvirt_table_get(&nvt_gid, 0, ntohl(inet_addr("192.168.10.13")));
  TEST_ASSERT(gid_entry != NULL, "Failed to find GRE entry (gid=0 outer_ip=192.168.10.13)");
  TEST_ASSERT(gid_entry->gid == 0, "GRE key mismatch for third GID entry");
  TEST_ASSERT(gid_entry->gre_key == 1, "GRE key mismatch for third GID entry");
  TEST_ASSERT(gid_entry->inner_ip == ntohl(inet_addr("10.0.0.3")), "Inner IP mismatch for third GID entry");
  TEST_ASSERT(gid_entry->outer_ip == ntohl(inet_addr("192.168.10.13")), "Outer IP mismatch for third GID entry");

  /* Clean up temporary file */
  remove(test_config);

  printf(ANSI_COLOR_GREEN "PASSED: netvirt_parser test" ANSI_COLOR_RESET "\n");
}

int main()
{
  printf("Running netvirt tests...\n");

  test_netvirt_table_init();
  test_netvirt_table_set();
  test_netvirt_table_set_multiple();
  test_netvirt_table_get_nonexistent();
  test_netvirt_table_set_and_overwrite();
  test_netvirt_parser();

  printf("All netvirt tests passed!\n");
  return 0;
}

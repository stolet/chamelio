#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netvirt.h"
#include "test_utils.h"

/* Test IP addresses */
#define TEST_GRE_KEY_1 1
#define TEST_GRE_KEY_2 2
#define TEST_INNER_IP_1 0xC0A80001
#define TEST_INNER_IP_2 0xC0A80002
#define TEST_OUTER_IP_1 0xC0A80101
#define TEST_OUTER_IP_2 0xC0A80102
#define TEST_OUTER_IP_COLLISION 0xC0A80103

#define TEST_GID_1 1
#define TEST_GID_2 2
#define TEST_GID_COLLISION 3

static void test_ip_table_init()
{
  struct ip_table it = {0};
  
  printf(ANSI_COLOR_BLUE "Testing IP table initialization..." ANSI_COLOR_RESET "\n");

  netvirt_ip_init(&it);
  
  TEST_ASSERT(it.len == 0, "IP table length should be 0 after init");
  
  for (int i = 0; i < NETVIRT_LEN; i++)
  {
    TEST_ASSERT(it.ips[i].gre_key == NETVIRT_INVALID, "GRE key should be NETVIRT_INVALID after init");
    TEST_ASSERT(it.ips[i].inner_ip == NETVIRT_INVALID, "Inner IP should be NETVIRT_INVALID after init");
    TEST_ASSERT(it.ips[i].outer_ip == NETVIRT_INVALID, "Outer IP should be NETVIRT_INVALID after init");
  }

  printf(ANSI_COLOR_GREEN "PASSED: IP table initialization test" ANSI_COLOR_RESET "\n");
}

static void test_ip_table_set()
{
  struct ip_table it = {0};
  struct ip_table_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing IP table set..." ANSI_COLOR_RESET "\n");

  netvirt_ip_init(&it);
  
  int result = netvirt_ip_set(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_1);
  TEST_ASSERT(result == 0, "Failed to set IP table entry");
  
  entry = netvirt_ip_get(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1);
  TEST_ASSERT(entry != NULL, "Failed to get IP table entry");
  TEST_ASSERT(entry->gre_key == TEST_GRE_KEY_1, "GRE key mismatch");
  TEST_ASSERT(entry->inner_ip == TEST_INNER_IP_1, "Inner IP mismatch");
  TEST_ASSERT(entry->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: IP table set test" ANSI_COLOR_RESET "\n");
}

static void test_ip_table_set_multiple()
{
  struct ip_table it = {0};
  struct ip_table_entry *entry1, *entry2;
  
  printf(ANSI_COLOR_BLUE "Testing IP table set multiple..." ANSI_COLOR_RESET "\n");

  netvirt_ip_init(&it);
  
  int result1 = netvirt_ip_set(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_1);
  TEST_ASSERT(result1 == 0, "Failed to set first IP table entry");
  
  int result2 = netvirt_ip_set(&it, TEST_GRE_KEY_2, TEST_INNER_IP_2, TEST_OUTER_IP_2);
  TEST_ASSERT(result2 == 0, "Failed to set second IP table entry");

  entry1 = netvirt_ip_get(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1);
  entry2 = netvirt_ip_get(&it, TEST_GRE_KEY_2, TEST_INNER_IP_2);

  TEST_ASSERT(entry1 != NULL, "Failed to get first IP table entry");
  TEST_ASSERT(entry1->gre_key == TEST_GRE_KEY_1, "GRE key mismatch for first entry");
  TEST_ASSERT(entry1->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch for first entry");

  TEST_ASSERT(entry2 != NULL, "Failed to get second IP table entry");
  TEST_ASSERT(entry2->gre_key == TEST_GRE_KEY_2, "GRE key mismatch for second entry");
  TEST_ASSERT(entry2->outer_ip == TEST_OUTER_IP_2, "Outer IP mismatch for second entry");

  printf(ANSI_COLOR_GREEN "PASSED: IP table set multiple test" ANSI_COLOR_RESET "\n");
}

static void test_ip_table_collision()
{
  struct ip_table it = {0};
  struct ip_table_entry *entry1, *entry_collision;
  
  printf(ANSI_COLOR_BLUE "Testing IP table collision handling..." ANSI_COLOR_RESET "\n");

  netvirt_ip_init(&it);
  
  int result1 = netvirt_ip_set(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_1);
  TEST_ASSERT(result1 == 0, "Failed to set first entry");
  
  int result_collision = netvirt_ip_set(&it, TEST_GRE_KEY_1, TEST_INNER_IP_2, TEST_OUTER_IP_COLLISION);
  TEST_ASSERT(result_collision == 0, "Failed to set colliding entry");
  
  entry1 = netvirt_ip_get(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1);
  entry_collision = netvirt_ip_get(&it, TEST_GRE_KEY_1, TEST_INNER_IP_2);

  TEST_ASSERT(entry1 != NULL, "Failed to get first IP table entry");
  TEST_ASSERT(entry1->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch for first entry");

  TEST_ASSERT(entry_collision != NULL, "Failed to get colliding IP table entry");
  TEST_ASSERT(entry_collision->outer_ip == TEST_OUTER_IP_COLLISION, "Outer IP mismatch for colliding entry");

  printf(ANSI_COLOR_GREEN "PASSED: IP table collision handling test" ANSI_COLOR_RESET "\n");
}

static void test_ip_table_get_nonexistent()
{
  struct ip_table it = {0};
  struct ip_table_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing IP table get nonexistent..." ANSI_COLOR_RESET "\n");

  netvirt_ip_init(&it);
  
  entry = netvirt_ip_get(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1);
  TEST_ASSERT(entry == NULL || entry->gre_key == 0, "Got nonexistent IP table entry");

  printf(ANSI_COLOR_GREEN "PASSED: IP table get nonexistent test" ANSI_COLOR_RESET "\n");
}

static void test_ip_table_set_and_overwrite()
{
  struct ip_table it = {0};
  struct ip_table_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing IP table set and overwrite..." ANSI_COLOR_RESET "\n");

  netvirt_ip_init(&it);
  
  int result1 = netvirt_ip_set(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_1);
  TEST_ASSERT(result1 == 0, "Failed to set initial entry");
  
  entry = netvirt_ip_get(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1);
  TEST_ASSERT(entry != NULL, "Failed to get initial entry");
  TEST_ASSERT(entry->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch for initial set");

  int result2 = netvirt_ip_set(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1, TEST_OUTER_IP_2);
  TEST_ASSERT(result2 == 0, "Failed to set overwriting entry");
  
  entry = netvirt_ip_get(&it, TEST_GRE_KEY_1, TEST_INNER_IP_1);
  TEST_ASSERT(entry != NULL, "Failed to get overwritten entry");
  TEST_ASSERT(entry->outer_ip == TEST_OUTER_IP_2, "GRE key mismatch after overwrite");

  printf(ANSI_COLOR_GREEN "PASSED: GRE table set and overwrite test" ANSI_COLOR_RESET "\n");
}

static void test_gre_table_init()
{
  struct gre_table gt = {0};
  
  printf(ANSI_COLOR_BLUE "Testing GRE table initialization..." ANSI_COLOR_RESET "\n");

  netvirt_gre_init(&gt);
  
  TEST_ASSERT(gt.len == 0, "GRE table length should be 0 after init");
  
  for (int i = 0; i < NETVIRT_LEN; i++)
  {
    TEST_ASSERT(gt.gre[i].gre_key == NETVIRT_INVALID, "GRE key should be NETVIRT_INVALID after init");
    TEST_ASSERT(gt.gre[i].outer_ip == NETVIRT_INVALID, "Outer IP should be NETVIRT_INVALID after init");
    TEST_ASSERT(gt.gre[i].gid == NETVIRT_INVALID, "Guest id should be NETVIRT_INVALID after init");
  }

  printf(ANSI_COLOR_GREEN "PASSED: GRE table initialization test" ANSI_COLOR_RESET "\n");
}

static void test_gre_table_set()
{
  struct gre_table gt = {0};
  struct gre_table_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing GRE table set..." ANSI_COLOR_RESET "\n");

  netvirt_gre_init(&gt);
  
  int result = netvirt_gre_set(&gt, TEST_OUTER_IP_1, TEST_GID_1, TEST_GRE_KEY_1);
  TEST_ASSERT(result == 0, "Failed to set GRE table entry");
  
  entry = netvirt_gre_get(&gt, TEST_OUTER_IP_1, TEST_GID_1);
  TEST_ASSERT(entry != NULL, "Failed to get GRE table entry");
  TEST_ASSERT(entry->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch");
  TEST_ASSERT(entry->gid == TEST_GID_1, "Guest id mismatch");
  TEST_ASSERT(entry->gre_key == TEST_GRE_KEY_1, "GRE key mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: GRE table set test" ANSI_COLOR_RESET "\n");
}

static void test_gre_table_set_multiple()
{
  struct gre_table gt = {0};
  struct gre_table_entry *entry1, *entry2;
  
  printf(ANSI_COLOR_BLUE "Testing GRE table set multiple..." ANSI_COLOR_RESET "\n");

  netvirt_gre_init(&gt);
  
  int result1 = netvirt_gre_set(&gt, TEST_OUTER_IP_1, TEST_GID_1, TEST_GRE_KEY_1);
  TEST_ASSERT(result1 == 0, "Failed to set first GRE table entry");
  
  int result2 = netvirt_gre_set(&gt, TEST_OUTER_IP_2, TEST_GID_2, TEST_GRE_KEY_2);
  TEST_ASSERT(result2 == 0, "Failed to set second GRE table entry");

  entry1 = netvirt_gre_get(&gt, TEST_OUTER_IP_1, TEST_GID_1);
  entry2 = netvirt_gre_get(&gt, TEST_OUTER_IP_2, TEST_GID_2);

  TEST_ASSERT(entry1 != NULL, "Failed to get first GRE table entry");
  TEST_ASSERT(entry1->gre_key == TEST_GRE_KEY_1, "GRE key mismatch for first entry");
  TEST_ASSERT(entry1->outer_ip == TEST_OUTER_IP_1, "Outer IP mismatch for first entry");

  TEST_ASSERT(entry2 != NULL, "Failed to get second GRE table entry");
  TEST_ASSERT(entry2->gre_key == TEST_GRE_KEY_2, "GRE key mismatch for second entry");
  TEST_ASSERT(entry2->outer_ip == TEST_OUTER_IP_2, "Outer IP mismatch for second entry");

  printf(ANSI_COLOR_GREEN "PASSED: GRE table set multiple test" ANSI_COLOR_RESET "\n");
}

static void test_gre_table_collision()
{
  struct gre_table gt = {0};
  struct gre_table_entry *entry1, *entry_collision;
  
  printf(ANSI_COLOR_BLUE "Testing GRE table collision handling..." ANSI_COLOR_RESET "\n");

  netvirt_gre_init(&gt);
  
  int result1 = netvirt_gre_set(&gt, TEST_OUTER_IP_1, TEST_GID_1, TEST_GRE_KEY_1);
  TEST_ASSERT(result1 == 0, "Failed to set first entry");
  
  int result_collision = netvirt_gre_set(&gt, TEST_OUTER_IP_1, TEST_GID_COLLISION, TEST_GRE_KEY_2);
  TEST_ASSERT(result_collision == 0, "Failed to set colliding entry");
  
  entry1 = netvirt_gre_get(&gt, TEST_OUTER_IP_1, TEST_GID_1);
  entry_collision = netvirt_gre_get(&gt, TEST_OUTER_IP_1, TEST_GID_COLLISION);

  TEST_ASSERT(entry1 != NULL, "Failed to get first GRE table entry");
  TEST_ASSERT(entry1->gre_key == TEST_GRE_KEY_1, "GRE key mismatch for first entry");

  TEST_ASSERT(entry_collision != NULL, "Failed to get colliding GRE table entry");
  TEST_ASSERT(entry_collision->gre_key == TEST_GRE_KEY_2, "GRE key mismatch for colliding entry");

  printf(ANSI_COLOR_GREEN "PASSED: GRE table collision handling test" ANSI_COLOR_RESET "\n");
}

static void test_gre_table_get_nonexistent()
{
  struct gre_table gt = {0};
  struct gre_table_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing GRE table get nonexistent..." ANSI_COLOR_RESET "\n");

  netvirt_gre_init(&gt);
  
  entry = netvirt_gre_get(&gt, TEST_OUTER_IP_1, TEST_GID_1);
  TEST_ASSERT(entry == NULL || entry->gre_key == 0, "Got nonexistent GRE table entry");

  printf(ANSI_COLOR_GREEN "PASSED: GRE table get nonexistent test" ANSI_COLOR_RESET "\n");
}

static void test_gre_table_set_and_overwrite()
{
  struct gre_table gt = {0};
  struct gre_table_entry *entry;
  
  printf(ANSI_COLOR_BLUE "Testing GRE table set and overwrite..." ANSI_COLOR_RESET "\n");

  netvirt_gre_init(&gt);
  
  int result1 = netvirt_gre_set(&gt, TEST_OUTER_IP_1, TEST_GID_1, TEST_GRE_KEY_1);
  TEST_ASSERT(result1 == 0, "Failed to set initial entry");
  
  entry = netvirt_gre_get(&gt, TEST_OUTER_IP_1, TEST_GID_1);
  TEST_ASSERT(entry != NULL, "Failed to get initial entry");
  TEST_ASSERT(entry->gre_key == TEST_GRE_KEY_1, "GRE key mismatch for initial set");

  int result2 = netvirt_gre_set(&gt, TEST_OUTER_IP_1, TEST_GID_1, TEST_GRE_KEY_2);
  TEST_ASSERT(result2 == 0, "Failed to set overwriting entry");
  
  entry = netvirt_gre_get(&gt, TEST_OUTER_IP_1, TEST_GID_1);
  TEST_ASSERT(entry != NULL, "Failed to get overwritten entry");

  fprintf(stderr, "actual=%d expected=%d\n", entry->gre_key, TEST_GRE_KEY_2);
  TEST_ASSERT(entry->gre_key == TEST_GRE_KEY_2, "GRE key mismatch after overwrite");

  printf(ANSI_COLOR_GREEN "PASSED: GRE table set and overwrite test" ANSI_COLOR_RESET "\n");
}

int main()
{
  printf("Running netvirt tests...\n");

  test_ip_table_init();
  test_ip_table_set();
  test_ip_table_set_multiple();
  test_ip_table_collision();
  test_ip_table_get_nonexistent();
  test_ip_table_set_and_overwrite();
  test_gre_table_init();
  test_gre_table_set();
  test_gre_table_set_multiple();
  test_gre_table_collision();
  test_gre_table_get_nonexistent();
  test_gre_table_set_and_overwrite();

  printf("All netvirt tests passed!\n");
  return 0;
}

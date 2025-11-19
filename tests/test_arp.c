#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arp.h"
#include "test_utils.h"

/* 192.168.0.1 */
#define TEST_IP_1 0xC0A80001
/* 192.168.0.2 */
#define TEST_IP_2 0xC0A80002
/* 192.168.0.3 */
#define TEST_IP_COLLISION 0xC0A80003

#define TEST_MAC_1 {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}
#define TEST_MAC_2 {0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB}
#define TEST_MAC_COLLISION {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE}

static void test_single_insert()
{
  int ret;
  struct arp_entry *entry;
  struct arp_table at = {0};
  __u8 mac1[6] = TEST_MAC_1;
  
  printf(ANSI_COLOR_BLUE "Testing single ARP insert..." ANSI_COLOR_RESET "\n");

  ret = arp_insert(&at, TEST_IP_1, mac1);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  entry = arp_lookup(&at, TEST_IP_1);

  TEST_ASSERT(entry != NULL, "Failed to find inserted ARP entry");
  TEST_ASSERT(memcmp(entry->mac, mac1, 6) == 0, "MAC address mismatch");
  TEST_ASSERT(entry->pending == 0, "Pending value mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: single ARP insert test" 
      ANSI_COLOR_RESET "\n");
}

static void test_multiple_inserts()
{
  int ret;
  struct arp_entry *entry1, *entry2;
  struct arp_table at = {0};
  __u8 mac1[6] = TEST_MAC_1;
  __u8 mac2[6] = TEST_MAC_2;
  
  printf(ANSI_COLOR_BLUE "Testing multiple ARP inserts..." 
      ANSI_COLOR_RESET "\n");

  ret = arp_insert(&at, TEST_IP_1, mac1);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  ret = arp_insert(&at, TEST_IP_2, mac2);
  TEST_ASSERT(ret == 0, "Failed to insert IP");

  entry1 = arp_lookup(&at, TEST_IP_1);
  entry2 = arp_lookup(&at, TEST_IP_2);

  TEST_ASSERT(entry1 != NULL, "Failed to find first ARP entry");
  TEST_ASSERT(memcmp(entry1->mac, mac1, 6) == 0, 
      "MAC address mismatch for first entry");
  TEST_ASSERT(entry1->pending == 0, "Pending value mismatch");

  TEST_ASSERT(entry2 != NULL, "Failed to find second ARP entry");
  TEST_ASSERT(memcmp(entry2->mac, mac2, 6) == 0, 
      "MAC address mismatch for second entry");
  TEST_ASSERT(entry2->pending == 0, "Pending value mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: multiple ARP inserts test" 
      ANSI_COLOR_RESET "\n");
}

static void test_collision_handling()
{
  int ret;
  struct arp_entry *entry1, *entry_collision;
  struct arp_table at = {0};
  __u8 mac1[6] = TEST_MAC_1;
  __u8 mac_collision[6] = TEST_MAC_COLLISION;
  
  printf(ANSI_COLOR_BLUE "Testing ARP collision handling..." 
      ANSI_COLOR_RESET "\n");

  ret = arp_insert(&at, TEST_IP_1, mac1);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  ret = arp_insert(&at, TEST_IP_COLLISION, mac_collision);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  entry1 = arp_lookup(&at, TEST_IP_1);
  entry_collision = arp_lookup(&at, TEST_IP_COLLISION);

  TEST_ASSERT(entry1 != NULL, "Failed to find first ARP entry");
  TEST_ASSERT(memcmp(entry1->mac, mac1, 6) == 0, 
      "MAC address mismatch for first entry");
  TEST_ASSERT(entry1->pending == 0, "Pending value mismatch");

  TEST_ASSERT(entry_collision != NULL, "Failed to find colliding ARP entry");
  TEST_ASSERT(memcmp(entry_collision->mac, mac_collision, 6) == 0, 
      "MAC address mismatch for colliding entry");
  TEST_ASSERT(entry_collision->pending == 0, "Pending value mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: ARP collision handling test" 
      ANSI_COLOR_RESET "\n");
}

static void test_lookup_nonexistent()
{
  struct arp_entry *entry;
  struct arp_table at = {0};
  
  printf(ANSI_COLOR_BLUE "Testing lookup of nonexistent ARP entry..." 
      ANSI_COLOR_RESET "\n");

  entry = arp_lookup(&at, TEST_IP_1);
  TEST_ASSERT(entry == NULL, "Found nonexistent ARP entry");

  printf(ANSI_COLOR_GREEN "PASSED: lookup of nonexistent ARP entry test" 
      ANSI_COLOR_RESET "\n");
}

static void test_insert_and_overwrite()
{
  int ret;
  struct arp_entry *entry;
  struct arp_table at = {0};
  __u8 mac1[6] = TEST_MAC_1;
  __u8 mac2[6] = TEST_MAC_2;
  
  printf(ANSI_COLOR_BLUE "Testing ARP insert and overwrite..." 
      ANSI_COLOR_RESET "\n");

  ret = arp_insert(&at, TEST_IP_1, mac1);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  entry = arp_lookup(&at, TEST_IP_1);
  TEST_ASSERT(entry != NULL, "Failed to find inserted ARP entry");
  TEST_ASSERT(memcmp(entry->mac, mac1, 6) == 0, 
      "MAC address mismatch for initial insert");
  TEST_ASSERT(entry->pending == 0, "Pending value mismatch");

  ret = arp_insert(&at, TEST_IP_1, mac2);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  entry = arp_lookup(&at, TEST_IP_1);
  TEST_ASSERT(entry != NULL, "Failed to find overwritten ARP entry");
  TEST_ASSERT(memcmp(entry->mac, mac2, 6) == 0, 
      "MAC address mismatch after overwrite");
  TEST_ASSERT(entry->pending == 0, "Pending value mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: ARP insert and overwrite test" 
      ANSI_COLOR_RESET "\n");
}

static void test_insert_pending()
{
  int ret;
  struct arp_entry *entry;
  struct arp_table at = {0};
  
  printf(ANSI_COLOR_BLUE "Testing ARP insert pending.." ANSI_COLOR_RESET "\n");

  ret = arp_insert_pending(&at, TEST_IP_1);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  entry = arp_lookup(&at, TEST_IP_1);

  TEST_ASSERT(entry != NULL, "Failed to find inserted ARP entry");
  TEST_ASSERT(entry->pending == 1, "Pending value mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: single ARP insert test" 
      ANSI_COLOR_RESET "\n");
}

static void test_insert_pending_overwrite()
{
  int ret;
  struct arp_entry *entry;
  struct arp_table at = {0};
  __u8 mac2[6] = TEST_MAC_2;
  
  printf(ANSI_COLOR_BLUE "Testing ARP insert pending and overwrite..." 
      ANSI_COLOR_RESET "\n");

  ret = arp_insert_pending(&at, TEST_IP_1);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  entry = arp_lookup(&at, TEST_IP_1);
  TEST_ASSERT(entry != NULL, "Failed to find inserted ARP entry");
  TEST_ASSERT(entry->pending == 1, "Pending value mismatch");

  ret = arp_insert(&at, TEST_IP_1, mac2);
  TEST_ASSERT(ret == 0, "Failed to insert IP");
  
  entry = arp_lookup(&at, TEST_IP_1);
  TEST_ASSERT(entry != NULL, "Failed to find overwritten ARP entry");
  TEST_ASSERT(memcmp(entry->mac, mac2, 6) == 0, 
      "MAC address mismatch after overwrite");
  TEST_ASSERT(entry->pending == 0, "Pending value mismatch");

  printf(ANSI_COLOR_GREEN "PASSED: ARP insert and overwrite test" 
      ANSI_COLOR_RESET "\n");
}

static void test_arp_table_full()
{
  int ret;
  __u32 ip;
  struct arp_table at = {0};
  __u8 mac[6] = TEST_MAC_1;
  
  printf(ANSI_COLOR_BLUE "Testing ARP table full condition..." ANSI_COLOR_RESET "\n");

  /* Fill the ARP table */
  for (int i = 0; i < ARP_TABLE_SIZE; i++)
  {
    ip = 0xC0A80000 + i;
    ret = arp_insert(&at, ip, mac);
    TEST_ASSERT(ret == 0, "Failed to insert entry into ARP table");
  }

  /* Attempt to insert into a full table */
  ret = arp_insert(&at, 0xDEADBEEF, mac);
  TEST_ASSERT(ret == -1, "ARP insert should fail when table is full");

  printf(ANSI_COLOR_GREEN "PASSED: ARP table full condition test" ANSI_COLOR_RESET "\n");
}

int main()
{
  printf("Running ARP tests...\n");

  test_single_insert();
  test_multiple_inserts();
  test_collision_handling();
  test_lookup_nonexistent();
  test_insert_and_overwrite();
  test_insert_pending();
  test_insert_pending_overwrite();
  test_arp_table_full();

  printf("All ARP tests passed!\n");
  return 0;
}

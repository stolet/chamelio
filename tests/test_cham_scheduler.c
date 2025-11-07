#include <stdio.h>
#include <stdlib.h>

#include "cham_fast.h"
#include "queue.h"
#include "scheduler_fns.h"
#include "test_utils.h"

static void test_sched_init() 
{
  printf(ANSI_COLOR_BLUE "Testing sched_init..." ANSI_COLOR_RESET "\n");
  struct cham_scheduler sched;
  
  sched_init(&sched);
  
  TEST_ASSERT(sched.head == SCHED_ID_INVALID, 
    "Head should be invalid after initialization");
  TEST_ASSERT(sched.tail == SCHED_ID_INVALID, 
    "Tail should be invalid after initialization");
  
  printf(ANSI_COLOR_GREEN "PASSED: Scheduler initialization test" 
      ANSI_COLOR_RESET "\n");
}

static void test_sched_add() 
{
  printf(ANSI_COLOR_BLUE "Testing sched_add..." ANSI_COLOR_RESET "\n");
  struct cham_scheduler sched;

  sched_init(&sched);
  
  /* Test adding first entry */
  TEST_ASSERT(sched_add(&sched, 1, 100) == 0, "Failed to add first entry");
  TEST_ASSERT(sched.head == 1, "Head should be 1 after first addition");
  TEST_ASSERT(sched.tail == 1, "Tail should be 1 after first addition");

  /* Test adding higher priority entry */
  TEST_ASSERT(sched_add(&sched, 2, 200) == 0, "Failed to add second entry");
  TEST_ASSERT(sched.head == 2, "Head should be 2 (higher priority)");
  TEST_ASSERT(sched.entries[2].next_entry == 1, "Entry 2 should point to 1");

  /* Test adding lower priority entry */
  TEST_ASSERT(sched_add(&sched, 3, 50) == 0, "Failed to add third entry");
  TEST_ASSERT(sched.tail == 3, "Tail should be 3 (lowest priority)");
  TEST_ASSERT(sched.entries[1].next_entry == 3, "Entry 1 should point to 3");
  
  printf(ANSI_COLOR_GREEN "PASSED: Scheduler add test" 
      ANSI_COLOR_RESET "\n");
}

static void test_sched_head() 
{
  printf(ANSI_COLOR_BLUE "Testing sched_head..." ANSI_COLOR_RESET "\n");
  struct cham_scheduler sched;
  struct cham_sched_entry *entry;
  
  sched_init(&sched);

  /* Test head on empty scheduler */
  entry = sched_head(&sched);
  TEST_ASSERT(entry == NULL, "Head should be NULL on empty scheduler");

  /* Add an entry and test head */
  sched_add(&sched, 1, 100);
  entry = sched_head(&sched);
  TEST_ASSERT(entry != NULL, "Head should not be NULL after adding entry");
  TEST_ASSERT(entry->id == 1, "Head should have id 1");
  TEST_ASSERT(entry->priority == 100, "Head should have priority 100");

  /* Add higher priority entry and test head */
  sched_add(&sched, 2, 200);
  entry = sched_head(&sched);
  TEST_ASSERT(entry->id == 2, "Head should have id 2 (higher priority)");
  TEST_ASSERT(entry->priority == 200, "Head should have priority 200");
  
  printf(ANSI_COLOR_GREEN "PASSED: Scheduler head test" ANSI_COLOR_RESET "\n");
}

static void test_sched_pop() 
{
  printf(ANSI_COLOR_BLUE "Testing sched_pop..." ANSI_COLOR_RESET "\n");
  struct cham_scheduler sched;
  struct cham_sched_entry *entry;
  
  sched_init(&sched);

  /* Test pop on empty scheduler */
  TEST_ASSERT(sched_pop(&sched) == -1, "Pop on empty scheduler should return -1");

  /* Add entries with different priorities */
  sched_add(&sched, 1, 100);
  sched_add(&sched, 2, 200);
  sched_add(&sched, 3, 50);

  /* Pop highest priority and verify new head */
  TEST_ASSERT(sched_pop(&sched) == 0, "Pop should succeed");
  entry = sched_head(&sched);
  TEST_ASSERT(entry->id == 1, "New head should be id 1");

  /* Pop again and verify */
  TEST_ASSERT(sched_pop(&sched) == 0, "Second pop should succeed");
  entry = sched_head(&sched);
  TEST_ASSERT(entry->id == 3, "New head should be id 3");

  /* Pop last entry */
  TEST_ASSERT(sched_pop(&sched) == 0, "Third pop should succeed");
  TEST_ASSERT(sched.head == SCHED_ID_INVALID, 
      "Head should be invalid after popping all entries");
  TEST_ASSERT(sched.tail == SCHED_ID_INVALID, 
      "Tail should be invalid after popping all entries");
  
  printf(ANSI_COLOR_GREEN "PASSED: Scheduler pop test" ANSI_COLOR_RESET "\n");
}

int main() 
{
  printf("Running scheduler tests...\n");
  
  test_sched_init();
  test_sched_add();
  test_sched_head();
  test_sched_pop();
  
  printf("All scheduler tests passed!\n");
  return 0;
}

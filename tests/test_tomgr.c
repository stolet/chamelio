#include <stdio.h>
#include <stdlib.h>
#include "tomgr.h"
#include "test_utils.h"

static void test_tomgr_insert()
{
  printf(ANSI_COLOR_BLUE "Testing tomgr_insert..." ANSI_COLOR_RESET "\n");
  struct tomgr *mgr;
  struct to_entry *inserted, *peeked;

  mgr = tomgr_init();
  TEST_ASSERT(mgr != NULL, "Failed to initialize timeout manager");

  /* Insert first entry */
  inserted = tomgr_insert(mgr, TO_ARP, 100, (void *)1);
  TEST_ASSERT(inserted != NULL, "Failed to insert first entry");
  TEST_ASSERT(inserted->to == 100, "Inserted entry timeout should be 100");
  TEST_ASSERT(inserted->data == (void *)1, "Inserted entry data should be 1");
  TEST_ASSERT(inserted->heap_idx == 0, 
      "Heap index of first entry should be 0");

  peeked = tomgr_peek(mgr);
  TEST_ASSERT(peeked == inserted, "Peeked entry should match inserted entry");

  /* Insert second entry with earlier timeout */
  inserted = tomgr_insert(mgr, TO_ARP, 50, (void *)2);
  TEST_ASSERT(inserted != NULL, "Failed to insert second entry");
  TEST_ASSERT(inserted->to == 50, "Inserted entry timeout should be 50");
  TEST_ASSERT(inserted->data == (void *)2, "Inserted entry data should be 2");
  TEST_ASSERT(inserted->heap_idx == 0, 
      "Heap index of second entry should be 0 after heapify");

  peeked = tomgr_peek(mgr);
  TEST_ASSERT(peeked == inserted, 
      "Peeked entry should match the earliest timeout");

  free(mgr);
  printf(ANSI_COLOR_GREEN "PASSED: tomgr_insert test" ANSI_COLOR_RESET "\n");
}

static void test_tomgr_cancel()
{
  printf(ANSI_COLOR_BLUE "Testing tomgr_cancel..." ANSI_COLOR_RESET "\n");
  struct tomgr *mgr;
  struct to_entry *entry1, *entry2, *entry3, *peeked;

  mgr = tomgr_init();
  TEST_ASSERT(mgr != NULL, "Failed to initialize timeout manager");

  /* Insert entries */
  entry1 = tomgr_insert(mgr, TO_ARP, 100, (void *)1);
  entry2 = tomgr_insert(mgr, TO_ARP, 50, (void *)2);
  entry3 = tomgr_insert(mgr, TO_ARP, 150, (void *)3);

  /* Cancel middle entry */
  TEST_ASSERT(tomgr_cancel(mgr, entry2) == 0, "Failed to cancel entry2");
  TEST_ASSERT(entry2->heap_idx == TO_INVALID, 
      "Heap index of canceled entry2 should be invalidated");
  peeked = tomgr_peek(mgr);
  TEST_ASSERT(peeked == entry1, "Peeked entry should now be entry1");

  /* Cancel first entry */
  TEST_ASSERT(tomgr_cancel(mgr, entry1) == 0, "Failed to cancel entry1");
  TEST_ASSERT(entry1->heap_idx == TO_INVALID, 
      "Heap index of canceled entry1 should be invalidated");
  peeked = tomgr_peek(mgr);
  TEST_ASSERT(peeked == entry3, "Peeked entry should now be entry3");

  /* Cancel last entry */
  TEST_ASSERT(tomgr_cancel(mgr, entry3) == 0, "Failed to cancel entry3");
  TEST_ASSERT(entry3->heap_idx == TO_INVALID, 
      "Heap index of canceled entry3 should be invalidated");
  peeked = tomgr_peek(mgr);
  TEST_ASSERT(peeked == NULL, 
      "Peeked entry should be NULL after all entries are canceled");

  free(mgr);
  printf(ANSI_COLOR_GREEN "PASSED: tomgr_cancel test" ANSI_COLOR_RESET "\n");
}

static void test_tomgr_peek()
{
  printf(ANSI_COLOR_BLUE "Testing tomgr_peek..." ANSI_COLOR_RESET "\n");
  struct tomgr *mgr;
  struct to_entry *peeked;

  mgr = tomgr_init();
  TEST_ASSERT(mgr != NULL, "Failed to initialize timeout manager");

  /* Peek on empty manager */
  peeked = tomgr_peek(mgr);
  TEST_ASSERT(peeked == NULL, "Peek should return NULL on empty manager");

  /* Insert an entry and peek */
  tomgr_insert(mgr, TO_ARP, 100, (void *)1);
  peeked = tomgr_peek(mgr);
  TEST_ASSERT(peeked != NULL, "Peeked entry should not be NULL");
  TEST_ASSERT(peeked->to == 100, "Peeked entry timeout should be 100");
  TEST_ASSERT(peeked->data == (void *)1, "Peeked entry data should be 1");

  free(mgr);
  printf(ANSI_COLOR_GREEN "PASSED: tomgr_peek test" ANSI_COLOR_RESET "\n");
}

static void test_tomgr_pop()
{
  printf(ANSI_COLOR_BLUE "Testing tomgr_pop..." ANSI_COLOR_RESET "\n");
  struct tomgr *mgr;
  struct to_entry *popped;

  mgr = tomgr_init();
  TEST_ASSERT(mgr != NULL, "Failed to initialize timeout manager");

  /* Pop on empty manager */
  popped = tomgr_pop(mgr);
  TEST_ASSERT(popped == NULL, "Pop should return NULL on empty manager");

  /* Insert entries and pop */
  tomgr_insert(mgr, TO_ARP, 100, (void *)1);
  tomgr_insert(mgr, TO_ARP, 50, (void *)2);

  popped = tomgr_pop(mgr);
  TEST_ASSERT(popped != NULL, "Popped entry should not be NULL");
  TEST_ASSERT(popped->to == 50, "Popped entry timeout should be 50");
  TEST_ASSERT(popped->data == (void *)2, "Popped entry data should be 2");
  TEST_ASSERT(popped->heap_idx == TO_INVALID, 
      "Popped entry heap_idx should be invalid");

  popped = tomgr_pop(mgr);
  TEST_ASSERT(popped != NULL, "Popped entry should not be NULL");
  TEST_ASSERT(popped->to == 100, "Popped entry timeout should be 100");
  TEST_ASSERT(popped->data == (void *)1, "Popped entry data should be 1");
  TEST_ASSERT(popped->heap_idx == TO_INVALID, 
      "Popped entry heap_idx should be invalid");

  free(mgr);
  printf(ANSI_COLOR_GREEN "PASSED: tomgr_pop test" ANSI_COLOR_RESET "\n");
}

int main()
{
  printf("Running timeout manager tests...\n");

  test_tomgr_insert();
  test_tomgr_cancel();
  test_tomgr_peek();
  test_tomgr_pop();

  printf("All timeout manager tests passed!\n");
  return 0;
}

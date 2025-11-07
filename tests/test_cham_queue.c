#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "queue_fns.h"
#include "test_utils.h"

#define TEST_QUEUE_SIZE 4
#define TEST_ELEMENT_SIZE 64

static void test_queue_enqueue()
{
  printf(ANSI_COLOR_BLUE "Testing queue_enqueue..." ANSI_COLOR_RESET "\n");

  __u8 buffer[TEST_QUEUE_SIZE * TEST_ELEMENT_SIZE] = {0};
  struct equeue eq;

  equeue_init(&eq, TEST_QUEUE_SIZE, TEST_ELEMENT_SIZE, buffer, 0);

  for (__u8 i = 1; i <= TEST_QUEUE_SIZE; ++i) 
  {
    TEST_ASSERT(queue_enqueue(&eq, i) == 0, "Failed to enqueue element");
  }

  TEST_ASSERT(queue_enqueue(&eq, 99) == -1, "Queue should be full");

  printf(ANSI_COLOR_GREEN "PASSED: queue_enqueue test" ANSI_COLOR_RESET "\n");
}

static void test_queue_dequeue()
{
  printf(ANSI_COLOR_BLUE "Testing queue_dequeue..." ANSI_COLOR_RESET "\n");

  __u8 buffer[TEST_QUEUE_SIZE * TEST_ELEMENT_SIZE] = {0};
  struct equeue eq;
  struct dqueue dq;

  equeue_init(&eq, TEST_QUEUE_SIZE, TEST_ELEMENT_SIZE, buffer, 0);
  dqueue_init(&dq, TEST_QUEUE_SIZE, TEST_ELEMENT_SIZE, buffer, 0);

  for (__u8 i = 1; i <= TEST_QUEUE_SIZE; ++i) 
  {
    TEST_ASSERT(queue_enqueue(&eq, i) == 0, "Failed to enqueue element");
  }

  TEST_ASSERT(queue_enqueue(&eq, 99) == -1, "Queue should be full");

  for (__u8 i = 1; i <= TEST_QUEUE_SIZE; ++i) 
  {
    TEST_ASSERT(queue_dequeue(&dq) == 0, "Failed to dequeue element");
  }

  TEST_ASSERT(queue_dequeue(&dq) == -1, "Queue should be empty");

  printf(ANSI_COLOR_GREEN "PASSED: queue_enqueue and queue_dequeue test"
      ANSI_COLOR_RESET "\n");
}

static void test_queue_wraparound()
{
  printf(ANSI_COLOR_BLUE "Testing queue wraparound..." ANSI_COLOR_RESET "\n");

  __u8 buffer[TEST_QUEUE_SIZE * TEST_ELEMENT_SIZE] = {0};
  struct equeue eq;
  struct dqueue dq;

  equeue_init(&eq, TEST_QUEUE_SIZE, TEST_ELEMENT_SIZE, buffer, 0);
  dqueue_init(&dq, TEST_QUEUE_SIZE, TEST_ELEMENT_SIZE, buffer, 0);

  for (__u8 i = 1; i <= TEST_QUEUE_SIZE; ++i) 
  {
    TEST_ASSERT(queue_enqueue(&eq, i) == 0, "Failed to enqueue element");
  }

  TEST_ASSERT(queue_dequeue(&dq) == 0, "Failed to dequeue element");
  TEST_ASSERT(queue_dequeue(&dq) == 0, "Failed to dequeue element");

  TEST_ASSERT(queue_enqueue(&eq, 99) == 0, 
    "Failed to enqueue element after wraparound");
  TEST_ASSERT(queue_enqueue(&eq, 100) == 0, 
    "Failed to enqueue element after wraparound");

  for (__u8 i = 3; i <= TEST_QUEUE_SIZE; ++i) 
  {
    TEST_ASSERT(queue_dequeue(&dq) == 0, "Failed to dequeue element");
  }
  TEST_ASSERT(queue_dequeue(&dq) == 0, "Failed to dequeue element");
  TEST_ASSERT(queue_dequeue(&dq) == 0, "Failed to dequeue element");

  TEST_ASSERT(queue_dequeue(&dq) == -1, "Queue should be empty");

  printf(ANSI_COLOR_GREEN "PASSED: queue wraparound test" 
      ANSI_COLOR_RESET "\n");
}

static void test_queue_head_tail()
{
    printf(ANSI_COLOR_BLUE "Testing queue_head and queue_tail..." ANSI_COLOR_RESET "\n");

    __u8 buffer[TEST_QUEUE_SIZE * TEST_ELEMENT_SIZE] = {0};
    struct equeue eq;
    struct dqueue dq;

    equeue_init(&eq, TEST_QUEUE_SIZE, TEST_ELEMENT_SIZE, buffer, 0);
    dqueue_init(&dq, TEST_QUEUE_SIZE, TEST_ELEMENT_SIZE, buffer, 0);

    TEST_ASSERT(queue_head(&dq) == NULL, "Head should be NULL on empty queue");
    TEST_ASSERT(queue_tail(&eq) != NULL, "Tail should not be NULL on empty queue");

    TEST_ASSERT(queue_enqueue(&eq, 1) == 0, "Failed to enqueue element");
    TEST_ASSERT(queue_tail(&eq) != NULL, "Tail should not be NULL after enqueue");

    __u8 *head = queue_head(&dq);
    TEST_ASSERT(head != NULL, "Head should not be NULL after enqueue");
    TEST_ASSERT(*head == 1, "Head should point to the first element");

    TEST_ASSERT(queue_dequeue(&dq) == 0, "Failed to dequeue element");
    TEST_ASSERT(queue_head(&dq) == NULL, "Head should be NULL after dequeue");
    TEST_ASSERT(queue_tail(&eq) != NULL, "Tail should not be NULL after dequeue");

    printf(ANSI_COLOR_GREEN "PASSED: queue_head and queue_tail test" ANSI_COLOR_RESET "\n");
}

int main()
{
    printf("Running queue tests...\n");

    test_queue_enqueue();
    test_queue_dequeue();
    test_queue_wraparound();
    test_queue_head_tail();

    printf("All queue tests passed!\n");
    return 0;
}

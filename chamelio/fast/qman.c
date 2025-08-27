#include "qman.h"

void qman_init(struct qman *qman)
{
    qman->head = ID_INVALID;
    qman->tail = ID_INVALID;
    qman->free_head = 0;

    /* Link free entries */
    for (uint32_t i = 0; i < MAX_QMENTRIES - 1; ++i) 
    {
        qman->entries[i].next_entry = i + 1;
        qman->entries[i].id = ID_INVALID;
    }

    /* End of free list */
    qman->entries[MAX_QMENTRIES - 1].next_entry = ID_INVALID;
    qman->entries[MAX_QMENTRIES - 1].id = ID_INVALID;
}

int qman_add(struct qman *qman, struct qman_entry *entry)
{
    int prev, cur;
    struct qman_entry *new_entry;

    /* No space */
    int new_idx = qman->free_head;
    if (new_idx == ID_INVALID)
        return -1;

    new_entry = &qman->entries[new_idx];
    qman->free_head = new_entry->next_entry;

    /* Copy data from the provided entry */
    *new_entry = *entry;
    new_entry->next_entry = ID_INVALID;

    if (qman->head == ID_INVALID) 
    {
        qman->head = new_idx;
        qman->tail = new_idx;
        return 0;
    }

    prev = -1;
    cur = qman->head;
    while (cur != ID_INVALID && qman->entries[cur].priority > new_entry->priority)
    {
        prev = cur;
        cur = qman->entries[cur].next_entry;
    }

    if (prev == -1) 
    {
        /* Insert at head */
        new_entry->next_entry = qman->head;
        qman->head = new_idx;
    } 
    else 
    {
        /* Insert in middle or end */
        new_entry->next_entry = qman->entries[prev].next_entry;
        qman->entries[prev].next_entry = new_idx;
    }

    if (new_entry->next_entry == ID_INVALID)
        qman->tail = new_idx;

    return 0;
}

int qman_remove(struct qman *qman, uint32_t id)
{
    int prev, cur;

    prev = -1;
    cur = qman->head;
    while (cur != ID_INVALID) 
    {
        if (qman->entries[cur].id == id) 
        {
            if (prev == -1) 
            {
                qman->head = qman->entries[cur].next_entry;
            } 
            else 
            {
                qman->entries[prev].next_entry = qman->entries[cur].next_entry;
            }

            if (qman->tail == cur)
                qman->tail = prev;

            /* Return removed entry to free list */
            qman->entries[cur].id = ID_INVALID;
            qman->entries[cur].next_entry = qman->free_head;
            qman->free_head = cur;

            return 0;
        }
        
        prev = cur;
        cur = qman->entries[cur].next_entry;
    }

    return -1;
}

int qman_pop(struct qman *qman)
{
    int pop_idx;
    struct qman_entry *pop_entry;

    /* Queue is empty */
    if (qman->head == ID_INVALID)
        return -1;

    pop_idx = qman->head;
    pop_entry = &qman->entries[pop_idx];

    qman->head = pop_entry->next_entry;
    if (qman->head == ID_INVALID)
        qman->tail = ID_INVALID;

    /* Return popped entry to free list */
    pop_entry->id = ID_INVALID;
    pop_entry->next_entry = qman->free_head;
    qman->free_head = pop_idx;

    return 0;
}
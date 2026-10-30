/* test_queue.c — your test of src/queue.c, with ONE thread.
 *
 * The course wrote the includes, make(), the first test and main(). Write
 * the body of each other test function: the comment above it gives what to
 * check. Each check is an assert(). make test builds and runs this file.
 *
 * MAN PAGES. Each call of the C library and of Linux in this file, the
 * command that shows its manual page in a terminal, and the same page on
 * the web. The functions of the course have no man page: read them in
 * ipc.h, proc.h and syscall.h.
 *
 *   assert()                 man 3 assert
 *       https://man7.org/linux/man-pages/man3/assert.3.html
 *   errno, error names       man 3 errno
 *       https://man7.org/linux/man-pages/man3/errno.3.html
 *   memset()                 man 3 memset
 *       https://man7.org/linux/man-pages/man3/memset.3.html
 *   snprintf()               man 3 snprintf
 *       https://man7.org/linux/man-pages/man3/snprintf.3.html
 *   strcmp()                 man 3 strcmp
 *       https://man7.org/linux/man-pages/man3/strcmp.3.html
 */

#include "sentinel/ipc.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* A report with the contact id `id`. The queue keeps a copy, so a local
   variable is enough. */
static pending_t make(const char *id)
{
    pending_t p;
    memset(&p, 0, sizeof p);
    snprintf(p.c.id, sizeof p.c.id, "%s", id);
    return p;
}

/* EXAMPLE, written by the course. Three reports come out in the order in
   which they went in, and then the queue is empty. */
static void test_order(void)
{
    queue_t q;
    assert(queue_init(&q, 8) == 0);

    pending_t a = make("A");
    pending_t b = make("B");
    pending_t c = make("C");
    assert(queue_push(&q, &a) == 0);
    assert(queue_push(&q, &b) == 0);
    assert(queue_push(&q, &c) == 0);
    assert(queue_count(&q) == 3);

    pending_t out;
    assert(queue_pop(&q, &out) == 1);
    assert(strcmp(out.c.id, "A") == 0);
    assert(queue_pop(&q, &out) == 1);
    assert(strcmp(out.c.id, "B") == 0);
    assert(queue_pop(&q, &out) == 1);
    assert(strcmp(out.c.id, "C") == 0);
    assert(queue_pop(&q, &out) == 0);       /* empty now */

    queue_destroy(&q);
}

/* A full queue. Make a queue of 2 slots, and push 2 reports: each push
   gives 0. Push a third report: it gives -1, errno is ENOSPC, q.rejected is
   1, and queue_count(&q) is still 2. */
static void test_full(void)
{
}

/* The ring wraps. Make a queue of 2 slots. Push A, push B, pop (it gives
   A), push C. Then a pop gives B, and the next pop gives C: C went into
   slot 0 again. */
static void test_wrap(void)
{
}

/* NULL. queue_init(NULL, 4), queue_push(NULL, &p) and queue_pop(NULL, &out)
   each give -1 with errno EINVAL. queue_count(NULL) gives 0. */
static void test_null(void)
{
}

int main(void)
{
    test_order();
    test_full();
    test_wrap();
    test_null();
    printf("test_queue: OK\n");
    return 0;
}

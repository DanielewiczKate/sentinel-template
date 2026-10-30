# Lab 3 — `libipc`: worker threads, the shared queue and the track picture

This directory is Lab 3. You write its code in `src/` and `tests/`.

| Path | Who writes it |
|---|---|
| `include/sentinel/ipc.h` | The course. It states what each function does, in order, with each of its errors and its log lines. Do not change a declaration |
| `src/queue.c` | You: the five functions. Each one is a stub, with its steps in a comment |
| `src/pool.c` | The course wrote `free_pool()` and `start_failed()`. You: `pool_create_deadline`, `pool_stop` and `pool_destroy`. Each one is a stub, with its steps in a comment |
| `tests/test_queue.c` | The course wrote the first case. You: the three other cases, with one thread |
| `given/` | The course: the worker loop, `track_update` and the track picture, the readers of the queue, the instruments of the pool, association, validation, the cost of a report, and the demo. Read them; do not change them |
| `Makefile` | The course |

Lab 3 uses your Lab 1 and Lab 2 libraries. Do not add a lock, an atomic variable or a
semaphore: Lab 3 measures the code with no protection.

Read these papers:

1. [`docs/lab3/lab3_brief.pdf`](../../docs/lab3/lab3_brief.pdf), the Lab 3 Brief: the task,
   the ideas, the rules, the commands, the hand-in, the dates and the marks.
2. [`docs/lab3/prelab3.pdf`](../../docs/lab3/prelab3.pdf), the Lab 3 prelab: the questions to
   answer before the Lab 3 session.
3. [`docs/submit/submit.pdf`](../../docs/submit/submit.pdf), the Submission Guide: how to hand
   in the lab.

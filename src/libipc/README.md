# Lab 3 — `libipc`: worker threads, the shared queue and the track picture

This directory is Lab 3. You write its code in `src/` and `tests/`.

| Path | Who writes it |
|---|---|
| `include/sentinel/ipc.h` | The course. It states what each function does, in order, with each of its errors and its log lines. Do not change a declaration |
| `src/queue.c`, `src/track.c`, `src/pool.c` | You: the functions that `ipc.h` marks YOURS, each in the file that `ipc.h` names |
| `tests/test_queue.c`, `tests/test_track.c` | You: two tests, each with one thread |
| `given/` | The course: the readers of the queue, the table of the track picture, the instruments and the hooks of the pool, association, validation, the cost of a report, and the demo. Read them; do not change them |
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

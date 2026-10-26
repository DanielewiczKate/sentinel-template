# Lab 4 — `libsync`: a mutex, two semaphores and a lock order

This directory is Lab 4. You write its code in `src/` and `tests/`.

| Path | Who writes it |
|---|---|
| `include/sentinel/sync.h` | The course. It states what each function does, in order, with each of its errors. Do not change a declaration |
| `src/safetrack.c` | You: `safe_track_init`, `safe_track_destroy` and `safe_track_update`. Each one is a stub, with its steps in a comment |
| `src/sem_queue.c` | The course wrote `wait_for_slot()`, `is_shut_down()` and `pop_after_shutdown()`. You: `sem_queue_init`, `sem_queue_destroy`, `sem_queue_push` and `sem_queue_pop`. Each one is a stub, with its steps in a comment |
| `src/resources.c` | You: `resources_acquire_both` and `resources_release_both`. Each one is a stub, with its steps in a comment |
| `tests/test_sync.c` | The course wrote the first case. You: the three other cases |
| `given/` | The course: the worker pool, the rest of the track picture, of the queue and of the resources, and the picture divided into cells. Read them; do not change them |
| `Makefile` | The course |

Lab 4 uses your Lab 1, Lab 2 and Lab 3 libraries: your `queue.c` of Lab 3 is inside your
`sem_queue.c`. The final program, `build/sentinel`, is in `src/sentinel/`.

Read these papers:

1. [`docs/lab4/lab4_brief.pdf`](../../docs/lab4/lab4_brief.pdf), the Lab 4 Brief: the task,
   the ideas, the rules, the commands, the hand-in, the video, the dates and the marks.
2. [`docs/lab4/prelab4.pdf`](../../docs/lab4/prelab4.pdf), the Lab 4 prelab: the questions to
   answer before the Lab 4 session.
3. [`docs/submit/submit.pdf`](../../docs/submit/submit.pdf), the Submission Guide: how to hand
   in the lab.

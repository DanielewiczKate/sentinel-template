# Lab 2 — `libproc`: sensors as processes

This directory is Lab 2. You write its code in `src/` and `tests/`.

| Path | Who writes it |
|---|---|
| `include/sentinel/proc.h` | The course. It states what each function does, in order, with each of its errors and its log lines. Do not change a declaration |
| `src/sensor.c` | You: `sensor_spawn`, `sensor_stop` and `sensor_reap`. Each one is a stub, with its steps in a comment |
| `src/prio_channel.c` | The course wrote `open_failed()`. You: `prio_open`, `prio_close` and `prio_send`. Each one is a stub, with its steps in a comment |
| `src/sched.c` | The course wrote `struct sched` and `sched_next`. You: `sched_create`, `sched_destroy`, `sched_push` and `sched_pending`. Each one is a stub, with its steps in a comment |
| `tests/test_lab2.c` | The course wrote `read_lines()` and the first case. You: the three other cases |
| `tests/test_policies.c` | The course: the test of the three policies. It uses your `sched.c`. Do not change it |
| `given/` | The course: the channel reader, the loop of the centre, the class HIGH, the names of the policies, and the demo. Read them; do not change them |
| `Makefile` | The course |

Lab 2 also changes your Lab 1 code: add `O_CLOEXEC` to each `open()` of it, as Section 1 of
the Lab 2 Brief states.

Read these papers:

1. [`docs/lab2/lab2_brief.pdf`](../../docs/lab2/lab2_brief.pdf), the Lab 2 Brief: the task,
   the ideas, the rules, the commands, the hand-in, the dates and the marks.
2. [`docs/lab2/prelab2.pdf`](../../docs/lab2/prelab2.pdf), the Lab 2 prelab: the questions to
   answer before the Lab 2 session.
3. [`docs/submit/submit.pdf`](../../docs/submit/submit.pdf), the Submission Guide: how to hand
   in the lab.

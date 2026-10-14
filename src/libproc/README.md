# Lab 2 — `libproc`: sensors as processes

This directory is Lab 2. You write its code in `src/` and `tests/`.

| Path | Who writes it |
|---|---|
| `include/sentinel/proc.h` | The course. It states what each function does, in order, with each of its errors and its log lines. Do not change a declaration |
| `src/sensor.c`, `src/prio_channel.c`, `src/sched.c` | You: the functions that `proc.h` marks YOURS, each in the file that `proc.h` names |
| `tests/test_sched.c`, and `tests/test_sensor.c` or `tests/test_prio.c` | You: two tests |
| `given/` | The course: the channel reader, the loop of the centre, the class HIGH, the names of the policies, and the demo. Read them; do not change them |
| `Makefile` | The course |

Lab 2 also changes two lines of your Lab 1 code: see Section 1 of the Lab 2 Brief.

Read these papers:

1. [`docs/lab2/lab2_brief.pdf`](../../docs/lab2/lab2_brief.pdf), the Lab 2 Brief: the task,
   the ideas, the rules, the commands, the hand-in, the dates and the marks.
2. [`docs/lab2/prelab2.pdf`](../../docs/lab2/prelab2.pdf), the Lab 2 prelab: the questions to
   answer before the Lab 2 session.
3. [`docs/submit/submit.pdf`](../../docs/submit/submit.pdf), the Submission Guide: how to hand
   in the lab.

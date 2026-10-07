# Lab 1 — `libsyscall`: the operating system interface

This directory is Lab 1. You write its code here.

| Path | Who writes it |
|---|---|
| `include/sentinel/syscall.h` | The course. It declares the 15 functions, and it states what each one does and each of its errors. Do not change a declaration |
| `src/io.c`, `src/clock.c`, `src/log.c`, `src/proc.c` | You: the 15 functions, each in the file that `syscall.h` names |
| `tests/test_io.c`, `tests/test_clock.c`, `tests/test_log.c`, `tests/test_proc.c` | You: one test for each source file |
| `tests/demo_lab1.c` | You: the demonstration program |
| `Makefile` | You |

Read these papers:

1. [`docs/lab1/lab1_brief.pdf`](../../docs/lab1/lab1_brief.pdf), the Lab 1 Brief: the
   contract, the commands, the dates and the marks.
2. [`docs/lab1/lab1_guide.pdf`](../../docs/lab1/lab1_guide.pdf), the Lab 1 Guide: the ideas
   behind Lab 1.
3. [`docs/submit/submit.pdf`](../../docs/submit/submit.pdf), the Submission Guide: how to hand
   in the lab.

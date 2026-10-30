# SENTINEL

**Sensor Network for Tracking, Integration, and Event Logging**

SENTINEL is the software of a surveillance operations centre. Four sensors send contact
reports to the centre, and SENTINEL collects them, records them, sorts them by importance, and
keeps one correct picture of the contacts. You build it in this repository, in C, for Linux:
one library in each lab of ECE 476 Engineering System Software (Fall 2026, University of
Victoria).

**Start with the Setup Guide**, [`docs/setup/setup.pdf`](docs/setup/setup.pdf). It sets up your
machine and your own private copy of this repository.

## The documents

| Document | Where | What it holds |
|---|---|---|
| Lab Documentation | on Brightspace | The scenario: the detachment, the four sensors, the four parts of SENTINEL, and a user story |
| SENTINEL Lab Manual | [`docs/manual/sentinel_manual.pdf`](docs/manual/sentinel_manual.pdf) | The facts for all labs: the labs, the requirements, the repository, the given programs, the make targets |
| Setup Guide | [`docs/setup/setup.pdf`](docs/setup/setup.pdf) | Your machine, the tools, and your private copy of this repository |
| Submission Guide | [`docs/submit/submit.pdf`](docs/submit/submit.pdf) | The marks and the deadlines of each lab, the oral exam, the Lab 4 video, and how to hand in a lab |
| Lab 1 Brief | [`docs/lab1/lab1_brief.pdf`](docs/lab1/lab1_brief.pdf) | The contract of Lab 1 |
| Lab 1 Guide | [`docs/lab1/lab1_guide.pdf`](docs/lab1/lab1_guide.pdf) | The ideas behind Lab 1 |
| Lab 1 Primer | [`docs/lab1/lab1_primer.pdf`](docs/lab1/lab1_primer.pdf) | `make` and your Makefile, tests with `assert`, and `/proc` for `proc.c` |
| Lab 1 prelab | [`docs/lab1/prelab1.pdf`](docs/lab1/prelab1.pdf) | The questions to answer before the Lab 1 session |
| Lab 1 slides | [`docs/lab1/lab1_slides.pdf`](docs/lab1/lab1_slides.pdf) | The slides of the Lab 1 session: all four labs, the marks, the dates, and the start of Lab 1 |
| Lab 2 Brief | [`docs/lab2/lab2_brief.pdf`](docs/lab2/lab2_brief.pdf) | The one paper of Lab 2: its contract, its ideas and its hand-in |
| Lab 2 prelab | [`docs/lab2/prelab2.pdf`](docs/lab2/prelab2.pdf) | The questions to answer before the Lab 2 session |
| Lab 2 slides | [`docs/lab2/lab2_slides.pdf`](docs/lab2/lab2_slides.pdf) | The slides of the Lab 2 session: the task, the ideas, the commands and the hand-in |
| Lab 3 Brief | [`docs/lab3/lab3_brief.pdf`](docs/lab3/lab3_brief.pdf) | The one paper of Lab 3: its contract, its ideas and its hand-in |
| Lab 3 prelab | [`docs/lab3/prelab3.pdf`](docs/lab3/prelab3.pdf) | The questions to answer before the Lab 3 session |
| Lab 3 slides | [`docs/lab3/lab3_slides.pdf`](docs/lab3/lab3_slides.pdf) | The slides of the Lab 3 session: the task, the ideas, the race, the commands and the hand-in |
| Lab 4 Brief | [`docs/lab4/lab4_brief.pdf`](docs/lab4/lab4_brief.pdf) | The one paper of Lab 4: its contract, its ideas, its hand-in and the video |
| Lab 4 prelab | [`docs/lab4/prelab4.pdf`](docs/lab4/prelab4.pdf) | The questions to answer before the Lab 4 session |
| Lab 4 slides | [`docs/lab4/lab4_slides.pdf`](docs/lab4/lab4_slides.pdf) | The slides of the Lab 4 session: the task, the ideas, the commands, the hand-in and the video |
| The page | [`web/README.md`](web/README.md) | The operations centre display |
| Lab 1 directory | [`src/libsyscall/README.md`](src/libsyscall/README.md) | The files that you write for Lab 1 |
| Lab 2 directory | [`src/libproc/README.md`](src/libproc/README.md) | The files that you write for Lab 2, and the files that the course gives |
| Lab 3 directory | [`src/libipc/README.md`](src/libipc/README.md) | The files that you write for Lab 3, and the files that the course gives |
| Lab 4 directory | [`src/libsync/README.md`](src/libsync/README.md) | The files that you write for Lab 4, and the files that the course gives |
| The final program | [`src/sentinel/README.md`](src/sentinel/README.md) | The program of all four labs, which Lab 4 runs |

When the course releases a later lab, its papers arrive in `docs/<lab>/`, with
`git pull upstream main`.

## Credit

The project brief and the scenario come from the ECE 476 Lab Documentation.

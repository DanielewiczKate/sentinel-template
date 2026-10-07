# The SENTINEL page

The page shows the operations centre from the files that your programs write. It only reads
files, and it changes nothing in your code or your run.

Section 5 of the course Lab Documentation[^labdoc] says that the dashboard screens are not a
course requirement.
The page is there to help you see what your code does.

This file is the reference for the page. The SENTINEL Lab Manual,
`docs/manual/sentinel_manual.pdf`, names it, and it does not repeat it.

---

## Run it

From the top of the repository:

```sh
make lab1      # write a run into build/live/
make watch     # serve the page
```

Open <http://localhost:8080/>. Press Ctrl-C to stop the server.

**A page that you open as a file does not work.** The browser does not let a page read a local
file, so the page cannot read your mission log. For this reason, `make watch` starts a small
server.

To open one view directly, add `?view=` to the address: <http://localhost:8080/?view=log>.

---

## Over SSH

When you use SSH, the browser is on your own computer, and `localhost` is your own computer.
`make watch` prints the two ways to connect:

- Forward the port. Run this on your own computer, then open <http://localhost:8080/>:

  ```sh
  ssh -L 8080:localhost:8080 <user>@<host>
  ```

- Or publish the page on the network. Everyone on that network can then read it:

  ```sh
  make watch BIND=0.0.0.0
  ```

---

## Options

| Command | What it does |
|---|---|
| `make watch PORT=8081` | use a different port |
| `make watch SRC=<directory>` | show the run in a different directory |
| `make lab1 PROFILE=dirty` | a feed with damaged lines |
| `make bench-sim` | a simulated bench board on this computer, for the Bench view. Ctrl-C stops it |
| `make bench-sim SCENARIO=target` | the same, with another scenario: `empty`, `target`, `strait`, `flood` or `demo` |
| `make bench-health BENCH=tcp:<address>:4760` | the TA's bench, for the Bench view. Ctrl-C stops it |

---

## What each lab adds, and what caused it

Every lab makes something new appear on the page. The point of this table is that you should
be able to look at the page and say which of your own changes caused each line of it.

| Lab | What appears that was not there before | The change that caused it |
|---|---|---|
| **1** | **Mission log** fills with every line your `log_writef` wrote. **Contacts** lists every report. **Processes** shows one card: your own process. | Your library can open a file, read whole lines, stamp the time, and append to the log. The page reads the pid, the ppid, the name and the descriptor count out of the one log line your demo writes. |
| **2** | **Processes** gains one row for each of the four sensors, each with its own pid. **Contacts becomes empty.** | Your centre starts four sensor processes with `fork` and `exec`, and logs one line for each. The reports now arrive through a pipe and a FIFO, so there is no feed file left to read. |
| **3** | **Dashboard** gains the worker rows, the queue depth, the share of a processor and the core strip. **Situation** gains the track picture. The panel says `protected: false`, and **the numbers disagree with each other.** | Eight worker threads share one queue and one track picture, with no lock. The disagreement is not a bug in the page: it is what unprotected shared data looks like, and Lab 4 repairs it. |
| **4** | Every panel becomes **exact**. **Where every report went** appears, and it balances. **Situation** lights all seven states. An unknown contact raises an amber alert and draws a trail behind it. | A mutex for each cell of the picture, and a semaphore on the queue, make the shared numbers correct, so a snapshot can be trusted. Every report that did not reach the picture now carries a named cause. |

Two things to know, because you will meet both:

- **Labs 1 and 2 never write `live.json`.** That is why their Dashboard keeps a placeholder
  where the worker and queue panels go. Nothing is broken.
- **Labs 3 and 4 write it only for a timed run.** The brief of those labs gives the command
  when you reach them.

The page reads the files again every second, so a view fills as soon as the run that writes it
finishes. To watch a view fill DURING a run, your program must still be writing while you
look. Lab 1 does that with `demo_lab1 --follow`, and Section 5 of the Lab 1 Guide,
`docs/lab1/lab1_guide.pdf`, gives the three commands.

---

## The views

| View | It reads | It shows |
|---|---|---|
| Dashboard | `missions.log`, `feed.txt` | the state of the centre, the map, the counts, R1 to R6[^req], the alerts, the end of the log |
| Situation | `missions.log`, `feed.txt` | the seven states of Section 5 of the Lab Documentation[^labdoc], and which of them your run shows |
| Contacts | `feed.txt` | every contact report |
| Processes | `missions.log` | the processes that your code wrote to the log |
| Mission log | `missions.log` | every line of the log, by level |
| Bench | `bench.json` | the map box: the board, each sensor's reading, the plots, the satellite's store, the wiring |

A panel with no data tells you which file it waits for.

---

## The files

`make watch` makes `web/live`, a link to your run directory (`build/live/` by default).

```
web/live/
  missions.log     your mission log
  feed.txt         the contact reports of the run
  run.json         what a run reported about itself (a later lab)
  tracks.json      the track picture: one entry for each target (a later lab)
  bench.json       the state of the bench board (make bench-sim or make bench-health)
```

The page understands the log lines that the brief of each lab defines[^brief]. A line in another format
still appears, marked UNPARSED.

The Bench view watches the time inside `bench.json`, not the clock of your browser. When the
file stops changing for 3 seconds, the program that writes it has stopped, and the view says so.

---

[^labdoc]: The ECE 476 Lab Documentation for SENTINEL. The course gives it to you. Its Section 5
    is a user story of the operations centre, and its Figures 3 to 9 show the dashboard screens.
[^req]: The six requirements of SENTINEL, R1 to R6. The SENTINEL Lab Manual, Section 1.2,
    `docs/manual/sentinel_manual.pdf`, lists them.
[^brief]: For Lab 1: the Lab 1 Brief, Section 3.3, `docs/lab1/lab1_brief.pdf`.

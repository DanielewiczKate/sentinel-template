import { el, text, chip, pending } from "../dom.js";
import { sensorRows, sensorProcessCount, contactsForDisplay, elapsedText } from "../model.js";
import { clockText } from "../parse.js";
import { renderMap, SENSOR_COLOUR } from "./map.js";

const LIVE_RUN = null;

const LEVEL_CLASS = {
    SYSTEM: "info", INFO: "idle", PENDING: "warn",
    PRIORITY: "warn", ALERT: "bad", TRACK: "ok",
};

function tile(label, value, cls, note) {
    return el("div", { class: "tile " + (cls || "idle") },
        el("h4", {}, text(label)),
        el("strong", {}, text(value)),
        el("small", {}, text(note || "")));
}

function runLabel(run) {
    if (!run) return null;
    if (run.feed) return chip("info", "feed " + run.feed);
    const bits = [];
    if (run.part) bits.push("Part " + run.part);
    if (run.workers) bits.push(run.workers + " workers");
    if (run.policy) bits.push("policy " + run.policy);
    return bits.length ? chip("info", bits.join(", ")) : null;
}

function count(label, value, cls) {
    return el("div", { class: "count" },
        el("dt", {}, text(label)),
        el("dd", { class: cls || null }, text(value === null ? "—" : String(value))));
}

function requirementsFromRun(run) {
    const box = el("div", { class: "reqs" });

    run.requirements.forEach((r) => {
        const cls = { pass: "ok", fail: "bad", part: "warn", wait: "idle" }[r.state]
                  || "idle";
        const label = { pass: "PASS", fail: "FAIL", part: "PARTIAL", wait: "n/a" }[r.state]
                  || r.state;
        box.append(el("div", { class: "req-row" },
            el("span", { class: "rid" }, text(r.id)),
            el("span", { class: "rwhat" }, text(r.what)),
            el("span", { class: "rhow" }, text(r.how || "")),
            chip("info", "measured"),
            chip(cls, label)));
    });

    return box;
}

function requirements(model) {
    const procs = sensorProcessCount(model);
    const up = sensorRows(model).filter((s) => s.pid !== null && s.up !== false).length;
    const hasLog = model.events.length > 0;
    const t = model.totals;

    const sat = sensorRows(model).find((s) => s.name === "SAT") || null;

    const w = model.priorityWaits;
    const meanWait = w.length ? w.reduce((a, b) => a + b, 0) / w.length : null;

    const rows = [
        { id: "R1", what: "Four sensors, one may stop",
          state: procs === 0 ? "wait" : (procs >= 4 ? "pass" : "part"),
          how: procs === 0 ? "waiting for Part 2"
               : (model.finished
                  ? procs + " sensor processes ran, then stopped with the run"
                  : up + " of " + procs + " sensor processes running"),
          kind: "measured" },

        { id: "R2", what: "Priority overtakes routine",
          state: model.priority > 0 ? "pass" : "wait",
          how: model.priority > 0
               ? model.priority + " priority contacts" +
                 (meanWait !== null ? ", mean wait " + meanWait.toFixed(1) + " ms" : "")
               : "waiting for a priority contact", kind: "measured" },

        { id: "R3", what: "Absorbs 2000 a second",
          state: sat && sat.pid !== null && sat.count > 0 ? "pass" : "wait",
          how: !sat || sat.pid === null ? "waiting for Part 2"
               : (sat.burst ? "SAT sent a burst of " + sat.burst
                            : "SAT at " + (sat.rate || "?") + "/s, " +
                              sat.count + " reports in the log"),
          kind: "measured" },

        { id: "R4", what: "Shared data stays exact",
          state: !t ? "wait" : (t.processed === t.read ? "pass" : "fail"),
          how: !t ? "waiting for Part 3"
               : t.processed + " processed of " + t.read + " read", kind: "measured" },

        { id: "R5", what: "Never freezes",
          state: t ? "pass" : "wait",
          how: t ? "the run completed" : "waiting for Part 4", kind: "asserted" },

        { id: "R6", what: "Every action timestamped",
          state: hasLog ? "pass" : "wait",
          how: hasLog ? model.events.length + " lines in the mission log"
               : "waiting for the mission log", kind: hasLog ? "measured" : "asserted" },
    ];

    const box = el("div", { class: "reqs" });
    rows.forEach((r) => {
        const cls = { pass: "ok", fail: "bad", part: "warn", wait: "idle" }[r.state];
        const label = { pass: "PASS", fail: "FAIL", part: "PARTIAL", wait: "waiting" }[r.state];
        box.append(el("div", { class: "req-row" },
            el("span", { class: "rid" }, text(r.id)),
            el("span", { class: "rwhat" }, text(r.what)),
            el("span", { class: "rhow" }, text(r.how)),
            r.state === "pass"
                ? chip(r.kind === "measured" ? "info" : "idle", r.kind)
                : el("span", {}),
            chip(cls, label)));
    });
    return box;
}

function liveState(live, liveSame) {
    if (!live) return { on: false, label: "no live run" };
    if (!live.running) return { on: false, label: live.stalled ? "stalled" : "run ended" };
    if (liveSame >= 3) return { on: false, label: "no update for 3 polls" };
    return { on: true, label: "LIVE" };
}

function coreShares(live, prev) {
    if (!live || !prev || !live.workers || !prev.workers) return null;
    const dt = live.written - prev.written;
    if (!(dt > 0)) return null;

    const wasCpu  = new Map(prev.workers.map((w) => [w.index, w.cpu_ns || 0]));
    const wasBusy = new Map(prev.workers.map((w) => [w.index, w.busy_ns || 0]));
    const out = new Map();
    const held = new Map();
    let total = 0, heldTotal = 0;
    live.workers.forEach((w) => {
        const before = wasCpu.get(w.index);
        if (before === undefined) return;
        const share = ((w.cpu_ns || 0) - before) / dt;
        if (share < 0) return;
        out.set(w.index, share);
        total += share;

        const b = wasBusy.get(w.index);
        if (b !== undefined) {
            const hs = ((w.busy_ns || 0) - b) / dt;
            if (hs >= 0) {
                held.set(w.index, hs);
                heldTotal += hs;
            }
        }
    });
    return { per: out, held: held, total: total, heldTotal: heldTotal, dt: dt };
}

function mostAtOnce(shown) {
    const ev = [];
    shown.forEach((x) => {
        ev.push({ t: x.sp.s, d: +1 });
        ev.push({ t: x.sp.e, d: -1 });
    });
    ev.sort((a, b) => (a.t - b.t) || (a.d - b.d));
    let now = 0, most = 0;
    ev.forEach((e) => { now += e.d; if (now > most) most = now; });
    return most;
}

function coreStrip(live) {
    const rows = (live && live.workers) || [];

    const MIN_FRAC = 0.025;
    const MIN_SHOWN = 3;
    const MAX_SHOWN = 48;

    const all = [];
    rows.forEach((w, lane) => {
        (w.spans || []).forEach((sp) => all.push({ sp: sp, lane: lane }));
    });
    if (!all.length) return null;

    all.sort((a, b) => a.sp.e - b.sp.e);

    const median = (xs) => {
        const v = xs.map((x) => x.sp.e - x.sp.s).sort((a, b) => a - b);
        return v.length ? v[v.length >> 1] : 0;
    };
    const widthOf = (xs) => {
        let a = Infinity, b = -Infinity;
        xs.forEach((x) => {
            if (x.sp.s < a) a = x.sp.s;
            if (x.sp.e > b) b = x.sp.e;
        });
        return b - a;
    };

    let shown = all.slice(-Math.min(MIN_SHOWN, all.length));
    for (let n = shown.length + 1; n <= Math.min(MAX_SHOWN, all.length); n++) {
        const wider = all.slice(-n);
        const w = widthOf(wider);
        if (!(w > 0)) continue;
        if (median(wider) / w < MIN_FRAC) break;
        shown = wider;
    }

    let lo = Infinity, hi = -Infinity;
    shown.forEach((x) => {
        if (x.sp.s < lo) lo = x.sp.s;
        if (x.sp.e > hi) hi = x.sp.e;
    });
    const span = hi - lo;
    if (!(span > 0)) return null;

    const W = 1000, LANE = 16, PAD = 2;
    const H = rows.length * LANE + PAD;
    const NS = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 " + W + " " + H);
    svg.setAttribute("class", "corestrip");
    svg.setAttribute("preserveAspectRatio", "none");
    svg.setAttribute("role", "img");
    svg.setAttribute("aria-label",
        "The most recent reports, one lane per worker, on one time axis. A "
        + "solid bar is processor time; a faded bar is time spent waiting for "
        + "the processor.");

    const mk = (name, attrs) => {
        const n = document.createElementNS(NS, name);
        Object.keys(attrs).forEach((k) => n.setAttribute(k, attrs[k]));
        return n;
    };

    rows.forEach((w, lane) => {
        svg.append(mk("rect", {
            x: 0, y: lane * LANE + PAD, width: W, height: LANE - 4, class: "lane",
        }));
    });

    shown.forEach((x) => {
        const sp = x.sp;
        const y  = x.lane * LANE + PAD;
        const x0 = (sp.s - lo) / span * W;
        const x1 = (sp.e - lo) / span * W;
        const wide = Math.max(x1 - x0, 3);
        const frac = Math.min(sp.cpu / Math.max(sp.e - sp.s, 1), 1);
        const cpuW = Math.max(wide * frac, 1.5);

        svg.append(mk("rect", {
            x: x0, y: y, width: wide, height: LANE - 4, class: "sp-wait",
        }));
        svg.append(mk("rect", {
            x: x0, y: y, width: cpuW, height: LANE - 4,
            class: (sp.f === "P" || sp.f === "H") ? "sp-p" : "sp-r",
        }));
    });

    let wall = 0, cpu = 0;
    shown.forEach((x) => { wall += x.sp.e - x.sp.s; cpu += x.sp.cpu; });

    return {
        svg: svg, ms: span / 1e6, lanes: rows.length,
        shown: shown.length,
        atOnce: mostAtOnce(shown),
        waited: wall > 0 ? 1 - cpu / wall : 0,
    };
}

function workerPanel(live, liveSame, prev) {
    const card = el("section", { class: "card" });
    const ls = liveState(live, liveSame);
    const cores = live && live.cpus ? live.cpus : null;

    const head = el("div", { class: "card-h" },
        el("h3", {}, text("Concurrent processing")),
        el("span", { style: "flex:1" }));
    if (cores) {
        head.append(chip(cores === 1 ? "warn" : "info",
            cores === 1 ? "1 processor" : cores + " processors"));
    }
    if (live && live.protected === false) {
        head.append(chip("bad", "unprotected"));
    }
    head.append(chip(ls.on ? "ok" : "idle", ls.label));
    card.append(head);

    if (!live) {
        card.append(el("div", { class: "card-b" }, pending(
            "No live run",
            "The worker pool publishes what each worker is doing only while it " +
            "runs. Lab 3 shows the same panel with the unprotected pool, and its " +
            "numbers disagree.",
            LIVE_RUN)));
        return card;
    }

    const rows = live.workers || [];
    const shares = coreShares(live, prev);
    const body = el("div", { class: "card-b workers" });

    if (shares && cores) {
        const used = shares.total;
        const cls = used > cores * 0.9 ? "bad" : (used > cores * 0.6 ? "warn" : "ok");
        body.append(el("div", { class: "coreuse " + cls },
            el("strong", {}, text(used.toFixed(2))),
            el("span", {}, text(" of " + cores + " processor"
                + (cores === 1 ? "" : "s") + " used by the workers")),
            el("small", {}, text(cores === 1
                ? "Eight workers or eighty, this cannot pass 1.00. That is "
                  + "concurrency: many reports in progress, one executing."
    : "Above 1.00 means reports really are being handled at the same instant."))));

        if (used < 0.10 && live.running !== false) {
            body.append(el("div", { class: "hint" },
                el("strong", {}, text("This pool is idle, not stuck.")),
                el("span", {}, text(
                    "The workers were given " + (used * 100).toFixed(1)
                    + "% of a processor, so a worker is holding a report too "
                    + "rarely for anything here to show. Nothing is wrong "
                    + "with the pool: there is nothing for it to do. Load it "
                    + "and every figure on this panel moves.")),
                el("code", {}, text(LIVE_RUN))));
        }
    }

    const finished = live.running === false;

    rows.forEach((w) => {
        const share = shares ? shares.per.get(w.index) : undefined;
        const pct = share === undefined ? null : Math.round(share * 100);
        const holding = shares ? shares.held.get(w.index) : undefined;
        const heldPct = holding === undefined ? null : Math.round(holding * 100);

        let state;
        if (finished) {
            state = chip("idle", "STOPPED");
        } else if (heldPct !== null) {
            state = heldPct > 0
                ? chip("ok", "WORKING")
                : chip("idle", live.protected === false
                    ? "POLLING FOR WORK" : "WAITING FOR WORK");
        } else {
            state = w.busy === true
                ? chip("ok", "WORKING")
                : chip("idle", live.protected === false
                    ? "POLLING FOR WORK" : "WAITING FOR WORK");
        }

        const bar = el("div", { class: "wbar" },
            el("u", { style: "width:" + (heldPct === null ? 0 : Math.min(heldPct, 100)) + "%" }),
            el("i", { style: "width:" + (pct === null ? 0 : Math.min(pct, 100)) + "%" }));

        body.append(el("div", { class: "wrow" },
            el("span", { class: "wname" }, text("Worker-" + (w.index + 1))),
            state,
            el("span", { class: "wlast" },
                text(w.last_id ? "last " + w.last_id : (live.protected === false
                    ? "not published by Lab 3" : "no report yet"))),
            w.last_flag === "H" ? chip("bad", "HIGH")
                : w.last_flag === "P" ? chip("warn", "PRIORITY")
                : (w.last_flag === "R" ? chip("info", "routine") : el("span", {})),
            el("span", { class: "wcount" },
                text(heldPct === null
                    ? (pct === null ? "—" : pct + "% of a core")
                    : "held " + heldPct + "%, given " + pct + "% of a core")),
            bar));
    });
    if (!rows.length) {
        body.append(el("p", { class: "note" }, text("The run published no workers.")));
    }

    const strip = coreStrip(live);
    if (strip) {
        body.append(el("h4", { class: "striph" }, text("Who held the processor")));
        body.append(el("div", { class: "stripwrap" }, strip.svg));
        const waitedPct = Math.round(strip.waited * 100);

        const verdict = strip.atOnce > 1
            ? (live.cpus === 1
                ? "At one point " + strip.atOnce + " of these reports were in "
                  + "progress at the same instant, on a machine that can "
                  + "execute one of them at a time. That is concurrency, and "
                  + "it is what those overlapping bars are."
                : "At one point " + strip.atOnce + " were in progress at the "
                  + "same instant, and with " + live.cpus + " processors some "
                  + "of them really were simultaneous.")
            : "No two of these reports were in progress at the same instant, "
              + "so this window shows no concurrency at all. A worker took a "
              + "report and finished it before the next one arrived. That is "
              + "what an idle pool looks like, and eight workers make no "
              + "difference to it.";

        body.append(el("p", { class: "note" }, text(
            "The last " + strip.shown + " reports, over " + strip.ms.toFixed(0)
            + " ms, one lane for each of the " + strip.lanes + " workers. A bar "
            + "runs from when a worker took a report to when it finished, and "
            + "the solid part is the processor time it was given; the faded "
            + "part is time the worker spent waiting its turn, which across "
            + "these reports was " + waitedPct + "% of the time. "
            + verdict
            + " How many reports are drawn is chosen so that a typical bar is "
            + "wide enough to see: a fixed count cannot fit both a busy pool "
            + "and an idle one.")));
    }

    const foot = el("dl", { class: "counts" },
        count("busy now", live.busy === undefined ? "—" : live.busy + " of " + rows.length,
              live.busy ? "ok" : null),
        count("queue", live.queue, live.queue > 0 ? "warn" : null),
        count("processed", live.processed),
        count("priority", live.priority, live.priority ? "warn" : null),
        count("rejected", live.rejected, live.rejected ? "bad" : null));
    body.append(foot);

    body.append(el("p", { class: "note" }, text(
        live.protected === false
        ? "Lab 3. These numbers are read from the workers with no lock, while " +
          "they write them, so they can be wrong: " + live.processed +
          " processed of " + live.pushed + " queued. That is requirement R4, " +
          "and Part 4 is the answer." +
          (live.stalled ? " This run STALLED: the queue lost count, so the " +
           "workers never finished and the track picture was not written." : "")
        : (finished
            ? "This run has ended and its worker threads are gone, so the " +
              "rows say STOPPED rather than describing threads that no " +
              "longer exist. The totals are the run's final ones."
            : "A row says how much of the LAST INTERVAL between two samples " +
              "the worker held a report, and how much processor it was given " +
              "while holding it. Neither is a single instant: on one " +
              "processor a worker can hold a report for a whole second and " +
              "be given a tenth of it."))));

    card.append(body);
    return card;
}

const LEDGER_ROWS = [
    ["rejected",     "malformed, rejected",         "bad"],
    ["lost_sched",   "refused by the scheduler",    "bad"],
    ["dropped_full", "dropped, the queue was full", "bad"],
    ["late",         "dropped, past the deadline",  "warn"],
    ["refused",      "refused as impossible",       "warn"],
    ["queued_now",   "still waiting in the queue",  "info"],
];

function ledgerPanel(live) {
    const lg = live && live.ledger;
    const card = el("section", { class: "card" });
    const head = el("div", { class: "card-h" },
        el("h3", {}, text("Where every report went")),
        el("span", { style: "flex:1" }));
    if (lg) {
        head.append(lg.balanced ? chip("ok", "BALANCED")
                                : chip("bad", "DOES NOT BALANCE"));
    }
    card.append(head);

    if (!lg) {
        card.append(el("div", { class: "card-b" }, pending(
            "No live run",
            "The centre counts every report it could not apply, and why. The "
            + "ledger is published only while it runs.",
            LIVE_RUN)));
        return card;
    }

    const body = el("div", { class: "card-b" });
    const tbl = el("table", { class: "ledger" });
    tbl.append(el("tr", { class: "lg-head" },
        el("th", {}, text("read from the sensors")),
        el("th", {}, text(String(lg.read)))));

    LEDGER_ROWS.forEach(([key, label, kind]) => {
        const v = lg[key];
        if (v === undefined) return;
        tbl.append(el("tr", { class: v ? "lg-" + kind : "lg-zero" },
            el("td", {}, text(label)),
            el("td", {}, text(String(v)))));
    });

    tbl.append(el("tr", { class: "lg-kept" },
        el("td", {}, text("reached the picture")),
        el("td", {}, text(String(lg.counted)))));
    tbl.append(el("tr", { class: lg.balanced ? "lg-sum ok" : "lg-sum bad" },
        el("td", {}, text("accounted for")),
        el("td", {}, text(String(lg.accounted)))));
    body.append(tbl);

    if (live.waits !== undefined) {
        body.append(el("dl", { class: "counts" },
            count("queue slots", live.queue_cap),
            count("deepest it got", live.queue_peak,
                  live.queue_peak >= live.queue_cap ? "bad" : null),
            count("the centre waited", live.waits + " times", live.waits ? "warn" : null),
            count("for", (live.wait_ms || 0).toFixed(1) + " ms",
                  live.wait_ms > 0 ? "warn" : null)));
    }

    body.append(el("p", { class: "note" }, text(
        lg.balanced
        ? "Every report is accounted for. A queue that fills does not lose "
          + "them here: the centre waits instead, and the wait is counted "
          + "above. The semaphore does not create capacity; it decides who "
          + "absorbs the shortage."
        : "The two sides disagree by " + Math.abs(lg.read - lg.accounted)
          + " report(s). Nothing was written to a file that is missing: the "
          + "COUNTERS are wrong, because several workers add to them at once "
          + "with nothing to stop them. That is requirement R4, and it is "
          + "what Part 4 repairs.")));

    card.append(body);
    return card;
}

function comparePanel(cmp) {
    if (!cmp || !Array.isArray(cmp.runs) || cmp.runs.length !== 2) return null;

    const [one, many] = cmp.runs;
    const card = el("section", { class: "card" },
        el("div", { class: "card-h" },
            el("h3", {}, text("The same work, one processor and all of them")),
            el("span", { style: "flex:1" }),
            chip("info", cmp.same.workers + " workers, " +
                 (cmp.same.ms / 1000) + " s each")));

    const body = el("div", { class: "card-b" });
    const tbl = el("table", { class: "ledger compare" });

    const head = el("tr", { class: "lg-head" },
        el("th", {}, text("")),
        el("th", {}, text((one.cpus_allowed || 1) + " processor")),
        el("th", {}, text((many.cpus_allowed || "all") + " processors")));
    tbl.append(head);

    const row = (label, a, b, cls) => tbl.append(el("tr", { class: cls || null },
        el("td", {}, text(label)),
        el("td", {}, text(a)),
        el("td", {}, text(b))));

    const cores = (r) => r.cores_used === null || r.cores_used === undefined
        ? "—" : r.cores_used.toFixed(2);
    const kept = (r) => (r.read && r.processed !== null && r.processed !== undefined)
        ? (r.read - r.processed) : null;

    row("processor used by the workers", cores(one), cores(many), "lg-kept");
    row("reports read", String(one.read), String(many.read));
    row("reports processed", String(one.processed), String(many.processed));

    const lostOne = kept(one), lostMany = kept(many);
    if (lostOne !== null && lostMany !== null) {
        row("still waiting when time ran out", String(lostOne), String(lostMany),
            lostOne > lostMany ? "cmp-worse-one" : null);
    }
    row("deepest the queue got", String(one.queue_peak), String(many.queue_peak));
    if (one.waits !== undefined && one.waits !== null) {
        row("the centre waited", one.waits + " times, " +
            (one.wait_ms || 0).toFixed(0) + " ms",
            many.waits + " times, " + (many.wait_ms || 0).toFixed(0) + " ms");
    }
    row("longest pause (R5)", one.r5 || "—", many.r5 || "—");
    body.append(tbl);

    body.append(el("p", { class: "note" }, text(
        "Same binary, same seed \"" + cmp.same.seed + "\", same " +
        (cmp.same.ms / 1000) + " s, same " + cmp.same.queue + "-slot queue, " +
        "same " + cmp.same.service_us + " us of processor for each report" +
        (cmp.same.sat_pass === "0"
            ? ", and the satellite sent with no gap between its passes. "
            : ". ") +
        "One thing changed." +
        (one.cores_used !== null && many.cores_used !== null && many.cores_used > 1
            ? " The second run passed 1.00, which only more than one processor " +
              "can do: those reports really were handled at the same instant."
            : "") +
        " The load is heavier than the sensors' own rates on purpose: it has " +
        "to exceed ONE processor, or there is nothing for a second processor " +
        "to do and nothing to compare. At the sensors' own rates both runs " +
        "measure the same thing." +
        (one.cores_used !== null && one.cores_used < 0.95
            ? " The one-processor run gave its workers " +
              one.cores_used.toFixed(2) + " of that processor and not all of " +
              "it, because the centre loop that reads the sensors is a thread " +
              "on the same processor and takes its turn too."
            : ""))));

    card.append(body);
    return card;
}

function sensorPanel(live, model) {
    const rows = live && live.sensors ? live.sensors : null;
    const card = el("section", { class: "card" },
        el("div", { class: "card-h" }, el("h3", {}, text("Sensor status"))));
    const body = el("div", { class: "card-b" });

    if (rows) {
        rows.forEach((s) => {
            body.append(el("div", { class: "srow" },
                el("span", { class: "sname" }, text(s.name)),
                el("span", { class: "scount" }, text(s.reports + " reports")),
                s.restarts ? chip("warn", "restarted " + s.restarts) : el("span", {}),
                chip(s.running ? "ok" : "idle", s.running ? "reporting" : "stopped")));
        });
    } else {
        const list = sensorRows(model);
        if (!list.length) {
            body.append(el("p", { class: "note" }, text("No sensor has reported yet.")));
        }
        list.forEach((s) => {
            body.append(el("div", { class: "srow" },
                el("span", { class: "sname" }, text(s.name)),
                el("span", { class: "scount" }, text(s.count + " reports")),
                el("span", {}),
                chip(s.pid !== null && s.up !== false ? "ok" : "idle",
                     s.pid === null ? "no process" : (s.up === false ? "stopped" : "reporting"))));
        });
    }
    card.append(body);
    return card;
}

function unknownIds(model, live) {
    const set = new Set(Object.keys(model.unknown || {}));
    if (live && Array.isArray(live.unknown)) {
        live.unknown.forEach((u) => set.add(u.id));
    }
    return set;
}

function highIds(model, live) {
    const map = new Map();
    Object.values(model.highContacts || {}).forEach((h) => map.set(h.id, h.region));
    if (live && Array.isArray(live.unknown)) {
        live.unknown.forEach((u) => { if (u.region) map.set(u.id, u.region); });
    }
    return map;
}

export function renderDashboard(root, model, run, tracks, live, liveSame, livePrev,
                               trails, compare) {
    root.replaceChildren();

    if (!model.events.length && !model.contacts.length) {
        root.append(pending(
            "Nothing is running yet",
            "The dashboard fills from your own run. It reads web/live/missions.log " +
            "and web/live/feed.txt. Build Part 1 and send a feed through it.",
            "make lab1"));
        return;
    }

    const sensors = sensorRows(model);
    const procs = sensorProcessCount(model);
    const running = sensors.filter((s) => s.pid !== null && s.up !== false).length;
    const alerts = model.events.filter((e) => e.level === "ALERT");
    const shown = contactsForDisplay(model);

    const tiles = el("div", { class: "tiles" });

    const unknown = unknownIds(model, live);
    const high = highIds(model, live);
    const outside = [...unknown].filter((id) => !high.has(id)).length;
    const regions = [...new Set(high.values())].join(", ");
    tiles.append(high.size > 0
        ? tile("Threat level", "HIGH", "bad",
               (high.size === 1 ? "1 unidentified contact" : high.size + " unidentified contacts") +
               " in " + regions)
        : (outside > 0
            ? tile("Threat level", "ELEVATED", "warn",
                   (outside === 1 ? "1 unidentified contact" : outside + " unidentified contacts") +
                   " outside the monitored regions")
            : (model.priority > 0
                ? tile("Threat level", "ELEVATED", "warn",
                       model.priority + " priority contacts")
                : tile("Threat level", "LOW", "ok", "no priority contact"))));

    tiles.append(procs > 0
        ? (model.finished
            ? tile("Sensor health", "RUN COMPLETE", "ok",
                   procs + " sensor processes ran")
            : tile("Sensor health", running === procs ? "NOMINAL" : "DEGRADED",
                   running === procs ? "ok" : "warn",
                   running + " of " + procs + " processes reporting"))
        : tile("Sensor health", "ONE FEED", "info",
               "Part 1 reads a file. Processes arrive in Part 2"));

    const wrows = live && live.workers ? live.workers : null;
    if (wrows) {
        const busy = live.busy !== undefined ? live.busy
                   : wrows.filter((w) => w.busy).length;
        tiles.append(tile("Worker pool", busy + " / " + wrows.length,
                          live.running ? "ok" : "idle",
                          live.running ? "busy now" : "the run ended"));
    } else {
        tiles.append(model.pool
            ? tile("Worker pool", model.pool.workers + " threads", "ok",
                   model.pool.processed !== null
                       ? model.pool.processed + " processed" : "running")
            : tile("Worker pool", "1 thread", "idle", "waiting for Part 3"));
    }

    const lg = live && live.ledger;
    if (lg) {
        const lost = lg.read - lg.counted;
        tiles.append(lg.balanced
            ? tile("System status",
                   lost ? "ACCOUNTED FOR" : "OPERATIONAL", lost ? "warn" : "ok",
                   lost ? lost + " of " + lg.read + " did not reach the picture, "
                          + "each for a named reason"
                        : "every report reached the picture")
            : tile("System status", "LOSING DATA", "bad",
                   "the ledger does not balance: " + lg.accounted
                   + " accounted of " + lg.read + " read"));
    } else {
        const exact = model.totals && model.totals.processed === model.totals.read;
        tiles.append(model.totals
            ? tile("System status", exact ? "OPERATIONAL" : "LOSING DATA",
                   exact ? "ok" : "bad",
                   exact ? "totals match" : "the totals do not match")
            : tile("System status", "OPERATIONAL", "ok",
                   model.events.length + " log lines"));
    }

    root.append(tiles);

    const active = new Set(sensors.filter((s) => s.count > 0).map((s) => s.name));
    const plotted = (tracks && tracks.tracks && tracks.tracks.length)
        ? tracks.tracks.map((t) => ({
              id: t.id, sensor: t.sensor, lat: t.lat, lon: t.lon,
              speed: t.speed, priority: t.priority,
              flag: t.flag || (t.priority ? "P" : "R"),
              last_ns: t.last_ns,
          }))
        : shown;
    const map = renderMap(plotted, {
        activeSensors: active, unknown: unknown, high: high, trails: trails,
        now: tracks ? tracks.now : null,
        staleMs: live && live.assoc_max_age_ms ? live.assoc_max_age_ms : null,
    });

    const legend = el("div", { class: "legend" });
    ["NWS", "AUR", "AIS", "SAT"].forEach((id) => {
        legend.append(el("span", { class: "lg" },
            el("i", { style: "background:" + SENSOR_COLOUR[id] }), text(id)));
    });
    legend.append(el("span", { class: "lg" },
        el("i", { class: "diamond" }), text("priority")));
    if (unknown.size) {
        legend.append(el("span", { class: "lg" },
            el("i", { class: "square" }), text("unknown")));
    }

    const mapCard = el("section", { class: "card" },
        el("div", { class: "card-h" },
            el("h3", {}, text("Track picture")),
            el("span", { style: "flex:1" }),
            legend),
        el("div", { class: "card-b mapwrap" }, map.svg));

    const caption = !plotted.length
        ? "No positions yet. The boxes are the areas each sensor watches in " +
          "contactfeed. Part 2 sends its contacts down a pipe and " +
          "logs no position, so a Part 2 run draws only the areas."
        : (tracks && tracks.tracks && tracks.tracks.length
            ? map.drawn + " of " + tracks.count + " TRACKS plotted: one symbol " +
              "per target, from tracks.json. Three sensors watching one " +
              "aircraft are one symbol here, because the centre associated " +
              "them. " + tracks.reports + " reports produced these " +
              tracks.count + " tracks" +
              (tracks.refused ? ", and " + tracks.refused +
                                " reports were refused as impossible" : "") + "." +
              (map.aged
                ? " " + (map.hidden
                    ? map.hidden + " target" + (map.hidden === 1 ? " is" : "s are")
                      + " not drawn: nothing has been heard from "
                      + (map.hidden === 1 ? "it" : "them") + " for more than "
                      + Math.round(map.staleMs / 1000) + " s, which is the "
                      + "centre's own rule for when a track stops being a "
                      + "candidate for a new report. A symbol fades as it "
                      + "approaches that."
                    : "Every target here was heard from in the last "
                      + Math.round(map.staleMs / 1000) + " s, the oldest "
                      + (map.oldestMs / 1000).toFixed(0) + " s ago. A symbol "
                      + "fades as it approaches that limit and goes when it "
                      + "passes it.")
                : "") +
              " A red diamond is a target whose LATEST report was urgent, "
              + map.urgent + " here" +
              (map.everUrgent
                ? "; " + map.everUrgent + " more raised priority earlier in "
                  + "this run and are drawn as what they are now, with the "
                  + "history in the tooltip." : ".")
            : map.drawn + " of " + map.total + " CONTACT REPORTS plotted, at the " +
              "lat and lon your own sensors reported. These are reports and not " +
              "targets: two sensors watching one aircraft are two symbols. A later " +
              "lab publishes the associated picture.") +
          " There is no coastline: a coarse one would invent a shoreline you " +
          "could not tell from the data.";

    let trailNote = "";
    if (trails && trails.size) {
        let drawn = 0;
        plotted.forEach((t) => {
            if (!unknown.has(t.id)) return;
            const pts = trails.get(t.id);
            if (pts && pts.length > 1) drawn++;
        });
        let furthest = 0;
        plotted.forEach((t) => {
            if (!unknown.has(t.id)) return;
            const pts = trails.get(t.id);
            if (!pts || pts.length < 2) return;
            const a = pts[0], b = pts[pts.length - 1];
            const dlat = (b.lat - a.lat) * 111.2;
            const dlon = (b.lon - a.lon) * 111.2 *
                         Math.cos((a.lat + b.lat) / 2 * Math.PI / 180);
            const km = Math.sqrt(dlat * dlat + dlon * dlon);
            if (km > furthest) furthest = km;
        });

        trailNote = drawn
            ? " An amber line is drawn behind " + drawn + " of them, the " +
              "unknown ones that have moved, because a line behind every " +
              "target is a ball of wool. That line is what THIS PAGE saw: it " +
              "starts when the page is opened and it has the resolution of " +
              "the poll, not of the reports. No file records where a target " +
              "HAS BEEN, because the centre keeps one record for each and " +
              "overwrites its position. The furthest of them has moved " +
              furthest.toFixed(1) + " km, which on a picture four thousand " +
              "kilometres wide is why the line is short: a ring marks where " +
              "each one started."
            : "";
    }

    const identity = unknown.size
        ? " " + unknown.size + " of them are UNKNOWN: the centre found no filed " +
          "plan, registration or zone for them. Nothing in a report line says " +
          "friend; the centre decides, against common/traffic.txt."
        : (live && live.expected
            ? " Every contact is filed traffic: the centre matched each one " +
              "against common/traffic.txt."
            : " No expected-traffic list was given, so no contact can be called " +
              "unknown.");

    mapCard.querySelector(".mapwrap").append(
        el("p", { class: "note" }, text(caption + identity + trailNote)));

    root.append(el("div", { class: "row-2" },
        workerPanel(live, liveSame, livePrev), mapCard));

    if (live && live.ledger) {
        root.append(ledgerPanel(live));
    }

    const cmpCard = comparePanel(compare);
    if (cmpCard) {
        root.append(cmpCard);
    }

    const counts = el("dl", { class: "counts" },
        count("contacts read", model.accepted + model.rejected || shown.length),
        count("accepted", model.accepted || shown.length, "ok"),
        count("rejected", model.rejected, model.rejected ? "bad" : null),
        count("priority", model.priority, model.priority ? "warn" : null),
        count("elapsed", elapsedText(model) || "—"));

    const perSensor = el("div", { class: "phase-pills" });
    sensors.forEach((s) => {
        perSensor.append(el("span", { class: "pill" },
            text(s.name + "  " + s.count)));
    });
    if (!sensors.length) perSensor.append(el("span", { class: "note" }, text("no sensor named yet")));

    root.append(el("section", { class: "card", style: "margin:16px 0" },
        el("div", { class: "card-h" },
            el("h3", {}, text("This run")),
            el("span", { style: "flex:1" }),
            runLabel(model.run)),
        el("div", { class: "card-b" }, counts,
            el("div", { style: "margin-top:14px" },
                el("h5", { class: "sub-h" }, text("Per sensor")), perSensor))));

    root.append(el("section", { class: "card", style: "margin-bottom:16px" },
        el("div", { class: "card-h" },
            el("h3", {}, text("The six requirements")),
            el("span", { style: "flex:1" }),
            chip(run ? "info" : "idle",
                 run ? "from your run" : "derived by the page")),
        el("div", { class: "card-b" },
            run ? requirementsFromRun(run) : requirements(model),
            el("p", { class: "note", style: "margin-top:12px" }, text(
                run
                ? "These are the verdicts SENTINEL printed. " +
                  "The page shows them and works nothing out. Ran " +
                  run.ranAt + ", exit code " + run.exitCode + "."
                : "No run has been recorded, so the page is deriving what it " +
                  "can from the mission log and saying which parts are only " +
                  "derived. A later lab records the verdicts of a run.")))));

    const two = el("div", { class: "three-col" });

    const alertBox = el("div", { class: "card-b" });
    if (!alerts.length) {
        alertBox.append(el("div", { class: "allclear" },
            el("strong", {}, text("No active alerts")),
            el("span", {}, text("All clear"))));
    } else {
        alerts.slice(-6).forEach((a) => {
            alertBox.append(el("div", { class: "alert" },
                el("span", { class: "ts" }, text(clockText(a.stamp))),
                el("span", {}, text(a.message))));
        });
    }
    two.append(el("section", { class: "card" },
        el("div", { class: "card-h" }, el("h3", {}, text("Recent alerts")),
            el("span", { style: "flex:1" }),
            alerts.length ? chip("bad", String(alerts.length)) : chip("ok", "0")),
        alertBox));

    const ls = liveState(live, liveSame);
    const tail = el("div", { class: "card-b" }, el("div", { class: "logbox short" },
        ...model.events.slice(-12).map((e) => e.ok
            ? el("div", { class: "logline" },
                el("span", { class: "ts" }, text(clockText(e.stamp))),
                el("span", { class: "lv " + (LEVEL_CLASS[e.level] || "idle") }, text(e.level)),
                el("span", { class: "msg" }, text(e.message)))
            : el("div", { class: "logline bad" },
                el("span", { class: "msg" }, text(e.raw))))));

    if (model.eventsDropped) {
        tail.append(el("p", { class: "note" }, text(
            "The page keeps the newest " + model.events.length + " lines and has " +
            "dropped " + model.eventsDropped + " older ones. The counts above are " +
            "of the whole run; " + (live ? "the mission log file holds every line."
                                         : "missions.log holds every line."))));
    }

    two.append(el("section", { class: "card" },
        el("div", { class: "card-h" }, el("h3", {}, text("Mission log")),
            el("span", { style: "flex:1" }),
            ls.on ? chip("ok", "LIVE") : chip("idle", ls.label),
            chip("info", model.events.length + " lines")),
        tail));

    two.append(sensorPanel(live, model));

    root.append(two);
}

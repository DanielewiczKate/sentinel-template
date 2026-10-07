import { el, text, chip, pending } from "../dom.js";
import { sensorProcessCount, sensorRows, contactsForDisplay } from "../model.js";
import { clockText } from "../parse.js";

const STATES = [
    {
        n: "i",
        title: "Normal Operations",
        story: "All four sensors report healthy. The dashboard shows a low " +
               "threat level and no active alerts.",
        needs: "Part 2", req: "R1",
        test: (m) => {
            const procs = sensorProcessCount(m);
            if (procs === 0) return null;
            const alerts = m.levelCount.ALERT;
            const routine = m.levelCount.INFO;
            return procs + " sensor processes started" +
                (alerts ? ", " + alerts + " alerts later"
                        : ", no alert before the first contact") +
                (routine
                    ? ", and the log records " + routine + " routine sensor updates"
                    : ", but the log records no routine sensor update: "
                      + "run with --log-detail full");
        },
    },
    {
        n: "ii",
        title: "Critical Contact Detected",
        story: "A sensor detects an unknown contact. A report is generated " +
               "carrying its position and speed. The fixed line format has " +
               "no altitude, and a heading has to be derived from two " +
               "positions: the map does that and says so.",
        needs: "Part 1", req: null,
        test: (m) => {
            const c = contactsForDisplay(m);
            if (c.length) {
                const p = c.filter((x) => x.priority);
                if (!p.length) return c.length + " contacts read, none flagged P";
                const first = p[0];
                return first.id + " from " + first.sensor + " at " +
                    first.lat.toFixed(2) + ", " + first.lon.toFixed(2);
            }
            if (m.totals) {
                return { partial: true, text: m.totals.read + " reports were read, but " +
                    "this run logs a summary and writes no feed file, so there is " +
                    "no position to plot. make lab1 keeps every contact." };
            }
            return null;
        },
    },
    {
        n: "iii",
        title: "Threat Assessment",
        story: "The scheduler evaluates the contact and assigns it high " +
               "priority. The threat level rises.",
        needs: "Part 2", req: "R2",
        test: (m) => {
            const hs = Object.values(m.highContacts || {});
            if (hs.length) {
                const first = hs[0];
                return m.high + " HIGH: " + first.id + " from " + first.sensor +
                    " is not filed and is in " + first.region +
                    (hs.length > 1 ? ", and " + (hs.length - 1) + " other contact" +
                        (hs.length > 2 ? "s" : "") : "");
            }
            const w = m.priorityWaits;
            if (w.length) {
                const mean = w.reduce((a, b) => a + b, 0) / w.length;
                return m.priority + " promoted ahead of routine traffic, mean wait " +
                    mean.toFixed(1) + " ms";
            }
            const policy = m.run && m.run.policy;
            if (m.priority && policy) {
                return m.priority + " promoted under the " + policy + " policy";
            }
            if (policy && m.urgent.length) {
                const n = m.urgent.length;
                return "policy " + policy + ", and " + n + " urgent contact" +
                    (n === 1 ? "" : "s") + " took the priority channel. The whole " +
                    "system logs a summary, so the per-contact count is on " +
                    "standard output and not in the log.";
            }
            if (policy) {
                return { partial: true, text: "the run used the " + policy +
                    " policy, but no priority contact appears in the log." };
            }
            if (m.priority) {
                return { partial: true, text: m.priority + " contacts carry the P flag, " +
                    "but nothing has scheduled them. Part 2 is what decides an order." };
            }
            return null;
        },
    },
    {
        n: "iv",
        title: "Concurrent Processing",
        story: "A worker takes the contact off the shared queue while the " +
               "others carry on. On one processor they take turns: many " +
               "reports are in progress, and one is executing.",
        needs: "Part 3", req: "R3",
        test: (m, live, prev) => {
            if (live && prev && live.workers && prev.workers && live.cpus) {
                const dt = live.written - prev.written;
                if (dt > 0) {
                    const was = new Map(prev.workers.map((w) => [w.index, w.cpu_ns || 0]));
                    let used = 0, seen = 0;
                    live.workers.forEach((w) => {
                        const before = was.get(w.index);
                        if (before === undefined) return;
                        const share = ((w.cpu_ns || 0) - before) / dt;
                        if (share >= 0) { used += share; seen++; }
                    });
                    if (seen) {
                        return live.workers.length + " workers on " + live.cpus +
                            " processor" + (live.cpus === 1 ? "" : "s") + ", using " +
                            used.toFixed(2) + " of " + live.cpus +
                            (live.cpus === 1
                                ? ". However many workers there are, this cannot pass 1.00"
                                : "");
                    }
                }
            }
            if (!m.pool) return null;
            return { partial: true, text: m.pool.workers + " workers ran" +
                (m.pool.processed !== null ? " and processed " + m.pool.processed : "") +
                ". A mission log records that they existed, not how much of a " +
                "processor each one was given. Run a live centre to see that." };
        },
    },
    {
        n: "v",
        title: "Track Picture Updated",
        story: "The contact is added to the shared track database and the " +
               "map updates. Unknown becomes tracked, and the map draws where " +
               "it has been.",
        needs: "Part 3", req: "R4",
        test: (m, live) => {
            if (live && live.ledger) {
                const lg = live.ledger;
                const lost = lg.read - lg.counted;
                return lg.counted + " of " + lg.read + " reports reached the " +
                    "picture" +
                    (lost > 0 ? ", and the other " + lost +
                                (lost === 1 ? " has" : " each have") +
                                " a named reason it did not" : "") +
                    (lg.balanced ? ". Every report is accounted for"
                                 : ". THE LEDGER DOES NOT BALANCE");
            }
            if (m.totals) {
                const exact = m.totals.processed === m.totals.read;
                return m.totals.tracks + " tracks from " + m.totals.read +
                    " reports" + (exact ? ", totals match" : ", TOTALS DO NOT MATCH");
            }
            if (m.levelCount.TRACK) return m.levelCount.TRACK + " track updates logged";
            return null;
        },
        warnIf: (m, live) => (live && live.ledger)
            ? !live.ledger.balanced
            : (m.totals && m.totals.processed !== m.totals.read),
    },
    {
        n: "vi",
        title: "Mission Logging",
        story: "Every step is timestamped and written to the mission log: " +
               "contact detected, priority assigned, worker started, track " +
               "updated, alert raised.",
        needs: "Part 1", req: "R6",
        test: (m) => {
            if (!m.events.length) return null;
            const NEED = [
                ["INFO",     "contact detected"],
                ["PRIORITY", "priority assigned"],
                ["PENDING",  "worker processing started"],
                ["TRACK",    "track updated"],
                ["ALERT",    "alert generated"],
            ];
            const missing = NEED.filter(([k]) => !m.levelCount[k]);
            const required = missing.filter(([k]) => k !== "ALERT");
            const counts = NEED.filter(([k]) => m.levelCount[k])
                .map(([k, what]) => what + " " + m.levelCount[k]).join(", ");

            const head = m.linesSeen + " lines. ";
            if (!required.length) {
                return head + "All five steps are recorded: " + counts +
                    (missing.length
                        ? ". No alert was raised, which is a quiet run and not "
                          + "a missing step: the centre needs --expected to "
                          + "decide that a contact is unknown."
                        : ".");
            }
            return {
                partial: true,
                text: head + "Only " + (5 - missing.length) + " of the five "
                    + "steps this story names are in the log. Missing: "
                    + required.map(([, what]) => what).join(", ")
                    + ". Recorded: " + (counts || "none")
                    + ". Run with --log-detail full.",
            };
        },
    },
    {
        n: "vii",
        title: "Operator Decision",
        story: "The picture and the alert are put in front of the watch " +
               "officer, who decides what to do about them.",
        needs: null, phase: null, req: null,
        human: true,
        test: () => null,
    },
];

export function renderSituation(root, model, live, livePrev) {
    root.replaceChildren();

    root.append(el("p", { class: "labs-intro" }, text(
        "Section 5 of the lab document tells one story in seven states. " +
        "The document says those screens are not a course requirement, so this " +
        "page does not copy them. It shows which part of the story your own " +
        "code tells today. A grey state names the Part that would light it.")));

    if (!model.events.length && !model.contacts.length) {
        root.append(pending(
            "Nothing has run yet",
            "Each state lights from your own mission log and feed. Build Part 1 " +
            "and send a feed through it, and state vi lights first.",
            "make lab1"));
        return;
    }

    const list = el("div", { class: "story" });

    STATES.forEach((s) => {
        const raw = s.test(model, live, livePrev);
        const partial = raw !== null && typeof raw === "object" && raw.partial;
        const got = raw === null ? null : (partial ? raw.text : raw);
        const warn = s.warnIf && s.warnIf(model, live);
        const lit = got !== null && !partial;

        const cls = "state" + (s.human ? " human"
            : partial ? " partial"
            : lit ? (warn ? " warn" : " lit") : "");
        const row = el("article", { class: cls });

        row.append(el("div", { class: "sn" }, text(s.n)));

        const body = el("div", { class: "sbody" });
        const head = el("div", { class: "shead" },
            el("h4", {}, text(s.title)),
            el("span", { style: "flex:1" }));

        if (s.human) head.append(chip("info", "a person decides"));
        else if (warn) head.append(chip("bad", "shown, and wrong"));
        else if (lit) head.append(chip("ok", "shown"));
        else if (partial) head.append(chip("warn", "part of it"));
        else head.append(chip("idle", "waiting for " + s.needs));

        if (s.req) head.append(chip("req", s.req));
        body.append(head);

        body.append(el("p", { class: "story-text" }, text(s.story)));

        if (s.human) {
            body.append(el("p", { class: "evidence human" }, text(
                "Nothing here is automatic, and nothing should be. The rule in " +
                "Canadian and NORAD practice is that machines watch and people " +
                "decide. Sending an aircraft to look at a contact is always a " +
                "human decision.")));
        } else if (partial) {
            body.append(el("p", { class: "evidence partial" }, text(got)));
        } else if (lit) {
            body.append(el("p", { class: "evidence" }, text(got)));
            if (warn) {
                body.append(el("p", { class: "evidence bad" }, text(
                    live && live.ledger
                    ? "The ledger does not balance: " + live.ledger.accounted +
                      " reports are accounted for out of " + live.ledger.read +
                      " read. The counters are being written by several workers " +
                      "with no protection, so they cannot be trusted to add up. " +
                      "This is the Part 3 fault, left in on purpose, and Part 4 " +
                      "repairs it."
                    : "The track picture is being written by several workers with " +
                      "no protection, so the totals disagree. This is the Part 3 " +
                      "fault, left in on purpose. Part 4 repairs it.")));
            }
        } else {
            body.append(el("p", { class: "evidence idle" }, text(
                "Not shown yet. " + s.needs + " makes this state possible.")));
        }

        row.append(body);
        list.append(row);
    });

    root.append(list);

    const lit = STATES.filter((s) => {
        const r = s.test(model, live, livePrev);
        return !s.human && r !== null && !(typeof r === "object" && r.partial);
    }).length;
    root.append(el("p", { class: "note" }, text(
        lit + " of 6 machine states are shown by this run. The seventh is the " +
        "watch officer, and it is not a state software can reach.")));
}

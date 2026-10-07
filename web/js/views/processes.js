import { el, text, chip, pending } from "../dom.js";
import { sensorRows } from "../model.js";

function stat(label, value, note) {
    return el("div", { class: "stat" },
        el("dt", {}, text(label)),
        el("dd", {}, text(value === null || value === undefined ? "—" : String(value))),
        note ? el("small", {}, text(note)) : null);
}

export function renderProcesses(root, model, live) {
    root.replaceChildren();

    const fromRun = {};
    if (live && Array.isArray(live.sensors)) {
        live.sensors.forEach((s) => { fromRun[s.name] = s; });
    }
    const hasRun = Object.keys(fromRun).length > 0;
    const ended  = live ? !live.running : model.finished;

    const sensors = sensorRows(model).filter((s) => s.pid !== null);
    const self = model.self;

    if (!self && !sensors.length && !model.pool) {
        root.append(pending(
            "No process reported yet",
            "The page reads the process facts out of the mission log. demo_lab1 logs " +
            "one line holding pid, ppid, name and open_fds. " +
            "Part 2 adds one line per sensor process.",
            "make lab1"));
        return;
    }

    if (self) {
        root.append(el("section", { class: "card", style: "margin-bottom:16px" },
            el("div", { class: "card-h" },
                el("h3", {}, text("The centre process")),
                el("span", { style: "flex:1" }),
                chip("info", "from /proc, via the log")),
            el("div", { class: "card-b" },
                el("dl", { class: "stat-row" },
                    stat("name", self.name),
                    stat("pid", self.pid),
                    stat("parent pid", self.ppid),
                    stat("open descriptors", self.openFds,
                         "0,1,2 plus the files it holds")))));
    }

    if (sensors.length) {
        const table = el("table", { class: "grid" });
        table.append(el("thead", {}, el("tr", {},
            el("th", {}, text("sensor")),
            el("th", { class: "num" }, text("pid")),
            el("th", { class: "num" }, text("rate")),
            el("th", { class: "num" }, text(hasRun ? "reports sent" : "reports in the log")),
            el("th", {}, text("state")),
            el("th", {}, text("")))));

        const tb = el("tbody", {});
        sensors.forEach((s) => {
            const rate = s.burst ? s.burst + " burst" : (s.rate ? s.rate + "/s" : "—");
            const run  = fromRun[s.name] || null;

            let state;
            if (s.up === false && /^finished/.test(s.how || "")) {
                state = chip("idle", "finished its count");
            } else if (s.up === false) {
                state = chip("warn", s.how || "stopped");
            } else if (run && !run.running) {
                state = chip("warn", "stopped");
            } else if (ended) {
                state = chip("idle", "the run ended");
            } else {
                state = chip("ok", "reporting");
            }

            tb.append(el("tr", {},
                el("td", { class: "mono" }, text(s.name)),
                el("td", { class: "num mono" }, text(s.pid)),
                el("td", { class: "num mono" }, text(rate)),
                el("td", { class: "num mono" }, text((run ? run.reports : s.count) || "—")),
                el("td", {}, state),
                el("td", {}, run && run.restarts
                    ? chip("warn", "restarted " + run.restarts)
                    : text(""))));
        });
        table.append(tb);

        root.append(el("section", { class: "card", style: "margin-bottom:16px" },
            el("div", { class: "card-h" },
                el("h3", {}, text("Sensor processes")),
                el("span", { style: "flex:1" }),
                ended ? chip("idle", "the run ended") : el("span", {}),
                chip("info", sensors.length + " of 4")),
            el("div", { class: "card-b tablewrap" }, table,
               el("p", { class: "note" }, text(hasRun
                   ? "reports sent is what each sensor sent, from live.json. NWS, " +
                     "AUR and AIS send for the whole run. By default the satellite " +
                     "sends 2000 each second in a pass of 10 s, then 10 s with no " +
                     "reports; its process runs in the gap."
                   : "reports in the log counts the contacts the LOG names. Part 1 logs " +
                     "every contact; Part 2 and later log only the priority ones, one " +
                     "report in ten. Run a lab with LIVE= to see what each sensor sent.")))));
    }

    if (model.pool) {
        root.append(el("section", { class: "card" },
            el("div", { class: "card-h" }, el("h3", {}, text("Worker pool"))),
            el("div", { class: "card-b" },
                el("dl", { class: "stat-row" },
                    stat("threads", model.pool.workers),
                    stat("processed", model.pool.processed)))));
    } else if (self) {
        root.append(el("p", { class: "note" }, text(
            "No worker pool. Part 1 does its work in one process with one thread; " +
            "the pool arrives in Part 3.")));
    }
}

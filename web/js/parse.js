export const LEVELS = ["SYSTEM", "INFO", "PENDING", "PRIORITY", "ALERT", "TRACK"];

const LOG_RE = /^(\d+)\.(\d{1,9})\s+([A-Z]+)\s+(.*)$/;

function stamp(sec, usec) {
    const micro = (usec + "000000").slice(0, 6);
    return {
        sec: Number(sec),
        usec: Number(micro),
        t: Number(sec) + Number(micro) / 1e6,
        date: new Date(Number(sec) * 1000 + Number(micro) / 1000),
    };
}

export function clockText(s) {
    if (!s || !s.date || isNaN(s.date.getTime())) return "--:--:--";
    const p = (n) => String(n).padStart(2, "0");
    return p(s.date.getHours()) + ":" + p(s.date.getMinutes()) + ":" + p(s.date.getSeconds());
}

const FACTS = [
    { key: "self", re: /^pid=(\d+)\s+ppid=(\d+)\s+name=(\S+)\s+open_fds=(\d+)/,
      take: (m) => ({ pid: +m[1], ppid: +m[2], name: m[3], openFds: +m[4] }) },

    { key: "sensorUp", re: /^sensor\s+(\w+)\s+started\s+pid=(\d+)(?:\s+rate=(\d+))?(?:\s+burst=(\d+))?/,
      take: (m) => ({ sensor: m[1], pid: +m[2], rate: m[3] ? +m[3] : null,
                      burst: m[4] ? +m[4] : null }) },

    { key: "sensorDown",
      re: /^sensor\s+(\w+)\s+\(pid\s+(\d+)\)\s+(stopped|finished.*|exited.*|died.*)$/,
      take: (m) => ({ sensor: m[1], pid: +m[2], how: m[3] }) },

    { key: "priorityContact",
      re: /^priority contact\s+(\S+)\s+from\s+(\w+)\s+at\s+(-?[\d.]+),(-?[\d.]+)\s+speed\s+([\d.]+)/,
      take: (m) => ({ id: m[1], sensor: m[2], lat: +m[3], lon: +m[4], speed: +m[5] }) },

    { key: "priorityServed",
      re: /^priority contact\s+(\S+)\s+from\s+(\w+)\s+served after\s+([\d.]+)\s*ms(?:,\s*HIGH: unknown in\s+(\S+))?/,
      take: (m) => ({ id: m[1], sensor: m[2], waitedMs: +m[3], high: m[4] || null }) },

    { key: "contact",
      re: /^contact\s+(\S+)\s+from\s+(\w+)(?:\s+at\s+(-?[\d.]+)\s+(-?[\d.]+),\s*([\d.]+)\s*kn,\s*(\w+))?/,
      take: (m) => ({
          id: m[1], sensor: m[2],
          lat: m[3] === undefined ? null : +m[3],
          lon: m[4] === undefined ? null : +m[4],
          speed: m[5] === undefined ? null : +m[5],
          flag: m[6] === "priority" ? "P" : (m[6] ? "R" : null),
      }) },

    { key: "workerStarted",
      re: /^worker\s+(\d+)\s+started\s+(\S+)\s+from\s+(\w+)\s+after\s+([\d.]+)\s*ms/,
      take: (m) => ({ worker: +m[1], id: m[2], sensor: m[3], waitedMs: +m[4] }) },

    { key: "trackUpdated",
      re: /^track\s+(\S+)\s+(created|updated)\s+from\s+(\w+)\s+at\s+(-?[\d.]+)\s+(-?[\d.]+),\s*([\d.]+)\s*kn/,
      take: (m) => ({
          id: m[1], created: m[2] === "created", sensor: m[3],
          lat: +m[4], lon: +m[5], speed: +m[6],
      }) },

    { key: "rejected", re: /^rejected line\s+(\d+):\s*(.*)$/,
      take: (m) => ({ line: +m[1], why: m[2] }) },

    { key: "partComplete",
      re: /^Part (\d) complete:\s*(\d+) (?:read|accepted)/,
      take: (m) => ({ part: +m[1], accepted: +m[2] }) },

    { key: "complete", re: /^SENTINEL complete:\s*(\d+) read,\s*(\d+) processed,\s*(\d+) tracks/,
      take: (m) => ({ read: +m[1], processed: +m[2], tracks: +m[3] }) },

    { key: "start", re: /^SENTINEL starting:\s*(\d+) workers,\s*policy (\w+),\s*(\d+) ms/,
      take: (m) => ({ workers: +m[1], policy: m[2], ms: +m[3] }) },

    { key: "startPart1", re: /^SENTINEL Part 1 starting,\s*feed=(\S+)/,
      take: (m) => ({ feed: m[1] }) },

    { key: "startPartN", re: /^SENTINEL Part (\d) starting(?:,\s*policy=(\w+))?/,
      take: (m) => ({ part: +m[1], policy: m[2] || null }) },

    { key: "poolUp", re: /^(?:protected )?worker pool started:\s*(\d+) workers/,
      take: (m) => ({ workers: +m[1] }) },

    { key: "poolDown", re: /^(?:protected )?worker pool stopped:\s*(\d+) processed/,
      take: (m) => ({ processed: +m[1] }) },

    { key: "unknown",
      re: /^unknown contact\s+(\S+)\s+from\s+(\w+)\s+at\s+(-?[\d.]+)\s+(-?[\d.]+)/,
      take: (m) => ({ id: m[1], sensor: m[2], lat: +m[3], lon: +m[4] }) },

    { key: "workers",
      re: /^workers:\s*(\d+) of (\d+) busy,\s*(\d+) processed,\s*queue (\d+)/,
      take: (m) => ({ busy: +m[1], workers: +m[2], processed: +m[3], queue: +m[4] }) },

    { key: "priorityQueued",
      re: /^priority contact\s+(\S+)\s+from\s+(\w+)\s+queued for the workers(?:,\s*HIGH: unknown in\s+(\S+))?/,
      take: (m) => ({ id: m[1], sensor: m[2], high: m[3] || null }) },

    { key: "stalled", re: /^the workers did not stop in\s+(\d+) s/,
      take: (m) => ({ seconds: +m[1] }) },

    { key: "urgent", re: /^urgent contact\s+(\S+)\s+received on the priority channel/,
      take: (m) => ({ id: m[1] }) },
];

export function parseLogLine(line) {
    const m = LOG_RE.exec(line);
    if (!m) {
        return { ok: false, level: null, message: line, raw: line, fact: null };
    }

    const level = m[3];
    const message = m[4];

    let fact = null;
    for (const f of FACTS) {
        const hit = f.re.exec(message);
        if (hit) { fact = { key: f.key, ...f.take(hit) }; break; }
    }

    return {
        ok: true,
        stamp: stamp(m[1], m[2]),
        level: level,
        known: LEVELS.indexOf(level) >= 0,
        message: message,
        raw: line,
        fact: fact,
    };
}

export function parseContactLine(line) {
    const f = line.trim().split(/\s+/);
    if (f.length !== 7) {
        return { ok: false, raw: line, why: "expected 7 fields, found " + f.length };
    }

    const dot = f[0].indexOf(".");
    const c = {
        ok: true,
        raw: line,
        stamp: dot < 0 ? stamp(f[0], "0") : stamp(f[0].slice(0, dot), f[0].slice(dot + 1)),
        sensor: f[1],
        id: f[2],
        lat: Number(f[3]),
        lon: Number(f[4]),
        speed: Number(f[5]),
        flag: f[6],
        priority: f[6] === "P",
    };

    if (!isFinite(c.lat) || !isFinite(c.lon) || !isFinite(c.speed)) {
        return { ok: false, raw: line, why: "a number did not parse" };
    }
    if (c.flag !== "P" && c.flag !== "R") {
        return { ok: false, raw: line, why: "the flag is not P or R" };
    }
    if (line.length > 128) {
        return { ok: false, raw: line, why: "longer than 128 bytes" };
    }
    return c;
}

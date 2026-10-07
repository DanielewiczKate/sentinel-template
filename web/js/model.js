import { parseLogLine, parseContactLine, LEVELS } from "./parse.js";

const MAX_KEEP = 4000;

function noteUrgent(m, f) {
    if (f.high) {
        m.high++;
        if (!m.highContacts[f.id]) {
            m.highContacts[f.id] = { id: f.id, sensor: f.sensor, region: f.high };
        }
    } else {
        m.priority++;
    }
}

export function createModel() {
    return {
        events: [],
        levelCount: LEVELS.reduce((a, l) => (a[l] = 0, a), {}),
        unparsed: 0,

        contacts: [],
        contactBad: [],
        perSensor: {},

        accepted: 0,
        rejected: 0,
        priority: 0,
        priorityWaits: [],
        high: 0,
        highContacts: {},
        logPerSensor: {},

        self: null,
        sensors: {},
        pool: null,

        unknown: {},
        unknownCount: 0,
        stalled: false,

        eventsDropped: 0,
        linesSeen: 0,
        contactsDropped: 0,

        workerWaits: [],
        trackUpdates: 0,
        trackCreated: 0,

        finished: false,
        run: null,
        totals: null,
        urgent: [],

        firstStamp: null,
        lastStamp: null,
    };
}

function bump(map, key) { map[key] = (map[key] || 0) + 1; }

export function addEvents(m, lines) {
    for (const line of lines) {
        const e = parseLogLine(line);

        if (!e.ok) { m.unparsed++; m.events.push(e); continue; }

        if (e.known) m.levelCount[e.level]++;
        m.linesSeen++;
        if (!m.firstStamp) m.firstStamp = e.stamp;
        m.lastStamp = e.stamp;

        const f = e.fact;
        if (f) {
            switch (f.key) {
            case "self":
                m.self = { pid: f.pid, ppid: f.ppid, name: f.name, openFds: f.openFds };
                break;
            case "sensorUp":
                m.sensors[f.sensor] = {
                    name: f.sensor, pid: f.pid, rate: f.rate, burst: f.burst,
                    up: true, how: null,
                };
                break;
            case "sensorDown": {
                const s = m.sensors[f.sensor] || { name: f.sensor, pid: f.pid };
                s.up = false;
                s.how = f.how;
                m.sensors[f.sensor] = s;
                break;
            }
            case "priorityContact":
                m.priority++;
                m.accepted++;
                bump(m.logPerSensor, f.sensor);
                m.contacts.push({
                    ok: true, id: f.id, sensor: f.sensor, lat: f.lat, lon: f.lon,
                    speed: f.speed, flag: "P", priority: true, stamp: e.stamp,
                    fromLog: true,
                });
                break;
            case "priorityServed":
                noteUrgent(m, f);
                m.accepted++;
                bump(m.logPerSensor, f.sensor);
                m.priorityWaits.push(f.waitedMs);
                break;
            case "contact":
                m.accepted++;
                bump(m.logPerSensor, f.sensor);
                if (f.lat !== null && f.lon !== null) {
                    m.contacts.push({
                        ok: true, id: f.id, sensor: f.sensor,
                        lat: f.lat, lon: f.lon, speed: f.speed,
                        flag: f.flag || "R", priority: f.flag === "P",
                        stamp: e.stamp, fromLog: true,
                    });
                }
                break;
            case "workerStarted":
                m.workerWaits.push(f.waitedMs);
                if (m.workerWaits.length > MAX_KEEP) m.workerWaits.splice(0, 1);
                break;
            case "trackUpdated":
                m.trackUpdates++;
                if (f.created) m.trackCreated++;
                break;
            case "rejected":
                m.rejected++;
                break;
            case "start":
                m.run = { workers: f.workers, policy: f.policy, ms: f.ms };
                break;
            case "startPart1":
                m.run = { feed: f.feed, part: 1 };
                break;
            case "startPartN":
                m.run = { part: f.part, policy: f.policy };
                break;
            case "partComplete":
                m.finished = true;
                break;
            case "complete":
                m.finished = true;
                m.totals = { read: f.read, processed: f.processed, tracks: f.tracks };
                break;
            case "poolUp":
                m.pool = { workers: f.workers, processed: null };
                break;
            case "poolDown":
                m.pool = { workers: m.pool ? m.pool.workers : null, processed: f.processed };
                break;
            case "urgent":
                m.urgent.push({ id: f.id, stamp: e.stamp });
                break;
            case "unknown":
                if (!m.unknown[f.id]) {
                    m.unknown[f.id] = { id: f.id, sensor: f.sensor, lat: f.lat,
                                        lon: f.lon, stamp: e.stamp };
                    m.unknownCount++;
                }
                break;
            case "workers":
                m.pool = { workers: f.workers, processed: f.processed,
                           busy: f.busy, queue: f.queue };
                break;
            case "priorityQueued":
                noteUrgent(m, f);
                bump(m.logPerSensor, f.sensor);
                break;
            case "stalled":
                m.stalled = true;
                break;
            default:
                break;
            }
        }

        m.events.push(e);
    }

    if (m.events.length > MAX_KEEP) {
        const keep = [];
        let   drop = m.events.length - MAX_KEEP;

        for (const e of m.events) {
            if (drop > 0 && e.level !== "ALERT") {
                drop--;
                m.eventsDropped++;
                continue;
            }
            keep.push(e);
        }
        m.events = keep;
    }
    return m;
}

export function addContacts(m, lines) {
    for (const line of lines) {
        const c = parseContactLine(line);
        if (!c.ok) {
            m.contactBad.push(c);
            if (m.contactBad.length > MAX_KEEP) m.contactBad.splice(0, 1);
            continue;
        }
        bump(m.perSensor, c.sensor);
        m.contacts.push(c);
    }
    if (m.contacts.length > MAX_KEEP) {
        m.contactsDropped += m.contacts.length - MAX_KEEP;
        m.contacts.splice(0, m.contacts.length - MAX_KEEP);
    }
    return m;
}

export function contactsForDisplay(m) {
    const fromFeed = m.contacts.filter((c) => !c.fromLog);
    return fromFeed.length ? fromFeed : m.contacts;
}

export function priorityContacts(m) {
    return m.contacts.filter((c) => c.priority);
}

export function sensorProcessCount(m) {
    return Object.keys(m.sensors).length;
}

export function sensorRows(m) {
    const names = new Set([
        ...Object.keys(m.sensors),
        ...Object.keys(m.perSensor),
        ...Object.keys(m.logPerSensor),
    ]);
    return [...names].sort().map((name) => {
        const s = m.sensors[name] || null;
        const count = s
            ? (m.logPerSensor[name] || 0)
            : (m.perSensor[name] || m.logPerSensor[name] || 0);
        return {
            name: name,
            pid: s ? s.pid : null,
            rate: s ? s.rate : null,
            burst: s ? s.burst : null,
            up: s ? s.up : null,
            how: s ? s.how : null,
            count: count,
        };
    });
}

export function isEmpty(m) {
    return m.events.length === 0 && m.contacts.length === 0;
}

export function elapsedText(m) {
    if (!m.firstStamp || !m.lastStamp) return null;
    const ms = (m.lastStamp.t - m.firstStamp.t) * 1000;
    if (ms < 1000) return ms.toFixed(1) + " ms";
    return (ms / 1000).toFixed(2) + " s";
}

const NS = "http://www.w3.org/2000/svg";

const BOUNDS = { latLo: 43, latHi: 82, lonLo: -145, lonHi: -55 };

const LAT0 = 48;
const KX = Math.cos((LAT0 * Math.PI) / 180);

const W = 1000;
const H = Math.round(
    (W * (BOUNDS.latHi - BOUNDS.latLo)) /
    ((BOUNDS.lonHi - BOUNDS.lonLo) * KX)
);

function project(lat, lon) {
    const x = ((lon - BOUNDS.lonLo) / (BOUNDS.lonHi - BOUNDS.lonLo)) * W;
    const y = ((BOUNDS.latHi - lat) / (BOUNDS.latHi - BOUNDS.latLo)) * H;
    return [x, y];
}

const REGIONS = [
    { id: "NWS", latLo: 66.0, latHi: 74.0, lonLo: -120.0, lonHi: -90.0,
      what: "Arctic air" },
    { id: "AUR", latLo: 69.0, latHi: 76.0, lonLo: -140.0, lonHi: -120.0,
      what: "Beaufort Sea" },
    { id: "AIS", latLo: 47.5, latHi: 49.5, lonLo: -125.5, lonHi: -122.5,
      what: "Juan de Fuca Strait" },
    { id: "SAT", latLo: 45.0, latHi: 80.0, lonLo: -140.0, lonHi: -60.0,
      what: "wide area, one pass" },
];

const SITES = [
    { id: "NWS", lat: 69.12, lon: -105.06, name: "North Warning System", where: "Cambridge Bay, NU" },
    { id: "AUR", lat: 71.50, lon: -133.00, name: "CP-140 Aurora",        where: "Beaufort Sea" },
    { id: "AIS", lat: 48.35, lon: -123.90, name: "Coastal radar & AIS",  where: "Juan de Fuca Strait" },
    { id: "SAT", lat: 78.00, lon: -96.00,  name: "RADARSAT",             where: "polar orbit" },
];

const CENTRE = { lat: 48.43, lon: -123.37, name: "Operations centre", where: "Victoria, BC" };

const SENSOR_COLOUR = {
    NWS: "#4da3ff", AUR: "#7bd88f", AIS: "#ffc857", SAT: "#c792ea",
};

const STALE_MS = 30000;

const FADE_FROM = 0.4;

function symbol(kind, x, y, colour) {
    if (kind === "vessel") {
        return svgEl("path", {
            d: "M" + (x - 4) + " " + (y - 1.5) + " L" + (x + 4) + " " + (y - 1.5) +
               " L" + (x + 2.2) + " " + (y + 2.5) + " L" + (x - 2.2) + " " + (y + 2.5) + " Z",
            fill: colour, opacity: 0.85,
        });
    }
    return svgEl("path", {
        d: "M" + x + " " + (y - 4) + " L" + (x + 3.4) + " " + (y + 3) +
           " L" + x + " " + (y + 1.2) + " L" + (x - 3.4) + " " + (y + 3) + " Z",
        fill: colour, opacity: 0.85,
    });
}

function unknownFrame(x, y, label) {
    const g = svgEl("g", { class: "unk" });

    g.append(svgEl("circle", {
        cx: x, cy: y, r: 16, class: "unkpulse",
        fill: "none", stroke: "#ffb020", "stroke-width": 1.4,
    }));

    const h = 11, t = 5;
    for (const [sx, sy] of [[-1, -1], [1, -1], [-1, 1], [1, 1]]) {
        g.append(svgEl("path", {
            d: "M" + (x + sx * h) + " " + (y + sy * (h - t)) +
               " L" + (x + sx * h) + " " + (y + sy * h) +
               " L" + (x + sx * (h - t)) + " " + (y + sy * h),
            fill: "none", stroke: "#ffb020", "stroke-width": 2.2,
            "stroke-linecap": "square",
        }));
    }

    if (label) {
        const tx = x + 15, ty = y - 14;
        for (const cls of ["halo", "lab"]) {
            const n = svgEl("text", {
                x: tx, y: ty, class: "unklabel " + cls,
                "font-size": 10, "font-family": "ui-monospace, monospace",
            });
            n.textContent = label;
            g.append(n);
        }
    }
    return g;
}

function svgEl(name, attrs) {
    const n = document.createElementNS(NS, name);
    for (const [k, v] of Object.entries(attrs || {})) {
        if (v !== null && v !== undefined) n.setAttribute(k, String(v));
    }
    return n;
}

function title(node, str) {
    const t = svgEl("title");
    t.textContent = str;
    node.append(t);
    return node;
}

function headingOf(trails, id) {
    if (!trails) return "";
    const pts = trails.get(id);
    if (!pts || pts.length < 2) return "";

    const a = pts[0], b = pts[pts.length - 1];
    const dlat = b.lat - a.lat;
    const dlon = (b.lon - a.lon) *
                 Math.cos((a.lat + b.lat) / 2 * Math.PI / 180);
    if (Math.abs(dlat) < 1e-9 && Math.abs(dlon) < 1e-9) return "";

    let deg = Math.atan2(dlon, dlat) * 180 / Math.PI;
    if (deg < 0) deg += 360;
    const box = ["N", "NE", "E", "SE", "S", "SW", "W", "NW"];
    const point = box[Math.round(deg / 45) % 8];
    const km = Math.sqrt(dlat * dlat + dlon * dlon) * 111.2;
    return "  heading " + Math.round(deg) + " deg (" + point + "), " +
           km.toFixed(1) + " km since this page opened (derived, not reported)";
}

export function renderMap(contacts, options) {
    const opts = options || {};
    const svg = svgEl("svg", {
        viewBox: "0 0 " + W + " " + H,
        class: "trackmap",
        preserveAspectRatio: "xMidYMid meet",
        role: "img",
        "aria-label": "Track picture: sensor coverage areas and contact positions",
    });

    svg.append(svgEl("rect", { x: 0, y: 0, width: W, height: H, class: "sea" }));

    const grid = svgEl("g", { class: "grat" });
    for (let lon = -140; lon <= -60; lon += 10) {
        const [x1, y1] = project(BOUNDS.latLo, lon);
        const [x2, y2] = project(BOUNDS.latHi, lon);
        grid.append(svgEl("line", { x1, y1, x2, y2 }));
        const t = svgEl("text", { x: x1 + 4, y: H - 8, class: "gratlab" });
        t.textContent = Math.abs(lon) + "°W";
        grid.append(t);
    }
    for (let lat = 50; lat <= 80; lat += 10) {
        const [x1, y1] = project(lat, BOUNDS.lonLo);
        const [x2, y2] = project(lat, BOUNDS.lonHi);
        grid.append(svgEl("line", { x1, y1, x2, y2 }));
        const t = svgEl("text", { x: 8, y: y1 - 5, class: "gratlab" });
        t.textContent = lat + "°N";
        grid.append(t);
    }
    svg.append(grid);

    const areas = svgEl("g", { class: "areas" });
    const ordered = [...REGIONS].sort(
        (a, b) => (b.lonHi - b.lonLo) * (b.latHi - b.latLo) -
                  (a.lonHi - a.lonLo) * (a.latHi - a.latLo));

    ordered.forEach((r) => {
        const [x1, y1] = project(r.latHi, r.lonLo);
        const [x2, y2] = project(r.latLo, r.lonHi);
        const live = opts.activeSensors && opts.activeSensors.has(r.id);
        const g = svgEl("g", { class: "area" + (live ? " live" : "") });
        g.append(svgEl("rect", {
            x: x1, y: y1, width: x2 - x1, height: y2 - y1,
            rx: 4,
            fill: SENSOR_COLOUR[r.id],
            "fill-opacity": live ? 0.1 : 0.04,
            stroke: SENSOR_COLOUR[r.id],
            "stroke-opacity": live ? 0.75 : 0.3,
            "stroke-width": 1.5,
            "stroke-dasharray": live ? null : "5 5",
        }));
        const lab = svgEl("text", { x: x1 + 8, y: y1 + 20, class: "arealab",
            fill: SENSOR_COLOUR[r.id], "fill-opacity": live ? 0.95 : 0.5 });
        lab.textContent = r.id;
        g.append(lab);
        title(g, r.id + " watches " + r.what + "  ·  " +
            r.latLo + " to " + r.latHi + " N, " +
            Math.abs(r.lonHi) + " to " + Math.abs(r.lonLo) + " W");
        areas.append(g);
    });
    svg.append(areas);

    const trails = opts.trails;
    const followed = opts.unknown || new Set();
    if (trails && trails.size) {
        const g = svgEl("g", { class: "trails" });
        contacts.forEach((c) => {
            if (!followed.has(c.id)) return;
            const pts = trails.get(c.id);
            if (!pts || pts.length < 2) return;
            const d = pts.map((p, i) => {
                const [x, y] = project(p.lat, p.lon);
                return (i ? "L" : "M") + x.toFixed(1) + " " + y.toFixed(1);
            }).join(" ");
            g.append(svgEl("path", {
                d: d, fill: "none",
                stroke: "#ffb020",
                class: "trail",
            }));
            const [ox, oy] = project(pts[0].lat, pts[0].lon);
            g.append(svgEl("circle", { cx: ox, cy: oy, r: 2.5, class: "trailfrom" }));
        });
        svg.append(g);
    }

    const routine = svgEl("g", { class: "contacts" });
    const urgent = svgEl("g", { class: "contacts priority" });
    const unknown = opts.unknown || new Set();
    const high    = opts.high || new Map();

    const staleMs = opts.staleMs || STALE_MS;
    const now = opts.now;
    const ageOf = (c) => (now && c.last_ns) ? (now - c.last_ns) / 1e6 : null;

    let hidden = 0, oldestShown = 0;
    const live = contacts.filter((c) => {
        const age = ageOf(c);
        if (age === null) return true;
        if (age > staleMs) { hidden++; return false; }
        if (age > oldestShown) oldestShown = age;
        return true;
    });
    const shown = live.slice(-1500);

    const PINGS = 6;
    const pinged = new Set(
        shown.filter((c) => c.last_ns)
             .sort((a, b) => b.last_ns - a.last_ns)
             .slice(0, PINGS).map((c) => c.id));

    shown.forEach((c) => {
        if (!isFinite(c.lat) || !isFinite(c.lon)) return;
        const [x, y] = project(c.lat, c.lon);
        const colour = SENSOR_COLOUR[c.sensor] || "#9fb0c4";
        const where = c.lat.toFixed(3) + ", " + c.lon.toFixed(3);
        const kind = c.sensor === "AIS" ? "vessel" : "aircraft";
        const foreign = unknown.has(c.id);

        const age = ageOf(c);
        let fade = 1;
        if (age !== null) {
            const t = (age / staleMs - FADE_FROM) / (1 - FADE_FROM);
            if (t > 0) fade = Math.max(0.25, 1 - 0.75 * Math.min(t, 1));
        }
        const agePart = age === null ? ""
            : "  last seen " + (age / 1000).toFixed(age < 10000 ? 1 : 0) + " s ago";

        const urgentNow = c.flag === "P";
        const everUrgent = !!c.priority;
        const name = c.id + "  " + c.sensor + "  " + (foreign ? "UNKNOWN  " : "") +
                     where + "  " + c.speed + " kn" + agePart +
                     (everUrgent && !urgentNow
                        ? "  (has raised priority earlier in this run)" : "");

        if (urgentNow) {
            const g = svgEl("g", { opacity: fade.toFixed(2) });
            if (pinged.has(c.id)) {
                g.append(svgEl("circle", { cx: x, cy: y, r: 7, class: "ping" }));
            }
            g.append(svgEl("path", {
                d: "M" + (x - 5) + " " + y + " L" + x + " " + (y - 5) +
                   " L" + (x + 5) + " " + y + " L" + x + " " + (y + 5) + " Z",
                fill: "#ff5f5f", stroke: "#fff", "stroke-width": 1.2,
            }));
            if (foreign) g.append(unknownFrame(x, y, c.id));
            title(g, c.id + "  " + c.sensor + "  PRIORITY  " +
                (foreign ? "UNKNOWN  " : "") +
                (high.has(c.id) ? "HIGH: in " + high.get(c.id) + "  " : "") +
                where + "  " + c.speed + " kn" + agePart);
            urgent.append(g);
        } else if (foreign) {
            const region = high.get(c.id);
            const g = svgEl("g", { opacity: fade.toFixed(2) });
            g.append(symbol(kind, x, y, region ? "#ff5f5f" : "#ffb020"));
            g.append(unknownFrame(x, y, c.id));
            title(g, name + (region ? "  HIGH: in " + region : "") +
                  headingOf(opts.trails, c.id));
            urgent.append(g);
        } else {
            const mark = symbol(kind, x, y, colour);
            if (fade < 1) mark.setAttribute("opacity", (0.85 * fade).toFixed(2));
            title(mark, name);
            routine.append(mark);
        }
    });
    svg.append(routine, urgent);

    const sites = svgEl("g", { class: "sites" });
    SITES.forEach((s) => {
        const [x, y] = project(s.lat, s.lon);
        const live = opts.activeSensors && opts.activeSensors.has(s.id);
        const g = svgEl("g", { class: "site" + (live ? " live" : "") });
        g.append(svgEl("circle", { cx: x, cy: y, r: 13, class: "halo" }));
        g.append(svgEl("circle", { cx: x, cy: y, r: 5.5,
            fill: live ? SENSOR_COLOUR[s.id] : "#0e2036",
            stroke: SENSOR_COLOUR[s.id], "stroke-width": 2 }));
        const t = svgEl("text", { x: x, y: y + 24, class: "sitelab" });
        t.textContent = s.id;
        g.append(t);
        title(g, s.name + " — " + s.where);
        sites.append(g);
    });

    const [cx, cy] = project(CENTRE.lat, CENTRE.lon);
    const c = svgEl("g", { class: "centre" });
    c.append(svgEl("path", {
        d: "M" + cx + " " + (cy - 7) + " L" + (cx + 7) + " " + cy +
           " L" + cx + " " + (cy + 7) + " L" + (cx - 7) + " " + cy + " Z",
        fill: "none", stroke: "#ffffff", "stroke-width": 2,
    }));
    title(c, CENTRE.name + " — " + CENTRE.where);
    sites.append(c);

    svg.append(sites);

    return {
        svg: svg,
        drawn: shown.length,
        total: contacts.length,
        hidden: hidden,
        staleMs: staleMs,
        oldestMs: oldestShown,
        aged: now ? true : false,
        urgent: shown.filter((c) => c.flag === "P").length,
        everUrgent: shown.filter((c) => c.priority && c.flag !== "P").length,
    };
}

export { SENSOR_COLOUR, SITES, REGIONS };

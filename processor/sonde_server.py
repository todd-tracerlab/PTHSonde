#!/usr/bin/env python3
"""
Sonde flight processor — local helper for the browser dashboard.

The dashboard (a browser app) can't run Python, create folders, or run SHARPpy,
so this small Flask server does it. Run it alongside the dashboard:

    pip install -r requirements.txt
    python sonde_server.py

Then in the dashboard: "New Flight" creates a UTC-named folder here, telemetry
streams into it, and "Process Flight" runs the sounding through SHARPpy, returns
the Skew-T + hodograph image, and prepends the computed indices to the CSV.

Endpoints (CORS-open for http://localhost):
  POST /flight/new       -> create flight_<UTC> folder, returns its name
  POST /flight/append    -> append one CSV line (and header) to the flight CSV
  POST /flight/process   -> run SHARPpy, save images, prepend indices, return them
  GET  /ping             -> health check
"""
import os, io, base64, datetime, math, traceback, json, subprocess
import threading, time, collections
from flask import Flask, request, jsonify, send_from_directory

try:
    import serial as _pyserial
    import serial.tools.list_ports as _list_ports
except Exception:                       # pyserial missing -> serial features disabled
    _pyserial = None
    _list_ports = None

import sys
# Resolve paths for both dev (python) and a frozen PyInstaller .exe.
#   RES     = where bundled resources live (sharppy_render.py, Dashboard/)
#   APP_DIR = where the app actually runs from (flights save here; venv310 sits here)
if getattr(sys, "frozen", False):
    RES = getattr(sys, "_MEIPASS", os.path.dirname(sys.executable))
    APP_DIR = os.path.dirname(sys.executable)
    DASH = os.path.join(RES, "Dashboard")
else:
    RES = os.path.dirname(os.path.abspath(__file__))
    APP_DIR = RES
    DASH = os.path.join(os.path.dirname(RES), "Dashboard")   # ..\Dashboard

app = Flask(__name__)
BASE = RES                                                   # bundled-resource root

# Default place to save flights: Documents\PTHSonde (created on demand).
DEFAULT_SAVE = os.path.join(os.path.expanduser("~"), "Documents", "PTHSonde")
STATE = {"flight_dir": None, "name": None, "save_dir": None}

# ---------------------------------------------------------------- CORS --------
@app.after_request
def _cors(resp):
    resp.headers["Access-Control-Allow-Origin"] = "*"
    resp.headers["Access-Control-Allow-Headers"] = "Content-Type"
    resp.headers["Access-Control-Allow-Methods"] = "POST, GET, OPTIONS"
    return resp

@app.route("/ping")
def ping():
    return jsonify({"ok": True, "flight": STATE["name"]})

@app.route("/config", methods=["GET", "POST", "OPTIONS"])
def config():
    if request.method == "OPTIONS":
        return ("", 204)
    if request.method == "GET":
        return jsonify({"ok": True, "save_dir": STATE.get("save_dir"),
                        "default": DEFAULT_SAVE,
                        "effective": STATE.get("save_dir") or DEFAULT_SAVE})
    body = request.get_json(force=True, silent=True) or {}
    sd = (body.get("save_dir") or "").strip()
    if sd:
        try:
            os.makedirs(sd, exist_ok=True)
        except OSError as e:
            return jsonify({"error": str(e)}), 400
        STATE["save_dir"] = sd
    else:
        STATE["save_dir"] = None
    return jsonify({"ok": True, "save_dir": STATE.get("save_dir")})

# =============================================================== serial I/O ===
# The server owns the COM port. A background thread keeps it open, reads CSV
# lines into a ring buffer, and auto-reconnects on any error. The dashboard
# opens/closes it and polls /serial/poll for new lines (by index). This replaces
# the browser's fragile Web Serial path.
SER = {
    "ser": None, "port": None, "baud": 115200,
    "thread": None, "want_open": False, "connected": False,
    "last_rx": 0.0, "error": None,
    "buf": collections.deque(maxlen=8000),   # recent lines (~6.5 h at 3 s)
    "total": 0,                              # total lines ever received
    "lock": threading.Lock(),
}

def _serial_reader():
    partial = b""
    while SER["want_open"]:
        ser = SER["ser"]
        if ser is None:                      # (re)open the port
            try:
                ser = _pyserial.Serial(SER["port"], SER["baud"], timeout=0.3)
                SER["ser"] = ser; SER["connected"] = True; SER["error"] = None
                partial = b""
            except Exception as e:
                SER["connected"] = False; SER["error"] = str(e)
                time.sleep(1.0); continue
        try:
            chunk = ser.read(256)            # blocks up to timeout=0.3 s
            if chunk:
                partial += chunk
                while b"\n" in partial:
                    raw, partial = partial.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if line:
                        with SER["lock"]:
                            SER["buf"].append(line)
                            SER["total"] += 1
                            SER["last_rx"] = time.time()
        except Exception as e:               # USB drop / device reset -> reopen
            SER["error"] = str(e); SER["connected"] = False
            try: ser.close()
            except Exception: pass
            SER["ser"] = None; time.sleep(0.5)
    try:
        if SER["ser"]: SER["ser"].close()
    except Exception: pass
    SER["ser"] = None; SER["connected"] = False

def _serial_stop():
    SER["want_open"] = False
    t = SER.get("thread")
    if t and t.is_alive():
        t.join(timeout=2.5)
    SER["thread"] = None; SER["connected"] = False

def _serial_start(port, baud):
    _serial_stop()
    SER["port"] = port; SER["baud"] = int(baud); SER["want_open"] = True
    t = threading.Thread(target=_serial_reader, daemon=True)
    SER["thread"] = t; t.start()

@app.route("/serial/ports")
def serial_ports():
    if not _list_ports:
        return jsonify({"ports": [], "error": "pyserial not installed"})
    ports = [{"device": p.device, "desc": (p.description or "")} for p in _list_ports.comports()]
    return jsonify({"ports": ports})

@app.route("/serial/open", methods=["POST", "OPTIONS"])
def serial_open():
    if request.method == "OPTIONS":
        return ("", 204)
    if not _pyserial:
        return jsonify({"error": "pyserial not installed"}), 500
    body = request.get_json(force=True, silent=True) or {}
    port = (body.get("port") or "").strip()
    baud = int(body.get("baud") or 115200)
    if not port:
        return jsonify({"error": "no port given"}), 400
    _serial_start(port, baud)
    time.sleep(0.5)                          # give it one open attempt
    return jsonify({"ok": True, "connected": SER["connected"],
                    "error": SER["error"], "port": port})

@app.route("/serial/close", methods=["POST", "OPTIONS"])
def serial_close():
    if request.method == "OPTIONS":
        return ("", 204)
    _serial_stop()
    return jsonify({"ok": True})

@app.route("/serial/poll")
def serial_poll():
    since = request.args.get("since", default=None, type=int)
    with SER["lock"]:
        total = SER["total"]
        buf = list(SER["buf"])
        if since is None or since < 0:
            since = total                    # default: only brand-new lines
        have_from = total - len(buf)         # global index of buf[0]
        start = since - have_from
        if start < 0:
            start = 0                        # caller fell behind the ring; give what we have
        lines = buf[start:] if start < len(buf) else []
        out = {"lines": lines, "next": total, "connected": SER["connected"],
               "port": SER["port"], "error": SER["error"],
               "last_age": (time.time() - SER["last_rx"]) if SER["last_rx"] else None}
    return jsonify(out)

@app.route("/serial/send", methods=["POST", "OPTIONS"])
def serial_send():
    if request.method == "OPTIONS":
        return ("", 204)
    body = request.get_json(force=True, silent=True) or {}
    text = body.get("text", "")
    ser = SER["ser"]
    if not ser or not SER["connected"]:
        return jsonify({"error": "serial not connected"}), 400
    try:
        ser.write((text + "\n").encode("utf-8")); ser.flush()
    except Exception as e:
        return jsonify({"error": str(e)}), 500
    return jsonify({"ok": True})

# ============================================================= static (HTML) ==
@app.route("/")
def index():
    return send_from_directory(DASH, "PTHSonde.html")

@app.route("/<path:fn>")
def asset(fn):
    # Serve real files from the Dashboard folder only; explicit API routes above
    # are more specific and always win, so this never shadows them.
    safe = os.path.normpath(os.path.join(DASH, fn))
    if not safe.startswith(DASH) or not os.path.isfile(safe):
        return ("not found", 404)
    return send_from_directory(DASH, fn)

# ------------------------------------------------------------ flight mgmt -----
@app.route("/flight/new", methods=["POST", "OPTIONS"])
def flight_new():
    if request.method == "OPTIONS":
        return ("", 204)
    name = datetime.datetime.now(datetime.timezone.utc).strftime("flight_%Y%m%d_%H%M%SZ")
    root = STATE.get("save_dir") or DEFAULT_SAVE
    path = os.path.join(root, name)
    os.makedirs(path, exist_ok=True)
    STATE["flight_dir"] = path
    STATE["name"] = name
    return jsonify({"name": name, "path": path})

@app.route("/flight/append", methods=["POST", "OPTIONS"])
def flight_append():
    if request.method == "OPTIONS":
        return ("", 204)
    if not STATE["flight_dir"]:
        return jsonify({"error": "no active flight"}), 400
    body = request.get_json(force=True, silent=True) or {}
    csv_path = os.path.join(STATE["flight_dir"], "data.csv")
    new = not os.path.exists(csv_path)
    with open(csv_path, "a", encoding="utf-8") as f:
        if new and body.get("header"):
            f.write(body["header"].rstrip("\n") + "\n")
        line = body.get("line")
        if line:
            f.write(line.rstrip("\n") + "\n")
    return jsonify({"ok": True})

@app.route("/flight/process", methods=["POST", "OPTIONS"])
def flight_process():
    if request.method == "OPTIONS":
        return ("", 204)
    if not STATE["flight_dir"]:
        return jsonify({"error": "No active flight. Click 'New Flight' first."}), 400
    body = request.get_json(force=True, silent=True) or {}
    csv_path = os.path.join(STATE["flight_dir"], "data.csv")
    # The dashboard may post the full dataset at process time; (re)write it.
    if body.get("rows"):
        with open(csv_path, "w", encoding="utf-8") as f:
            if body.get("header"):
                f.write(body["header"].rstrip("\n") + "\n")
            f.write("\n".join(r.rstrip("\n") for r in body["rows"]) + "\n")
    if not os.path.exists(csv_path):
        return jsonify({"error": "No data.csv in the flight folder yet."}), 400
    try:
        return jsonify(process_sounding(csv_path, STATE["flight_dir"]))
    except Exception as e:
        traceback.print_exc()
        return jsonify({"error": str(e)}), 500

# ------------------------------------------------------- sounding parsing -----
def _dewpoint(T, rh):
    if rh <= 0:
        return None
    a, b = 17.625, 243.04
    g = math.log(rh / 100.0) + (a * T) / (b + T)
    return (b * g) / (a - g)

def _read_csv(path):
    """Return list of dicts (data rows only), and the raw lines."""
    rows, raw = [], []
    header = None
    with open(path, "r", encoding="utf-8") as f:
        for ln in f:
            raw.append(ln.rstrip("\n"))
            s = ln.strip()
            if not s or s.startswith("#"):
                continue
            if header is None and s.startswith("rx_ms"):
                header = [c.strip() for c in s.split(",")]
                continue
            if header is None:
                continue
            cells = s.split(",")
            if len(cells) < len(header):
                continue
            rows.append({header[i]: cells[i] for i in range(len(header))})
    return rows, raw

def _num(d, k):
    v = d.get(k)
    if v is None or v == "" or v == "NA":
        return None
    try:
        return float(v)
    except ValueError:
        return None

# Centered moving-average window for RH / SHT-temp smoothing. The SHT41 transmits
# RH in 0.5% steps, which makes the humidity and (derived) dewpoint traces stair-
# step / look jagged; dithering them with a small centered average recovers a
# smooth curve. Applied at process time so the saved data.csv AND the SHARPpy
# dewpoint are smooth. Bump/drop this to taste.
RH_SMOOTH_WIN = 9
def _smooth_rows(rows, cols=("sht_rh_pct", "sht_temp_c"), win=RH_SMOOTH_WIN):
    """Smooth the given numeric columns in place (centered moving average).
    NA / non-numeric cells are left untouched and skipped in the average.
    sht_temp_c is smoothed too because it feeds the dewpoint calc; the primary
    thermistor air temperature (therm_temp_c) is deliberately left raw."""
    half = win // 2
    for col in cols:
        vals = [_num(d, col) for d in rows]
        fmt = "%.2f" if col == "sht_temp_c" else "%.1f"
        for i, d in enumerate(rows):
            if vals[i] is None:
                continue
            seg = [vals[j] for j in range(max(0, i - half), min(len(vals), i + half + 1))
                   if vals[j] is not None]
            if seg:
                d[col] = fmt % (sum(seg) / len(seg))

def _median(xs):
    xs = sorted(v for v in xs if v is not None)
    n = len(xs)
    if n == 0:
        return None
    m = n // 2
    return xs[m] if (n % 2) else 0.5 * (xs[m - 1] + xs[m])

def robust_surface(rows):
    """Derive a stable surface level (P, T, Td) from the near-launch samples.

    The SHARPpy parcel (SBCAPE/CIN/LCL ...) is very sensitive to surface T/Td, so
    a single noisy launch packet must not define it. We take every valid sample in
    the lowest-altitude band (highest pressure, within SURF_BAND_HPA of the max)
    and use the MEDIAN of each field -- robust to GPS/sensor spikes in a way a
    plain mean is not. Returns a level dict or None.
    """
    SURF_BAND_HPA  = 5.0     # ~40 m of altitude near the surface (primary window)
    SURF_WIDEN_HPA = 10.0    # widen to this (~80 m) if the primary band is sparse
    SURF_MIN_N     = 5       # ...but only widen the *pressure* window, never by count,
                             # so a fast ascent can't blow the surface up to altitude
    samp = []
    for d in rows:
        p_pa = _num(d, "ms_press_pa")
        if p_pa is None or _num(d, "v_ms") != 1:
            continue
        t = _num(d, "therm_temp_c") if _num(d, "v_therm") == 1 else None
        if t is None:
            t = _num(d, "sht_temp_c")
        samp.append({"p": p_pa / 100.0, "t": t,
                     "sht": _num(d, "sht_temp_c"), "rh": _num(d, "sht_rh_pct"),
                     "h": _num(d, "alt_m")})
    if not samp:
        return None
    pmax = max(s["p"] for s in samp)
    band = [s for s in samp if s["p"] >= pmax - SURF_BAND_HPA]
    if len(band) < SURF_MIN_N:                       # sparse -> widen the window, capped span
        band = [s for s in samp if s["p"] >= pmax - SURF_WIDEN_HPA]
    P  = _median([s["p"]   for s in band])
    T  = _median([s["t"]   for s in band])
    sht = _median([s["sht"] for s in band])
    rh = _median([s["rh"]  for s in band])
    h  = _median([s["h"]   for s in band])
    td = _dewpoint(sht, rh) if (sht is not None and rh) else None
    if P is None or T is None or td is None:
        return None
    return {"p": P, "t": T, "td": td,
            "h": (h if h is not None else 0.0), "wd": None, "ws": None, "n": len(band)}

def _average_winds(pts):
    """Replace each level's wind with the 500 m altitude-binned + 5-pt-smoothed
    wind, matching the dashboard's avgWinds(). Works in u,v space (knots, met FROM
    convention) so direction wrap is handled. Mutates pts in place."""
    samp = []
    for x in pts:
        if x["wd"] is None or x["ws"] is None or x["h"] is None:
            continue
        a = math.radians(x["wd"])
        samp.append((x["h"], -x["ws"] * math.sin(a), -x["ws"] * math.cos(a)))
    if len(samp) < 2:
        return
    bins = {}
    for z, u, v in samp:                                   # 500 m altitude bins
        b = bins.setdefault(round(z / 500.0), [0.0, 0.0, 0.0, 0])
        b[0] += z; b[1] += u; b[2] += v; b[3] += 1
    avg = sorted([[b[0]/b[3], b[1]/b[3], b[2]/b[3]] for b in bins.values()], key=lambda r: r[0])
    sm = []                                                # 5-point smoothing of u,v
    for i in range(len(avg)):
        su = sv = 0.0; c = 0
        for j in range(max(0, i - 2), min(len(avg), i + 3)):
            su += avg[j][1]; sv += avg[j][2]; c += 1
        sm.append((avg[i][0], su / c, sv / c))
    zs = [r[0] for r in sm]
    def interp(z):                                         # smoothed u,v at altitude z
        if z <= zs[0]:  return sm[0][1], sm[0][2]
        if z >= zs[-1]: return sm[-1][1], sm[-1][2]
        for i in range(1, len(zs)):
            if z <= zs[i]:
                t = (z - zs[i-1]) / (zs[i] - zs[i-1]) if zs[i] != zs[i-1] else 0.0
                return (sm[i-1][1] + t*(sm[i][1]-sm[i-1][1]),
                        sm[i-1][2] + t*(sm[i][2]-sm[i-1][2]))
        return sm[-1][1], sm[-1][2]
    for x in pts:
        if x["h"] is None:
            continue
        u, v = interp(x["h"])
        x["ws"] = math.hypot(u, v)
        x["wd"] = (math.degrees(math.atan2(-u, -v)) + 360.0) % 360.0

def _trim_to_ascent(rows, need=4, drop_hpa=0.2):
    """Drop pre-launch ground rows. On the pad the T/RH sensors are heat-soaked
    and erratic (e.g. a 42 C "surface" from sun-baking), which wrecks the surface
    parcel and every lifted index. Return rows starting at the launch = the first
    of `need` consecutive rising samples (pressure dropping >= drop_hpa/step).
    If no sustained ascent is found (short ground test), return rows unchanged."""
    run = 0
    start = None
    prev_p = None
    prev_i = None
    for i, d in enumerate(rows):
        p_pa = _num(d, "ms_press_pa")
        if p_pa is None:
            continue
        p = p_pa / 100.0
        if prev_p is not None and p < prev_p - drop_hpa:
            if run == 0:
                start = prev_i
            run += 1
            if run >= need:
                return rows[start:]
        else:
            run = 0
            start = None
        prev_p = p
        prev_i = i
    return rows

def _baro_alt(p_hpa):
    return 44330.0 * (1.0 - (p_hpa / 1013.25) ** 0.190284)

def _trim_soak(rows, fit_lo=800.0, fit_hi=3500.0, tol=1.5,
               end_run=3, cap_m=2500.0, min_fit=12):
    """Remove the sensor thermal-soak layer at the bottom of the ascent.

    The pad trim (_trim_to_ascent) drops rows while the sonde is *on the ground*,
    but the T/RH element keeps bleeding off accumulated pad heat for the first
    part of the climb -- a non-physical superadiabatic warm nose (this flight:
    35-42 C decaying at 100+ C/km). That wrecks the surface parcel and lifts.

    Method: fit the *real* lapse rate from the clean free atmosphere (800-3500 m
    above launch), extrapolate it down, and drop the contiguous bottom rows whose
    temperature sits > `tol` C ABOVE that clean profile (i.e. still soaked). If
    there isn't enough clean data yet, or the fit isn't a sane lapse, leave the
    rows untouched so short/ground-test files still render."""
    rows = _trim_to_ascent(rows)
    Z, T = [], []
    for d in rows:
        p_pa = _num(d, "ms_press_pa")
        z = _baro_alt(p_pa / 100.0) if p_pa is not None else None
        t = _num(d, "therm_temp_c")
        if t is None:
            t = _num(d, "sht_temp_c")
        Z.append(z); T.append(t)
    if not Z or Z[0] is None:
        return rows
    z0 = Z[0]
    xs = [z for z, t in zip(Z, T) if z is not None and t is not None and fit_lo <= z - z0 <= fit_hi]
    ys = [t for z, t in zip(Z, T) if z is not None and t is not None and fit_lo <= z - z0 <= fit_hi]
    if len(xs) < min_fit:
        return rows
    m = len(xs); sx = sum(xs); sy = sum(ys)
    sxx = sum(a * a for a in xs); sxy = sum(a * b for a, b in zip(xs, ys))
    den = m * sxx - sx * sx
    if den == 0:
        return rows
    slope = (m * sxy - sx * sy) / den
    inter = (sy - slope * sx) / m
    gamma = -slope * 1000.0                      # C/km
    if gamma <= 0 or gamma > 12:                 # not a sane lapse -> don't trust it
        return rows
    cut, clean = 0, 0
    for i in range(len(rows)):
        if Z[i] is None or T[i] is None:
            continue
        if Z[i] - z0 > cap_m:
            break
        if T[i] > (inter + slope * Z[i]) + tol:  # warmer than the clean profile -> soaked
            cut = i + 1; clean = 0
        else:
            clean += 1
            if clean >= end_run:
                break
    return rows[cut:]

def build_profile_arrays(rows):
    """Extract a clean, monotonic (decreasing pressure) sounding."""
    rows = _trim_soak(rows)               # skip pad rows AND the thermal-soak nose
    pts = []
    for d in rows:
        p_pa = _num(d, "ms_press_pa")
        if p_pa is None or _num(d, "v_ms") != 1:
            continue
        p = p_pa / 100.0                       # hPa
        t = _num(d, "therm_temp_c")            # thermistor = primary temp
        if t is None or _num(d, "v_therm") != 1:
            t = _num(d, "sht_temp_c")          # fall back to SHT
        sht = _num(d, "sht_temp_c")
        rh = _num(d, "sht_rh_pct")
        td = _dewpoint(sht, rh) if (sht is not None and rh) else None
        h = _num(d, "alt_m")
        if h is None or h == 0:                # fall back to barometric height
            h = 44330.0 * (1.0 - (p / 1013.25) ** 0.190284)
        wd = _num(d, "wind_deg")
        ws = _num(d, "speed_mps")
        if t is None or td is None:
            continue
        pts.append({"p": p, "t": t, "td": td, "h": h,
                    "wd": wd, "ws": (ws * 1.94384 if ws is not None else None)})  # m/s->kt
    if not pts:
        return []
    # ASCENT ONLY: keep launch -> peak (minimum pressure). Rows are in time order,
    # so truncate at the highest point; the parachute descent is noisy and would
    # double the profile back on itself.
    peak = min(range(len(pts)), key=lambda i: pts[i]["p"])
    pts = pts[:peak + 1]
    # sort by decreasing pressure (surface first), drop duplicates / non-monotonic
    pts.sort(key=lambda x: -x["p"])
    cleaned = []
    last_p = None
    for x in pts:
        if last_p is None or x["p"] < last_p - 0.1:
            cleaned.append(x)
            last_p = x["p"]
    # Robustify the surface parcel: keep the true surface pressure/height (the
    # highest-pressure level), but replace its T and Td with the MEDIAN of the
    # near-launch samples so a single noisy launch packet can't skew SBCAPE/LCL.
    surf = robust_surface(rows)
    if surf and cleaned:
        cleaned[0]["t"]  = surf["t"]
        cleaned[0]["td"] = surf["td"]
        cleaned[0]["n"]  = surf["n"]
    _average_winds(cleaned)              # 500 m bin + 5-pt smooth, matching the dashboard
    return cleaned

# ------------------------------------------------------------- indices --------
def sharppy_indices(pts):
    """Compute SHARPpy indices. Returns (flat_dict, extras) or raises."""
    import numpy as np
    import sharppy.sharptab.profile as profile
    import sharppy.sharptab.params as params
    import sharppy.sharptab.winds as winds
    import sharppy.sharptab.interp as interp
    import sharppy.sharptab.utils as utils

    pres = np.array([x["p"] for x in pts])
    tmpc = np.array([x["t"] for x in pts])
    dwpc = np.array([x["td"] for x in pts])
    hght = np.array([x["h"] if x["h"] is not None else np.nan for x in pts])
    wdir = np.array([x["wd"] if x["wd"] is not None else 0.0 for x in pts])
    wspd = np.array([x["ws"] if x["ws"] is not None else 0.0 for x in pts])

    prof = profile.create_profile(profile="default", pres=pres, hght=hght,
                                  tmpc=tmpc, dwpc=dwpc, wspd=wspd, wdir=wdir,
                                  missing=-9999, strictQC=False)
    sfc = params.parcelx(prof, flag=1)
    ml  = params.parcelx(prof, flag=4)
    mu  = params.parcelx(prof, flag=3)

    def f(v, nd=0):
        try:
            v = float(v)
            return None if (v != v or abs(v) > 1e8) else round(v, nd)
        except Exception:
            return None

    idx = {
        "SBCAPE (J/kg)": f(sfc.bplus), "SBCIN (J/kg)": f(sfc.bminus),
        "MLCAPE (J/kg)": f(ml.bplus), "MLCIN (J/kg)": f(ml.bminus),
        "MUCAPE (J/kg)": f(mu.bplus), "MUCIN (J/kg)": f(mu.bminus),
        "LCL (m)": f(sfc.lclhght), "LFC (m)": f(sfc.lfchght), "EL (m)": f(sfc.elhght),
        "Lifted Index (C)": f(sfc.li5, 1),
        "K-Index": f(params.k_index(prof), 1),
        "Total Totals": f(params.t_totals(prof), 1),
        "Precip Water (mm)": f((params.precip_water(prof) or 0) * 25.4, 1),
    }

    # ---- kinematics (best effort; missing winds -> skipped) ----
    storm = None
    def shear_mag(htop):
        try:
            uv = winds.wind_shear(prof, pbot=prof.pres[prof.sfc],
                                  ptop=interp.pres(prof, interp.to_msl(prof, htop)))
            return f(utils.mag(uv[0], uv[1]))
        except Exception:
            return None
    idx["0-1km Shear (kt)"] = shear_mag(1000.)
    idx["0-6km Shear (kt)"] = shear_mag(6000.)
    try:
        rstu, rstv, lstu, lstv = winds.non_parcel_bunkers_motion(prof)
        srh1 = winds.helicity(prof, 0, 1000, stu=rstu, stv=rstv)[0]
        srh3 = winds.helicity(prof, 0, 3000, stu=rstu, stv=rstv)[0]
        idx["0-1km SRH (m2/s2)"] = f(srh1)
        idx["0-3km SRH (m2/s2)"] = f(srh3)
        rd, rs = utils.comp2vec(rstu, rstv)
        ld, ls = utils.comp2vec(lstu, lstv)
        idx["Bunkers R (deg/kt)"] = "%03d/%d" % (rd, rs)
        idx["Bunkers L (deg/kt)"] = "%03d/%d" % (ld, ls)
        storm = {"rstu": float(rstu), "rstv": float(rstv), "lstu": float(lstu), "lstv": float(lstv)}
    except Exception:
        pass

    extras = {"storm": storm}
    try:
        extras["parcel"] = {"p": [float(x) for x in sfc.ptrace], "t": [float(x) for x in sfc.ttrace]}
    except Exception:
        extras["parcel"] = None
    try:
        extras["wetbulb"] = [float(x) for x in prof.wetbulb]
    except Exception:
        extras["wetbulb"] = None
    return idx, extras

# -------------------------------------------------------------- plotting ------
def _vec2comp(wd, ws):                       # met wind -> u,v (kt)
    a = math.radians(wd); return (-ws * math.sin(a), -ws * math.cos(a))

def render_images(pts, idx, extras, out_dir):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    BG, PANEL, GRID, TXT, DIM = "#0b0f15", "#11161f", "#2b3543", "#e7edf4", "#9aa7b6"
    pres = [x["p"] for x in pts]; tmpc = [x["t"] for x in pts]; dwpc = [x["td"] for x in pts]
    p0, ptop, skew = 1050.0, 100.0, 42.0
    span = math.log(p0) - math.log(ptop)
    yv = lambda p: math.log(p)
    X = lambda T, p: T + skew * (math.log(p0) - math.log(p)) / span

    fig = plt.figure(figsize=(13.6, 8.8), dpi=100); fig.patch.set_facecolor(BG)
    ax  = fig.add_axes([0.045, 0.05, 0.45, 0.90]); ax.set_facecolor(BG)
    hax = fig.add_axes([0.55, 0.55, 0.40, 0.40]); hax.set_facecolor(BG)
    tax = fig.add_axes([0.52, 0.05, 0.225, 0.44]); tax.set_facecolor(BG)
    kax = fig.add_axes([0.755, 0.05, 0.225, 0.44]); kax.set_facecolor(BG)

    # ---- Skew-T ----
    for pp in [1000, 925, 850, 700, 600, 500, 400, 300, 250, 200, 150, 100]:
        ax.plot([-60, 60], [yv(pp), yv(pp)], color=GRID, lw=0.6, zorder=1)
        ax.text(-58, yv(pp), str(pp), color=DIM, fontsize=7, va="center")
    for T in range(-100, 51, 10):
        ax.plot([X(T, p0), X(T, ptop)], [yv(p0), yv(ptop)],
                color=("#3f78c0" if T == 0 else GRID), lw=(1.1 if T == 0 else 0.5), zorder=1)
    for th in range(-40, 201, 20):
        xs, ys, p = [], [], p0
        while p >= ptop:
            Tc = (th + 273.15) * (p / 1000.0) ** 0.2854 - 273.15
            xs.append(X(Tc, p)); ys.append(yv(p)); p -= 10
        ax.plot(xs, ys, color="#33507a", lw=0.5, ls=(0, (4, 4)), zorder=1)
    if extras.get("wetbulb"):
        wb = extras["wetbulb"]
        ax.plot([X(w, p) for w, p in zip(wb, pres) if w is not None and abs(w) < 200],
                [yv(p) for w, p in zip(wb, pres) if w is not None and abs(w) < 200],
                color="#39b6c8", lw=1.0, zorder=3, label="Wetbulb")
    ax.plot([X(t, p) for t, p in zip(dwpc, pres)], [yv(p) for p in pres], color="#46c46a", lw=2.3, zorder=4, label="Dewpt")
    ax.plot([X(t, p) for t, p in zip(tmpc, pres)], [yv(p) for p in pres], color="#ff5a5a", lw=2.3, zorder=4, label="Temp")
    if extras.get("parcel") and extras["parcel"].get("p"):
        pc = extras["parcel"]
        ax.plot([X(t, p) for t, p in zip(pc["t"], pc["p"]) if p >= ptop],
                [yv(p) for p in pc["p"] if p >= ptop], color="#eaeaea", lw=1.3, ls="--", zorder=3, label="Parcel")
    ax.set_ylim(yv(p0), yv(ptop)); ax.set_xlim(-45, 50)
    ax.set_yticks([]); ax.set_xticks(range(-40, 41, 10)); ax.tick_params(colors=DIM, labelsize=8)
    for s in ax.spines.values(): s.set_color(GRID)
    ax.legend(loc="upper right", fontsize=8, facecolor=PANEL, edgecolor=GRID, labelcolor=TXT)

    # ---- Hodograph (height-colored) ----
    hax.set_title("Hodograph (kt)", color=TXT, fontsize=11, loc="left")
    for r in (10, 20, 30, 40, 50, 60):
        hax.add_artist(plt.Circle((0, 0), r, fill=False, color=GRID, lw=0.6))
    hax.axhline(0, color=GRID, lw=0.5); hax.axvline(0, color=GRID, lw=0.5)
    sfc_h = pts[0]["h"]
    bands = [(0, 3000, "#ff5a5a"), (3000, 6000, "#46c46a"), (6000, 9000, "#f5d24a"), (9000, 12000, "#39b6c8")]
    allu, allv = [], []
    for lo, hi, col in bands:
        seg_u, seg_v = [], []
        for x in pts:
            if x["wd"] is None or x["ws"] is None or x["h"] is None:
                continue
            agl = x["h"] - sfc_h
            if lo <= agl <= hi:
                u, v = _vec2comp(x["wd"], x["ws"]); seg_u.append(u); seg_v.append(v); allu.append(u); allv.append(v)
        if len(seg_u) > 1:
            hax.plot(seg_u, seg_v, color=col, lw=2.0)
    if extras.get("storm"):
        st = extras["storm"]
        hax.plot(st["rstu"], st["rstv"], "o", color="#ff5a5a", ms=6, mec="white", mew=0.8)
        hax.plot(st["lstu"], st["lstv"], "o", color="#4a9eff", ms=6, mec="white", mew=0.8)
    lim = max(40, (max([abs(u) for u in allu] + [abs(v) for v in allv]) + 8) if allu else 40)
    hax.set_xlim(-lim, lim); hax.set_ylim(-lim, lim); hax.set_aspect("equal")
    hax.tick_params(colors=DIM, labelsize=7)
    for s in hax.spines.values(): s.set_color(GRID)

    # ---- index panels ----
    def panel(pax, title, rows, accent):
        pax.axis("off"); pax.set_xlim(0, 1); pax.set_ylim(0, 1)
        pax.add_patch(plt.Rectangle((0, 0), 1, 1, facecolor=PANEL, edgecolor=GRID, lw=1.0))
        pax.text(0.5, 0.965, title, color=accent, fontsize=10.5, weight="bold",
                 ha="center", va="top", family="monospace")
        y = 0.88; step = 0.82 / max(len(rows), 1)
        for k, v in rows:
            pax.text(0.05, y, k, color=DIM, fontsize=9, va="top", family="monospace")
            pax.text(0.95, y, v, color=TXT, fontsize=9, va="top", ha="right", family="monospace", weight="bold")
            y -= step

    def g(key, unit="", nd=0):
        v = idx.get(key)
        if v is None:
            return "--"
        if isinstance(v, str):
            return v
        return ("%.*f%s" % (nd, v, unit)) if nd else ("%d%s" % (round(v), unit))

    panel(tax, "THERMODYNAMICS", [
        ("SBCAPE", g("SBCAPE (J/kg)")), ("SBCIN", g("SBCIN (J/kg)")),
        ("MLCAPE", g("MLCAPE (J/kg)")), ("MLCIN", g("MLCIN (J/kg)")),
        ("MUCAPE", g("MUCAPE (J/kg)")), ("MUCIN", g("MUCIN (J/kg)")),
        ("LCL", g("LCL (m)", "m")), ("LFC", g("LFC (m)", "m")), ("EL", g("EL (m)", "m")),
        ("Lifted Idx", g("Lifted Index (C)", "C", 1)),
        ("K-Index", g("K-Index", "", 1)), ("Tot Totals", g("Total Totals", "", 1)),
        ("PWAT", g("Precip Water (mm)", "mm", 1)),
    ], "#f5a623")
    panel(kax, "KINEMATICS", [
        ("0-1 SRH", g("0-1km SRH (m2/s2)")), ("0-3 SRH", g("0-3km SRH (m2/s2)")),
        ("0-1 Shear", g("0-1km Shear (kt)", "kt")), ("0-6 Shear", g("0-6km Shear (kt)", "kt")),
        ("Bunkers R", g("Bunkers R (deg/kt)")), ("Bunkers L", g("Bunkers L (deg/kt)")),
    ], "#4a9eff")

    fig.text(0.045, 0.975, "SOUNDING ANALYSIS  (SHARPpy)", color=TXT, fontsize=12, weight="bold", va="top")

    png_path = os.path.join(out_dir, "skewt.png")
    fig.savefig(png_path, facecolor=fig.get_facecolor())
    buf = io.BytesIO(); fig.savefig(buf, format="png", facecolor=fig.get_facecolor())
    plt.close(fig)
    return png_path, "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()

# ---- real SHARPpy GUI render (subprocess into the 3.10 venv) ----------------
def render_sharppy_real(pts, out_dir, lat, lon):
    """Render the genuine SHARPpy SPC panel via the 3.10 venv. Returns (png, err)."""
    # Find the SHARPpy venv. Search a few sensible spots so a frozen .exe can find
    # it whether venv310 sits next to the exe, one level up, or in the dev tree.
    cand = [os.environ.get("PTHSONDE_VENV"),
            os.path.join(APP_DIR, "venv310", "Scripts", "python.exe"),
            os.path.join(APP_DIR, "processor", "venv310", "Scripts", "python.exe"),
            os.path.join(os.path.dirname(APP_DIR), "venv310", "Scripts", "python.exe"),
            os.path.join(BASE, "venv310", "Scripts", "python.exe")]
    venv = next((c for c in cand if c and os.path.exists(c)), None)
    script = os.path.join(BASE, "sharppy_render.py")
    if not venv:
        return None, "venv310 not found (SHARPpy panel skipped; using matplotlib fallback)"
    levels = [{"p": x["p"], "h": (x["h"] if x["h"] is not None else 0.0),
               "t": x["t"], "td": x["td"],
               "wd": (x["wd"] if x["wd"] is not None else 0.0),
               "ws": (x["ws"] if x["ws"] is not None else 0.0)} for x in pts]
    payload = {"loc": "SONDE",
               "time": datetime.datetime.now(datetime.timezone.utc).strftime("%y%m%d/%H%M"),
               "lat": lat if lat is not None else 35.0,
               "lon": lon if lon is not None else -97.0,
               "levels": levels}
    injson = os.path.join(out_dir, "_sounding_in.json")
    outpng = os.path.join(out_dir, "sharppy.png")
    with open(injson, "w", encoding="utf-8") as f:
        json.dump(payload, f)
    try:
        r = subprocess.run([venv, script, injson, outpng],
                           capture_output=True, text=True, timeout=180)
    except Exception as e:
        return None, "render subprocess error: %s" % e
    try:
        os.remove(injson)
    except OSError:
        pass
    if os.path.exists(outpng) and os.path.getsize(outpng) > 2000:
        return outpng, None
    return None, "render failed: " + ((r.stderr or r.stdout or "")[-300:])

# ------------------------------------------------------------- pipeline -------
def process_sounding(csv_path, out_dir):
    rows, raw = _read_csv(csv_path)
    _smooth_rows(rows)                     # smooth RH + SHT temp (-> smooth dewpoint)
    pts = build_profile_arrays(rows)
    if len(pts) < 5:
        return {"error": "Not enough valid sounding levels yet (need pressure + temperature)."}

    idx, extras, sharppy_err = {}, {}, None
    try:
        idx, extras = sharppy_indices(pts)
    except ImportError:
        sharppy_err = "SHARPpy not installed (pip install sharppy). Image generated; indices skipped."
    except Exception as e:
        sharppy_err = "SHARPpy error: %s" % e

    # representative launch lat/lon for the panel header
    lat = lon = None
    for d in rows:
        la, lo = _num(d, "lat_deg"), _num(d, "lon_deg")
        if la and lo and (la != 0 or lo != 0):
            lat, lon = la, lo
            break

    # Prefer the GENUINE SHARPpy SPC panel (rendered in the 3.10 venv); fall
    # back to the matplotlib panel if that environment isn't available.
    realpng, realerr = render_sharppy_real(pts, out_dir, lat, lon)
    if realpng:
        png_path = realpng
        img_b64 = "data:image/png;base64," + base64.b64encode(open(realpng, "rb").read()).decode()
    else:
        png_path, img_b64 = render_images(pts, idx, extras, out_dir)
        sharppy_err = ((sharppy_err + " | ") if sharppy_err else "") + ("real render unavailable: %s" % realerr)

    # prepend indices to the top of the CSV as comment lines
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    sfc = pts[0]
    head = ["# ==== SOUNDING ANALYSIS (%s) ====" % stamp,
            "# levels=%d  top=%.0f hPa  surface=%.0f hPa" % (len(pts), pts[-1]["p"], sfc["p"]),
            "# surface  T=%.1fC  Td=%.1fC%s" % (sfc["t"], sfc["td"],
                ("  (median of %d near-launch samples)" % sfc["n"]) if sfc.get("n") else "  (single packet)")]
    if sharppy_err:
        head.append("# " + sharppy_err)
    for k, v in idx.items():
        head.append("# %-22s %s" % (k + ":", "NA" if v is None else v))
    head.append("# =====================================")
    # processed sounding profile (ascent only, winds 500 m-averaged) — the exact
    # data fed to SHARPpy. Comment-prefixed so a re-process safely skips it.
    prof_lines = ["# ==== PROCESSED PROFILE (ascent, 500 m-averaged winds, SHARPpy input) ====",
                  "# pres_hPa  height_m  temp_C  dewp_C  wdir_from  wspd_kt  wspd_mps"]
    for x in pts:
        prof_lines.append("# %8.1f %9.0f %7.1f %7.1f %9s %8s %9s" % (
            x["p"], (x["h"] if x["h"] is not None else 0.0), x["t"], x["td"],
            ("%.0f" % x["wd"]) if x["wd"] is not None else "NA",
            ("%.1f" % x["ws"]) if x["ws"] is not None else "NA",
            ("%.1f" % (x["ws"] / 1.94384)) if x["ws"] is not None else "NA"))
    prof_lines.append("# =====================================")
    # Rewrite the flight CSV: analysis header + the SMOOTHED data rows (clean
    # header + rows, so re-processing can't accumulate stale comment blocks) +
    # the processed profile. Reconstructed from the parsed rows so the saved
    # sht_rh_pct / sht_temp_c carry the smoothing.
    hdr = list(rows[0].keys()) if rows else []
    data_lines = [",".join(str(d.get(h, "")) for h in hdr) for d in rows]
    with open(csv_path, "w", encoding="utf-8") as f:
        f.write("\n".join(head) + "\n")
        if hdr:
            f.write(",".join(hdr) + "\n")
        f.write("\n".join(data_lines) + "\n")
        f.write("\n".join(prof_lines) + "\n")

    return {"flight": STATE["name"], "levels": len(pts),
            "indices": idx, "note": sharppy_err,
            "image": img_b64, "saved": {"png": png_path, "csv": csv_path}}

if __name__ == "__main__":
    print("Sonde processor on http://localhost:8765   (flights saved under %s)" % BASE)
    app.run(host="127.0.0.1", port=8765, threaded=True)

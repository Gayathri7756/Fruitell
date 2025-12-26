#!/usr/bin/env python3
# FRUITELL — robust Windows-friendly trainer:
# - Capture (label -> CSV)          : --port COM7 --out runs/session1.csv
# - Train from CSV (emit W: line)   : --csv runs/session1.csv
# - Live test with decimals         : --test --port COM7 --csv runs/session1.csv
# - Ping connection                 : --ping --port COM7
#
# pip install pyserial numpy scikit-learn

import argparse, os, sys, time, csv, threading, queue, re
from dataclasses import dataclass
import numpy as np
import serial, serial.tools.list_ports

# Optional sklearn for better convergence
try:
    from sklearn.linear_model import LogisticRegression
    HAVE_SK = True
except Exception:
    HAVE_SK = False

IS_WINDOWS = (os.name == "nt")
if IS_WINDOWS:
    import msvcrt  # non-blocking single key

# ---------------- Models & parsing ----------------

@dataclass
class Sample:
    echo_us: float
    label: int   # 1=fresh, 0=spoil
    fresh_anchor: float
    spoil_anchor: float

CSV_FIELDS = ["ts_ms","echo_us","mad_us","fresh_pct","conf_pct","fresh_anchor","spoil_anchor"]

HUMAN_RE = re.compile(
    r"Fresh\s*=\s*(?P<fresh>[\d.]+)\%.*?"
    r"Conf\s*=\s*(?P<conf>[\d.]+)\%.*?"
    r"Echo\s*=\s*(?P<echo>[\d.]+)\s*us.*?"
    r"MAD\s*=\s*(?P<mad>[\d.]+)\s*us.*?"
    r"F\s*=\s*(?P<F>\d+).*?"
    r"S\s*=\s*(?P<S>\d+)",
    re.IGNORECASE
)

def parse_device_csv_line(line: str):
    """Parse Arduino CSV: ts,echo,mad,fresh,conf,F,S"""
    try:
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 7:
            return None
        return {
            "ts_ms": int(float(parts[0])),
            "echo_us": float(parts[1]),
            "mad_us": float(parts[2]),
            "fresh_pct": float(parts[3]),
            "conf_pct": float(parts[4]),
            "fresh_anchor": float(parts[5]),
            "spoil_anchor": float(parts[6]),
        }
    except:
        return None

def parse_human_line(line: str):
    m = HUMAN_RE.search(line)
    if not m: return None
    try:
        return {
            "ts_ms": int(time.time()*1000),
            "echo_us": float(m.group("echo")),
            "mad_us": float(m.group("mad")),
            "fresh_pct": float(m.group("fresh")),
            "conf_pct": float(m.group("conf")),
            "fresh_anchor": float(m.group("F")),
            "spoil_anchor": float(m.group("S")),
        }
    except:
        return None

def scale_echo(echo_us: float, fa: float, sa: float) -> float:
    if fa == sa: sa = fa + 1.0
    lo, hi = (fa, sa) if fa < sa else (sa, fa)
    span = hi - lo
    x = (echo_us - lo) / span
    x = 0.0 if x < 0 else (1.0 if x > 1 else x)
    if fa < sa:  # firmware inverts if smaller echo = fresher
        x = 1.0 - x
    return x

def fit_logistic_1d(X, y):
    X = np.asarray(X).reshape(-1,1)
    y = np.asarray(y).astype(np.float64)
    if HAVE_SK:
        clf = LogisticRegression(solver="lbfgs", penalty="l2", C=1e3, max_iter=1000)
        clf.fit(X, y)
        return float(clf.coef_[0][0]), float(clf.intercept_[0])
    # fallback GD
    w=0.0; b=0.0; lr=0.5
    for _ in range(4000):
        z = b + w*X[:,0]
        yhat = 1.0/(1.0+np.exp(-z))
        db = np.mean(yhat - y)
        dw = np.mean((yhat - y) * X[:,0])
        w -= lr*dw; b -= lr*db; lr *= 0.9995
    return float(w), float(b)

def emit_W_line(w_fmed, b):
    weights = [0.0]*10
    weights[0] = w_fmed  # F_MED only
    w_txt = ",".join(f"{v:.6f}" for v in weights)
    return f"W:{w_txt},{b:.6f}"

# ---------------- I/O helpers ----------------

def autodetect_port():
    preferred_vids = {"1A86", "2341", "2A03", "10C4"}  # CH340, Arduino, CP210x
    for p in serial.tools.list_ports.comports():
        vid = f"{p.vid:04X}" if p.vid is not None else ""
        if vid in preferred_vids: return p.device
        if "CH340" in (p.description or "") or "Arduino" in (p.description or ""):
            return p.device
    return None

def open_port_with_retries(port, baud, tries=6, delay=1.0):
    last_err=None
    for i in range(1, tries+1):
        try:
            ser = serial.Serial(port, baud, timeout=0.15)
            # settle DTR/RTS (Windows lock fix)
            ser.setDTR(False); ser.setRTS(False); time.sleep(0.05)
            ser.setDTR(True);  ser.setRTS(True);  time.sleep(0.05)
            return ser
        except serial.SerialException as e:
            last_err = e
            print(f"[open retry {i}/{tries}] {e}")
            time.sleep(delay)
    raise last_err or RuntimeError("Failed to open port")

def save_csv(path, rows):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["echo_us","label","fresh_anchor","spoil_anchor"])
        for r in rows:
            w.writerow([f"{r.echo_us:.3f}", r.label, f"{r.fresh_anchor:.3f}", f"{r.spoil_anchor:.3f}"])
    print(f"[saved] {path}")

def load_csv(path):
    rows=[]
    with open(path, "r", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append(Sample(
                echo_us=float(row["echo_us"]),
                label=int(row["label"]),
                fresh_anchor=float(row.get("fresh_anchor", "1400")),
                spoil_anchor=float(row.get("spoil_anchor", "2600")),
            ))
    return rows

# ---------------- Key reader thread ----------------

def start_key_reader():
    """
    Returns a Queue that receives single-char keys: 'f','s','q'
    Works on Windows (instant, no Enter). On others, it reads lines and uses the first char.
    """
    q = queue.Queue()

    def run():
        if IS_WINDOWS:
            while True:
                if msvcrt.kbhit():
                    ch = msvcrt.getwch()
                    if ch:
                        q.put(ch.lower())
                else:
                    time.sleep(0.01)
        else:
            # fallback: read a line; take first non-space char
            while True:
                line = sys.stdin.readline()
                if not line:
                    time.sleep(0.05); continue
                ch = next((c for c in line.strip().lower() if c), "")
                if ch:
                    q.put(ch)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return q

# ---------------- Modes ----------------

def ping_only(args):
    port = args.port or autodetect_port()
    if not port:
        print("No serial port detected. Use --port COMx.")
        return
    print(f"[ping] Using {port} @ {args.baud}")
    ser = open_port_with_retries(port, args.baud)
    print(f"[ping] Connected to {ser.portstr}")
    time.sleep(2.8)  # <-- give the Nano time to reboot after serial open
    ser.reset_input_buffer(); ser.reset_output_buffer()
    ser.write(b"R\n")
    t_end = time.time()+1.5; got=False
    while time.time()<t_end:
        line = ser.readline().decode(errors="ignore").strip()
        if line:
            got=True; print("[device]", line)
    ser.close()
    print("[ping]", "OK" if got else "No response (check baud or firmware)")

def capture_mode(args):
    port = args.port or autodetect_port()
    if not port:
        print("No serial port. Use --port COMx."); sys.exit(2)

    print(f"[capture] opening {port} @ {args.baud}")
    ser = open_port_with_retries(port, args.baud)
    print(f"[capture] Connected to {ser.portstr}")
    # ADD:
    time.sleep(2.8)  # <-- give the Nano time to reboot after serial open
    ser.reset_input_buffer(); ser.reset_output_buffer()
    print("[capture] Serial buffers cleared")

    qrecs = queue.Queue()
    keyq  = start_key_reader()

    def reader():
        try:
            got_any = False
            last_tx = 0.0
            # Send TRAIN:ON once at start
            ser.write(b"TRAIN:ON\n")
            last_tx = time.time()
            while True:
                raw = ser.readline().decode(errors="ignore").strip()

                # If nothing yet, keep nudging every 1.5s (board may have missed it)
                if not raw and not got_any and (time.time() - last_tx) > 1.5:
                    ser.write(b"TRAIN:ON\n")
                    last_tx = time.time()
                    continue

                if not raw:
                    continue

                got_any = True
                rec = parse_device_csv_line(raw) or parse_human_line(raw)
                if rec:
                    qrecs.put(rec)
        except Exception:
            pass

    threading.Thread(target=reader, daemon=True).start()

    rows = []
    last_seen = 0.0
    rec_current = None
    saved = 0
    print("\nControls: tap f = fresh, s = spoil, q = stop (no Enter on Windows)\n")

    try:
        while True:
            # Get next usable record (respect --min-conf)
            try:
                rec = qrecs.get(timeout=0.25)
                last_seen = time.time()
                # compute a confidence proxy if the device is untrained
                conf = rec["conf_pct"] if rec["conf_pct"] > 0 else max(0.0, (1.0 - (rec["mad_us"]/(120.0*2))) * 100.0)
                if conf >= args.min_conf:
                    rec_current = rec
                    x = scale_echo(rec["echo_us"], rec["fresh_anchor"], rec["spoil_anchor"])
                    sys.stdout.write(
                        f"\rE={rec['echo_us']:7.2f}us  MAD={rec['mad_us']:6.2f}  x={x:.3f}  conf~{conf:5.1f}%  F={rec['fresh_anchor']:.0f}  S={rec['spoil_anchor']:.0f}  [f/s/q]: "
                    )
                    sys.stdout.flush()
            except queue.Empty:
                pass

            # Consume any key presses
            try:
                key = keyq.get_nowait()
            except queue.Empty:
                key = ""

            if key in ("f","s"):
                if rec_current:
                    lab = 1 if key=="f" else 0
                    rows.append(Sample(
                        echo_us=rec_current["echo_us"],
                        label=lab,
                        fresh_anchor=rec_current["fresh_anchor"],
                        spoil_anchor=rec_current["spoil_anchor"]
                    ))
                    saved += 1
                    print(f" +saved ({saved})")
                    rec_current = None
                else:
                    print(" (no sample ready yet)")
            elif key == "q":
                print("\n[stop] finishing capture…")
                break

            # Status hint if nothing incoming
            if (time.time() - last_seen) > 2.0:
                sys.stdout.write("\r(no data) Is the device RUNNING? Press the toggle button.      ")
                sys.stdout.flush()

    finally:
        try: ser.write(b"TRAIN:OFF\n"); print("\n[device] TRAIN:OFF")
        except: pass
        ser.close(); print("[serial] closed")

    if args.out:
        save_csv(args.out, rows)

def train_from_csv(args):
    rows = load_csv(args.csv)
    print(f"[train] loaded {len(rows)} rows from {args.csv}")
    if not rows:
        print("No rows to train."); return
    fa = np.median([r.fresh_anchor for r in rows])
    sa = np.median([r.spoil_anchor for r in rows])
    X = np.array([scale_echo(r.echo_us, fa, sa) for r in rows], float).reshape(-1,1)
    y = np.array([r.label for r in rows], int)
    if len(set(y.tolist())) < 2:
        print("Need both classes."); return
    w,b = fit_logistic_1d(X,y)
    z = b + w*X[:,0]; yhat = 1/(1+np.exp(-z))
    acc = np.mean(((yhat>=0.5).astype(int)==y))
    print(f"anchors used: F={fa:.1f} S={sa:.1f}")
    print(f"w(F_MED)={w:.6f}  bias={b:.6f}  acc={acc*100:.2f}%")
    print("Paste into Arduino Serial:")
    print(emit_W_line(w,b))

def live_test(args):
    rows = load_csv(args.csv) if args.csv else []
    fa = np.median([r.fresh_anchor for r in rows]) if rows else 1400
    sa = np.median([r.spoil_anchor for r in rows]) if rows else 2600
    port = args.port or autodetect_port()
    if not port:
        print("No serial port. Use --port COMx."); return
    print(f"[test] opening {port} @ {args.baud}")
    ser = open_port_with_retries(port, args.baud)
    print(f"[test] Connected to {ser.portstr}")
    time.sleep(2.8)  # <-- give the Nano time to reboot after serial open
    ser.reset_input_buffer(); ser.reset_output_buffer()
    ser.write(b"TRAIN:ON\n")
    print("Live test (Ctrl+C to quit)")
    try:
        while True:
            raw = ser.readline().decode(errors="ignore").strip()
            rec = parse_device_csv_line(raw) or parse_human_line(raw)
            if not rec: continue
            x = scale_echo(rec["echo_us"], fa, sa)
            fresh = rec["fresh_pct"] if rec["fresh_pct"]>0 else x*100.0
            conf  = rec["conf_pct"] if rec["conf_pct"]>0 else max(0.0, (1.0 - (rec["mad_us"]/(120.0*2))) * 100.0)
            print(f"E={rec['echo_us']:7.2f}us  MAD={rec['mad_us']:6.2f}   FreshProb={fresh:6.2f}%   Conf={conf:6.2f}%")
    except KeyboardInterrupt:
        pass
    finally:
        try: ser.write(b"TRAIN:OFF\n"); print("[device] TRAIN:OFF")
        except: pass
        ser.close(); print("[serial] closed")

# ---------------- CLI ----------------

def main():
    ap = argparse.ArgumentParser(description="FRUITELL trainer: capture -> CSV, train W:, live test")
    ap.add_argument("--port", help="Serial port (e.g., COM7 or /dev/ttyUSB0)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", help="Save labeled capture to CSV (capture mode)")
    ap.add_argument("--csv", help="Load CSV (train/test modes)")
    ap.add_argument("--ping", action="store_true", help="Test connection only")
    ap.add_argument("--test", action="store_true", help="Live test (decimal outputs)")
    ap.add_argument("--min-conf", type=float, default=60.0, help="Only prompt when conf >= this percent (default 60). Use 0 to take every reading.")
    args = ap.parse_args()

    if args.ping:
        ping_only(args); return
    if args.test:
        live_test(args); return
    if args.csv and not args.out:
        train_from_csv(args); return
    if args.out:
        capture_mode(args); return

    print("Nothing to do. Use --out for capture, --csv to train, or --test for live testing.")

if __name__ == "__main__":
    main()

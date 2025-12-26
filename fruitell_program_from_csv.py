# fruitell_program_from_csv.py (robust, fixed)
import argparse, time, csv, serial, os, sys

def read_rows(csv_path):
    out = []
    with open(csv_path, "r", newline="") as f:
        r = csv.reader(f)
        first = next(r, None)
        if first is None:
            return out

        def isnum(s):
            try:
                float(s); return True
            except:
                return False

        header = any(not isnum(c.strip()) for c in first)

        if header:
            hdr = [c.strip().lower().replace(" ", "_") for c in first]
            alias = {
                "echo_us": {"echo_us","echo","med_us","median_us"},
                "label": {"label","class","y"},
                "fresh_anchor": {"fresh_anchor","fresh","fresh_us","f_anchor"},
                "spoil_anchor": {"spoil_anchor","spoil","spoil_us","s_anchor"},
            }
            idx = {}
            for i, n in enumerate(hdr):
                for k, al in alias.items():
                    if n in al and k not in idx:
                        idx[k] = i
            if not all(k in idx for k in ("echo_us","label","fresh_anchor","spoil_anchor")):
                print("Bad header:", first, file=sys.stderr)
                return out

            for row in r:
                if len(row) < 4: continue
                try:
                    out.append((
                        int(float(row[idx["echo_us"]])),
                        int(float(row[idx["label"]])),
                        int(float(row[idx["fresh_anchor"]])),
                        int(float(row[idx["spoil_anchor"]])),
                    ))
                except:
                    pass
        else:
            raw_rows = [first] + list(r)   # <- use a separate list
            for row in raw_rows:
                if len(row) < 4: continue
                try:
                    out.append((
                        int(float(row[0])),
                        int(float(row[1])),
                        int(float(row[2])),
                        int(float(row[3])),
                    ))
                except:
                    pass
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--wait", type=float, default=12.0)
    ap.add_argument("--per_line_delay", type=float, default=0.08, help="seconds between rows")
    args = ap.parse_args()

    if not os.path.exists(args.csv):
        print("CSV not found:", args.csv, file=sys.stderr); sys.exit(1)

    rows = read_rows(args.csv)
    print("Parsed rows:", len(rows))
    if rows[:3]: print("First rows:", rows[:min(3, len(rows))])

    ser = serial.Serial(args.port, args.baud, timeout=0.4, write_timeout=2)
    try:
        time.sleep(3.0)  # let Nano reset
        ser.reset_input_buffer(); ser.reset_output_buffer()

        # BEGIN
        ser.write(b"CSVTEST:BEGIN\r\n"); ser.flush()

        # wait for READY (optional but nice)
        t0 = time.time()
        while time.time() - t0 < 3.0:
            ln = ser.readline().decode(errors="ignore").strip()
            if ln:
                print("[device]", ln)
                if "CSVTEST:READY" in ln:
                    break

        # send rows, slow & CRLF
        sent = 0
        for (echo, lab, fa, sa) in rows:
            line = f"{echo},{lab},{fa},{sa}\r\n".encode()
            ser.write(line); ser.flush()
            sent += 1
            time.sleep(args.per_line_delay)

        # END
        ser.write(b"CSVTEST:END\r\n"); ser.flush()

        # read device report
        t0 = time.time(); any_out = False
        while time.time() - t0 < args.wait:
            line = ser.readline().decode(errors="ignore").strip()
            if line:
                any_out = True
                print("[device]", line)

        # final status
        ser.write(b"R\r\n"); ser.flush(); time.sleep(0.2)
        t0 = time.time()
        while time.time() - t0 < 2.0:
            line = ser.readline().decode(errors="ignore").strip()
            if line: print("[device]", line)

        print(f"Sent rows: {sent}")
        if sent == 0:
            print("NOTE: 0 rows were sent. Check your CSV header/contents.", file=sys.stderr)
        if not any_out:
            print("NOTE: no CSVTEST report seen. If R shows TRAINED=0 & totals=0, "
                  "the device didn’t see rows/END (try larger per-line delay).", file=sys.stderr)
    finally:
        ser.close()

if __name__ == "__main__":
    main()

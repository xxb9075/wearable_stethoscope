"""Create a small simulated dataset in the layout used by frameset_creation.py.

Two subjects (S01, S02), two positions each, 20 s per recording at 4 kHz.
Each recording alternates inhale, exhale and pause phases made of band-limited
noise (inhale 400-900 Hz, exhale 150-400 Hz, pause: weak broadband noise), with
an annotation sheet listing the phases. The data only exercise the pipeline;
they are not physiological.

    python demo/make_demo_data.py --out demo/data
"""

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

FS = 4000
# label, duration (s), amplitude, band (Hz)
PHASES = (
    ("inhale", 1.2, 0.5, (400, 900)),
    ("exhale", 1.6, 0.3, (150, 400)),
    ("pause", 0.8, 0.02, (50, 1500)),
)


def band_noise(rng, n, band):
    """White noise restricted to a frequency band (FFT mask)."""
    spec = np.fft.rfft(rng.normal(size=n))
    f = np.fft.rfftfreq(n, 1 / FS)
    spec[(f < band[0]) | (f > band[1])] = 0
    x = np.fft.irfft(spec, n)
    return x / (np.std(x) + 1e-12)


def make_recording(rng, duration_s=20.0):
    """Return a signal and its phase annotations (label, start, end, maneuver)."""
    x = np.zeros(int(duration_s * FS))
    rows, t, i = [], 0.5, 0
    while True:
        label, length, amp, band = PHASES[i % 3]
        if t + length > duration_s - 0.5:
            break
        a, b = int(t * FS), int((t + length) * FS)
        x[a:b] = amp * band_noise(rng, b - a, band) * np.hanning(b - a)
        maneuver = "irregular" if t > 0.75 * duration_s else "normal"
        rows.append((label, t, t + length, maneuver))
        t += length
        i += 1
    x += 0.01 * rng.normal(size=x.size)
    return x, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default="demo/data", help="output folder")
    args = parser.parse_args()

    rng = np.random.default_rng(0)
    out = Path(args.out)
    for subject in ("S01", "S02"):
        for position in ("supine", "prone"):
            rec_dir = out / "fully_prepared" / subject / position
            ann_dir = out / "annotations" / subject / position
            rec_dir.mkdir(parents=True, exist_ok=True)
            ann_dir.mkdir(parents=True, exist_ok=True)

            x, rows = make_recording(rng)
            np.savetxt(rec_dir / "recording_MIC_post.csv", x, delimiter=",", fmt="%.6f")
            table = pd.DataFrame(
                [
                    dict(subject_id=subject, position=position, maneuver=m,
                         label=label, start_time=round(s, 3), end_time=round(e, 3))
                    for label, s, e, m in rows
                ]
            )
            table.to_excel(ann_dir / "recording_MIC_post.xlsx", index=False)
    print(f"Demo data written to {out}")


if __name__ == "__main__":
    main()

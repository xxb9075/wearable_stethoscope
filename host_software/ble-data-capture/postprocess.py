"""Offline post-processing of a recorded FLAC file.

Reads a ``recording.flac`` written by the capture app, subtracts the mean of
each channel (DC offset removal) and writes any of:

  * a DC-removed FLAC (``recording_dcremoved.flac``), lossless int16;
  * a CSV (``<session>_MIC_post.csv``) in the layout used by the analysis
    code: no header, column 0 is time in seconds (``sample_index / 4000``),
    columns 1..4 are the four DC-removed channels, 6 decimals;
  * an optional WAV copy and PNG preview.

The DC removal and the CSV export process the file in blocks, in two passes,
so a multi-hour recording is not loaded into memory at once. The optional WAV
copy is written from the whole DC-removed recording in one piece (about 115 MB
per hour of 4-channel int16 audio). Samples are read as int16
(``dtype="int16"``) to keep the raw ADC code scale; reading as ``float64``
would make libsndfile scale them to +/-1.

Usage::

    python ble-data-capture/postprocess.py data/<session>/recording.flac
    python ble-data-capture/postprocess.py data/<session>/recording.flac --csv
    python ble-data-capture/postprocess.py data/<session>/recording.flac --csv --wav --plot
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import soundfile as sf

_BLOCK = 1 << 18  # frames per streaming block (256k)


def _channel_means(path: Path, n_channels: int) -> np.ndarray:
    """First pass: mean of each channel over the whole file, in ADC codes."""
    total = np.zeros(n_channels, dtype=np.float64)
    count = 0
    for block in sf.blocks(str(path), blocksize=_BLOCK, dtype="int16"):
        block = np.atleast_2d(block)
        if block.shape[1] != n_channels:
            block = block.reshape(-1, n_channels)
        total += block.astype(np.float64).sum(axis=0)
        count += block.shape[0]
    if count == 0:
        return np.zeros(n_channels, dtype=np.float64)
    return total / count


def dc_remove(
    src: Path,
    write_wav: bool = False,
    plot: bool = False,
) -> Path:
    """DC-remove ``src`` and write ``*_dcremoved.flac`` next to it.

    Args:
        src: Path to ``recording.flac``.
        write_wav: Also write a 16-bit WAV copy.
        plot: Save a PNG preview (first 30 s) instead of showing a window.

    Returns:
        Path to the DC-removed FLAC.
    """
    src = Path(src)
    info = sf.info(str(src))
    n_channels = info.channels
    means = _channel_means(src, n_channels)

    dst = src.with_name(src.stem + "_dcremoved.flac")
    preview: list[np.ndarray] = []
    preview_frames = int(30 * info.samplerate)
    collected = 0

    with sf.SoundFile(
        str(dst),
        mode="w",
        samplerate=info.samplerate,
        channels=n_channels,
        subtype="PCM_16",
        format="FLAC",
    ) as out:
        for block in sf.blocks(str(src), blocksize=_BLOCK, dtype="int16"):
            block = np.atleast_2d(block)
            if block.shape[1] != n_channels:
                block = block.reshape(-1, n_channels)
            centered = block.astype(np.float64) - means
            clipped = np.clip(np.rint(centered), -32768, 32767).astype("<i2")
            out.write(clipped)
            if plot and collected < preview_frames:
                take = min(preview_frames - collected, clipped.shape[0])
                preview.append(clipped[:take])
                collected += take

    if write_wav:
        wav = src.with_name(src.stem + "_dcremoved.wav")
        data, sr = sf.read(str(dst), dtype="int16")
        sf.write(str(wav), data, sr, subtype="PCM_16")

    if plot and preview:
        _save_preview(np.vstack(preview), info.samplerate, src)

    print(f"DC means per channel: {np.round(means, 2)}")
    print(f"Wrote {dst}")
    return dst


def to_csv(src: Path, out_path: Path | None = None) -> Path:
    """Write a ``*_MIC_post.csv`` file next to ``src``.

    Layout: no header row, column 0 is time in seconds
    (``sample_index / sample_rate``), columns 1..N are the DC-removed channels,
    all with 6 decimal places. This is the input format of the analysis code.

    The file is written block by block, so a full night never sits in memory.

    Args:
        src: Path to ``recording.flac``.
        out_path: Destination CSV. Defaults to
            ``<session_dir>/<session_dir_name>_MIC_post.csv``.

    Returns:
        Path to the written CSV.
    """
    src = Path(src)
    info = sf.info(str(src))
    n_channels = info.channels
    sr = info.samplerate
    means = _channel_means(src, n_channels)

    if out_path is None:
        session = src.parent.name
        out_path = src.with_name(f"{session}_MIC_post.csv")
    out_path = Path(out_path)

    fmt = ",".join(["%.6f"] * (n_channels + 1))
    start = 0
    with open(out_path, "w", newline="") as fh:
        for block in sf.blocks(str(src), blocksize=_BLOCK, dtype="int16"):
            block = np.atleast_2d(block)
            if block.shape[1] != n_channels:
                block = block.reshape(-1, n_channels)
            n = block.shape[0]
            t = (start + np.arange(n, dtype=np.float64)) / sr
            centered = block.astype(np.float64) - means
            rows = np.column_stack([t, centered])
            np.savetxt(fh, rows, fmt=fmt, delimiter=",")
            start += n

    print(f"DC means per channel: {np.round(means, 2)}")
    print(f"Wrote {out_path} ({start} rows)")
    return out_path


def _save_preview(data: np.ndarray, samplerate: int, src: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")  # file output only
    import matplotlib.pyplot as plt

    n = data.shape[1]
    t = np.arange(data.shape[0]) / samplerate
    fig, axes = plt.subplots(n, 1, figsize=(12, 2 * n), sharex=True)
    axes = np.atleast_1d(axes)
    for c in range(n):
        axes[c].plot(t, data[:, c], linewidth=0.5)
        axes[c].set_ylabel(f"ch{c + 1}")
    axes[-1].set_xlabel("Time [s]")
    fig.suptitle(f"{src.parent.name} - DC-removed (first {int(t[-1])} s)")
    out = src.with_name(src.stem + "_dcremoved_preview.png")
    fig.tight_layout()
    fig.savefig(out, dpi=100)
    plt.close(fig)
    print(f"Wrote {out}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Post-process a recorded FLAC")
    parser.add_argument("flac", type=Path, help="Path to recording.flac")
    parser.add_argument(
        "--csv",
        action="store_true",
        help="Write a DC-removed <session>_MIC_post.csv.",
    )
    parser.add_argument(
        "--flac-out",
        action="store_true",
        help="Write a DC-removed FLAC (recording_dcremoved.flac).",
    )
    parser.add_argument("--wav", action="store_true", help="Also write a WAV")
    parser.add_argument(
        "--plot", action="store_true", help="Save a PNG preview (no window)"
    )
    args = parser.parse_args(argv)
    if not args.flac.exists():
        parser.error(f"file not found: {args.flac}")

    # Without output flags, write the DC-removed FLAC.
    want_flac = args.flac_out or args.wav or args.plot or not args.csv
    if want_flac:
        dc_remove(args.flac, write_wav=args.wav, plot=args.plot)
    if args.csv:
        to_csv(args.flac)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

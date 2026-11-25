# Source separation and respiratory phase classification

Source separation and respiratory phase classification code for *A Wearable Stethoscope for Source-Separated Body Acoustic Monitoring*.

This folder holds the analysis code behind the source separation (Fig. 3) and the respiratory phase segmentation and classification (Fig. 4) of the article.
The wearable stethoscope records four microphone channels at 4 kHz. The folder contains:

- **Source separation (MATLAB)**: normalisation, inter-channel ADC delay correction by linear interpolation, AMICA independent component analysis, band filtering into cardiac (DWT, 0.5-100 Hz) and respiratory (FFT, 100-1000 Hz) components, and selection of the respiratory component by respiratory-band SNR (Welch PSD, 200-600 Hz).
- **Respiratory signal conditioning and segmentation (MATLAB)**: spike removal, compression and Wiener denoising; pause detection from the Hilbert envelope (zero-phase FIR band-pass 100-1000 Hz, moving-average smoothing) with an adaptive percentile threshold.
- **Phase classification (Python)**: breath-phase framesets from annotated recordings, and a ResNet-34 classifier (exhale / inhale / pause) initialised from the RespireNet checkpoint, with conv1 to layer3 frozen and layer4 plus the classification head fine-tuned. Evaluation reports accuracy, normalised confusion matrices and one-vs-rest ROC AUC with micro- and macro-averages.

## Contents

```
preprocessing/                 MATLAB
  preprocess_ica_filter.m      delay correction, AMICA, low/high band filtering, IC SNR ranking, export
  compression_denoising.m      spike removal, compression, Wiener denoising, loudness normalisation
  segmentation.m               Hilbert-envelope pause detection and plot
  importfile.m                 reader for *_MIC_post.csv files from the host software
src/                           Python
  frameset_creation.py         annotated recordings -> framesets/{training,testing}/{exhale,inhale,pause}/*.npy
  npy_dataloader.py            .npy segment -> mel-spectrogram image (RespireNet style)
  nets/network_cnn.py          ResNet-34 classifier
  train.py                     transfer learning, validation, TensorBoard, Grad-CAM
  eval.py                      accuracy, confusion matrices, per-class recall, ROC / AUC
  utils.py                     spectrogram and normalisation helpers
demo/make_demo_data.py         small simulated dataset for the demo
```

## Pipeline

```
*_MIC_post.csv (host software, 4 channels)
  -> preprocess_ica_filter.m   -> data/preprocessed/<subject>/<position>/<recording>_respiratory.csv
                                  (highest-SNR IC; every IC is also saved as *_ic<k>.csv)
  -> compression_denoising.m   -> data/fully_prepared/<subject>/<position>/<recording>.csv
  -> segmentation.m            (pause detection, plot)
  -> manual phase annotation   -> data/annotations/<subject>/<position>/<recording>.xlsx
  -> src/frameset_creation.py  -> data/framesets/
  -> src/train.py, src/eval.py
```

The raw study recordings are not included in the repository (see Data availability in the paper).

## 1. System requirements

**Python (classification)**
- Tested on Windows 11 with Python 3.12.13 and [uv](https://docs.astral.sh/uv/) 0.11.
- Dependencies are pinned in `uv.lock`. Tested versions: torch 2.15.0, torchvision 0.29.2, librosa 1.0.1, numpy 2.5.5, opencv-python 5.1.0, scikit-learn 1.9.1, pandas 3.1.1, matplotlib 3.8.4 (cmapy requires matplotlib < 3.9), tensorboard 2.21.0, openpyxl 3.1.5.
- A CUDA GPU is recommended for training on the full dataset; the demo runs on a CPU.

**MATLAB (source separation and segmentation)**
- MATLAB R2024a with the Signal Processing Toolbox, Wavelet Toolbox, Audio Toolbox and Statistics and Machine Learning Toolbox.
- [EEGLAB](https://sccn.ucsd.edu/eeglab/) 2025.1.0 with the AMICA plugin 1.7 (`runamica15`), added to the MATLAB path.

No non-standard hardware is required.

## 2. Installation

Clone or download the repository, then run from this folder (`source_separation_classification/`):

```bash
uv sync
```

Download the RespireNet checkpoint (MIT License, Microsoft) used to initialise the classifier:

```bash
mkdir models
curl -L -o models/ckpt_best.pkl https://github.com/microsoft/RespireNet/raw/main/models/ckpt_best.pkl
# SHA-256: 4ae1a94a9f49ef4a51e54073b910382fce7d8d4f5d99d847d509e941a9d8f3ef
```

The first run also downloads the torchvision ImageNet ResNet-34 weights.
Typical install time is 2 to 5 minutes, depending on the network (about 20 s with a warm uv cache).
On Windows, PyPI provides CPU-only PyTorch; for GPU training install a CUDA build from <https://pytorch.org> into the environment.

For MATLAB, install EEGLAB and the AMICA plugin (EEGLAB menu File > Manage EEGLAB extensions) and add `preprocessing/` to the MATLAB path.

## 3. Demo

The demo runs the Python part on a small simulated dataset: 2 subjects, 2 positions, 20 s per recording.
The simulated recordings are single-channel conditioned signals, the input of `frameset_creation.py`; the MATLAB source separation has no demo data and runs on four-channel recordings from the host software.

```bash
uv run python demo/make_demo_data.py --out demo/data
uv run python src/frameset_creation.py --data_root demo/data/fully_prepared --annotation_root demo/data/annotations --export_root demo/data/framesets --test_subjects S02
cd src
uv run python train.py --data_dir ../demo/data/framesets --model_path ../outputs/demo --checkpoint ../models/ckpt_best.pkl --epochs 5 --batch_size 8 --num_worker 0
uv run python eval.py --data_dir ../demo/data/framesets --checkpoint ../outputs/demo/ckpt_best_5_-1.pkl --num_worker 0 --use_train_stats --split testing --out_dir ../outputs/eval
```

Expected output:
- `demo/data/framesets/` with 26 training and 34 testing segments.
- `train.py` prints the normalisation statistics, `Trainable params: 13196931` (layer4 and the head), and per-epoch training and validation accuracy and confusion matrices. It writes the best checkpoint `outputs/demo/ckpt_best_5_-1.pkl`, TensorBoard logs in `outputs/demo/runs/`, and Grad-CAM images in `outputs/demo/gradcam/epoch_XXX/{exhale,inhale,pause}/`.
- `eval.py` prints accuracy, cross-entropy loss, the raw and row-normalised confusion matrices, per-class recall, and the one-vs-rest AUC of each class with the micro- and macro-average, for the testing split. `outputs/eval/` holds `metrics.json`, `predictions.csv` (class probabilities per segment), `roc_curves.csv` and `roc.png`.

The three simulated phases differ in frequency band, so the classifier learns them partly within five epochs (in our run: accuracy 0.47, micro-average AUC 0.71 on the testing split). Values vary between machines and are not meaningful for respiratory data.
Expected run time: about 2 minutes on a desktop CPU.

## 4. Instructions for use

1. **Source separation.** In MATLAB, set `dataFilepath` and `exportFolder` at the top of `preprocess_ica_filter.m` and run it from this folder. It prints the SNR table of the independent components and writes the high band of the highest-SNR component as `<recording>_respiratory.csv`, the high band of every component as `<recording>_ic<k>.csv`, and a plot of all components in both bands.
2. **Conditioning.** Set `subject`, `position` and `filename` (the original recording name) in `compression_denoising.m` and run it. Input: `<recording>_respiratory.csv`. Output: `data/fully_prepared/<subject>/<position>/<recording>.csv` and `.wav`.
3. **Segmentation.** Set `dataPath` in `segmentation.m` to view the envelope and the detected pauses. The script analyses 2 to 37 s of the recording (`detection_start`, `detection_end`); adjust these for shorter recordings.
4. **Annotation.** Each `<subject>/<position>/` folder holds one recording. Phase labels are given per recording in `data/annotations/<subject>/<position>/<recording>.xlsx` with the columns `subject_id`, `position`, `maneuver`, `label` (`inhale`, `exhale`, `pause`), `start_time` and `end_time` in seconds.
5. **Framesets.** `uv run python src/frameset_creation.py --test_subjects <subject> ...` writes one `.npy` file per phase. Segments of the listed subjects and of the `irregular` maneuver go to `testing/`, all others to `training/`.
6. **Training.** From `src/`: `uv run python train.py --data_dir ../data/framesets --model_path ../outputs/<run> --checkpoint ../models/ckpt_best.pkl` (defaults: 40 epochs, batch size 16, learning rate 1e-3, SGD with momentum 0.9, weight decay 5e-4, 20 % validation split, seed 42). conv1 and the residual blocks up to layer3 are frozen; layer4, the 128-unit head and the classification layer are fine-tuned with class-weighted cross-entropy. The checkpoint with the best validation accuracy is kept. `train.py` prints the normalisation `MEAN` and `STD`.
7. **Evaluation.** `uv run python eval.py --data_dir ../data/framesets --checkpoint ../outputs/<run>/ckpt_best_40_-1.pkl --use_train_stats --split validation --out_dir ../outputs/<run>/eval`. `--split validation` evaluates the held-out 20 % of the training folder (the same random split as `train.py`; pass the same `--val_ratio` and `--seed`), `--split testing` the testing folder. `--use_train_stats` recomputes the training normalisation as `train.py` does; `--mean` and `--std` can be given instead.

Run `python <script> --help` for all options.

## Other parts of the repository

- [`host_software/`](../host_software/): BLE host software that records the `*_MIC_post.csv` input files.
- [`firmware/`](../firmware/): device firmware (nRF52840, nRF Connect SDK).

## License and acknowledgements

Released under the MIT License (see `LICENSE`).
The classification code is adapted from [RespireNet](https://github.com/microsoft/RespireNet) (Gairola et al., 2021; MIT License, Copyright (c) Microsoft Corporation).
The trained phase classifier is not distributed; it can be trained with step 6.
Source separation uses AMICA (Palmer et al.) through EEGLAB.

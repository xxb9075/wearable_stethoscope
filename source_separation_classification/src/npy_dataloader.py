"""Dataset of breath-phase segments stored as .npy waveforms.

Each segment is padded or cropped to a fixed length and converted to a
mel-spectrogram image with blank region clipping, the same image
representation as RespireNet.
"""
import os
import random

import numpy as np
import cv2
import torch
from torch.utils.data import Dataset

from utils import create_mel_raw


class NpyBreathDataset(Dataset):
    """Breath-phase segments stored as root/split/class_name/*.npy.

    Args:
        root: Frameset folder, e.g. ``data/framesets``.
        split: Sub-folder to read, ``"training"`` or ``"testing"``.
        class_names: Class folders; the order defines the labels.
        sample_rate: Sampling rate of the waveforms in Hz.
        desired_length: Segment length in seconds after padding or cropping.
        n_mels, f_max, nfft, hop: Mel-spectrogram settings.
        input_transform: Transform applied to the image.
        augment: Random time reversal with probability 0.5.
        dump_images, dump_dir: Optionally save the images as PNG files.
    """
    def __init__(
        self,
        root,
        split="training",
        class_names=("exhale", "inhale", "pause"),
        sample_rate=4000,
        desired_length=8.0,   # seconds
        n_mels=64,
        f_max=2000,
        nfft=256,
        hop=None,
        input_transform=None,
        augment=False,
        dump_images=False,
        dump_dir=None,
    ):
        super().__init__()
        self.root = root
        self.split = split
        self.class_names = list(class_names)
        self.label_map = {c: i for i, c in enumerate(self.class_names)}

        self.sample_rate = sample_rate
        self.desired_length = desired_length
        self.n_mels = n_mels
        self.f_max = f_max
        self.nfft = nfft
        self.hop = hop if hop is not None else nfft // 2

        self.input_transform = input_transform
        self.augment = augment
        self.dump_images = dump_images
        self.dump_dir = dump_dir

        self.target_len = int(self.sample_rate * self.desired_length)

        self.samples = []  # (file path, label)

        self._collect_files()

        # Labels and class counts (same attributes as the RespireNet dataset)
        self.labels = [label for _, label in self.samples]

        num_classes = len(self.class_names)
        self.class_probs = np.zeros(num_classes, dtype=np.float32)
        for y in self.labels:
            self.class_probs[y] += 1.0

        # Identifiers and (file, label) pairs, for logging
        self.identifiers = []
        self.filenames_with_labels = []
        for path, label in self.samples:
            base = os.path.basename(path)
            self.identifiers.append(f"{base}_{label}")
            self.filenames_with_labels.append((path, label))

    # File discovery
    def _collect_files(self):
        base = os.path.join(self.root, self.split)
        if not os.path.isdir(base):
            raise RuntimeError(f"Split directory not found: {base}")

        for cls in self.class_names:
            cls_dir = os.path.join(base, cls)
            if not os.path.isdir(cls_dir):
                print(f"[WARN] Class directory missing: {cls_dir}")
                continue

            label = self.label_map[cls]
            for fname in sorted(os.listdir(cls_dir)):  # fixed order for a reproducible random_split
                if not fname.endswith(".npy"):
                    continue
                fpath = os.path.join(cls_dir, fname)
                self.samples.append((fpath, label))

        if not self.samples:
            raise RuntimeError(f"No .npy files found under {base}")

        print(f"[NpyBreathDataset] split='{self.split}' -> {len(self.samples)} samples")
        for cls in self.class_names:
            n_cls = sum(1 for _, y in self.samples if y == self.label_map[cls])
            print(f"  class '{cls}' (label={self.label_map[cls]}): {n_cls} files")

    # Audio helpers
    def _load_waveform(self, path):
        x = np.load(path)
        x = np.asarray(x, dtype=np.float32)

        # Flatten segments stored as 2-D arrays
        if x.ndim > 1:
            x = x.reshape(-1)

        # Peak amplitude normalisation
        max_abs = np.max(np.abs(x)) if x.size > 0 else 0.0
        if max_abs > 0:
            x = x / max_abs

        # Zero-pad or randomly crop to the fixed length
        if x.shape[0] < self.target_len:
            pad = self.target_len - x.shape[0]
            x = np.pad(x, (0, pad), mode="constant")
        elif x.shape[0] > self.target_len:
            start = random.randint(0, x.shape[0] - self.target_len)
            x = x[start:start + self.target_len]

        return x

    def _waveform_to_image(self, audio):
        """Mel-spectrogram image with blank region clipping, as in RespireNet."""
        # RGB mel image, upscaled 3x
        audio_image = cv2.cvtColor(
            create_mel_raw(
                audio,
                self.sample_rate,
                f_max=self.f_max,
                n_mels=self.n_mels,
                nfft=self.nfft,
                hop=self.hop,
                resz=3,
            ),
            cv2.COLOR_BGR2RGB,
        )

        # Grayscale mel image used to find the blank region
        audio_raw_gray = cv2.cvtColor(
            create_mel_raw(
                audio,
                self.sample_rate,
                f_max=self.f_max,
                n_mels=self.n_mels,
                nfft=self.nfft,
                hop=self.hop,
            ),
            cv2.COLOR_BGR2GRAY,
        )

        audio_raw_gray[audio_raw_gray < 10] = 0

        # First row that is less than 80 % black
        row = 0
        for r in range(audio_raw_gray.shape[0]):
            black_percent = np.mean(audio_raw_gray[r, :] == 0)
            if black_percent < 0.80:
                row = r
                break

        # Clip the blank rows (x3 because the RGB image is upscaled 3x)
        if (row + 1) * 3 < audio_image.shape[0]:
            audio_image = audio_image[(row + 1) * 3 :, :, :]

        # Resize back to the fixed height
        audio_image = cv2.resize(
            audio_image,
            (audio_image.shape[1], self.n_mels * 3),
            interpolation=cv2.INTER_LINEAR,
        )

        return audio_image

    # Dataset API
    def __len__(self):
        return len(self.samples)

    def __getitem__(self, index):
        path, label = self.samples[index]

        audio = self._load_waveform(path)

        # Augmentation: reverse in time with probability 0.5
        if self.augment:
            if random.random() < 0.5:
                audio = audio[::-1].copy()

        img = self._waveform_to_image(audio)

        if self.dump_images:
            if self.dump_dir is not None:
                os.makedirs(self.dump_dir, exist_ok=True)
                base = os.path.basename(path).replace(".npy", ".png")
                out_path = os.path.join(self.dump_dir, base)
                cv2.imwrite(out_path, cv2.cvtColor(img, cv2.COLOR_RGB2BGR))

        if self.input_transform is not None:
            img = self.input_transform(img)

        # Without a transform, convert to a float tensor in [0, 1]
        if isinstance(img, np.ndarray):
            # HWC -> CHW
            img = torch.from_numpy(img.transpose(2, 0, 1)).float() / 255.0

        return img, label

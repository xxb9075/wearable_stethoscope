"""Evaluate a trained respiratory phase classifier.

Reports overall accuracy, cross-entropy loss, the confusion matrix (raw and
row-normalised; rows = true, columns = predicted), per-class accuracy (recall)
and one-vs-rest ROC AUC for each class with micro- and macro-averages.

The evaluated set is one of the frameset folders (``training`` or
``testing``) or ``validation``: the held-out part of the training folder,
rebuilt with the same random split as train.py (same --val_ratio and --seed).

Example, run from src/:

    python eval.py --data_dir ../data/framesets --checkpoint ../outputs/ckpt_best_40_-1.pkl --split validation --use_train_stats --out_dir ../outputs/eval
"""
import os
import argparse
import csv
import json
import numpy as np
from tqdm import tqdm

import torch
import torch.nn as nn
from torch.utils.data import DataLoader, random_split
from torchvision.transforms import Compose, Normalize, ToTensor
from sklearn.metrics import confusion_matrix, accuracy_score, roc_auc_score, roc_curve
from sklearn.preprocessing import label_binarize

from npy_dataloader import NpyBreathDataset
from nets.network_cnn import model
from utils import get_mean_and_std


# Mean and standard deviation per channel, pooled over all pixels of a dataset.
# Not used any more: --use_train_stats now calls utils.get_mean_and_std, the
# estimate used by train.py, so that evaluation uses the training normalisation.
@torch.no_grad()
def compute_mean_std(dataset, batch_size=32, num_workers=4):
    loader = DataLoader(dataset, batch_size=batch_size, shuffle=False, num_workers=num_workers)
    n = 0
    mean = torch.zeros(3)
    m2 = torch.zeros(3)  # variance

    for x, _ in tqdm(loader, desc="==> Computing mean/std from TRAIN", leave=False):
        # x: [B, C, H, W]
        b, c, h, w = x.shape
        x = x.float()
        x = x.permute(1, 0, 2, 3).contiguous().view(c, -1)  # (C, B*H*W)

        batch_n = x.shape[1]
        batch_mean = x.mean(dim=1)
        batch_var = x.var(dim=1, unbiased=False)

        # Merge with the running statistics
        if n == 0:
            mean = batch_mean
            m2 = batch_var
            n = batch_n
        else:
            delta = batch_mean - mean
            total = n + batch_n
            mean = mean + delta * (batch_n / total)
            m2 = (n * m2 + batch_n * batch_var + (delta ** 2) * (n * batch_n / total)) / total
            n = total

    std = torch.sqrt(m2 + 1e-12)
    return mean, std


def clean_state_dict_for_load(sd, net):
    """Match the "module." key prefix of a checkpoint to the model (DataParallel or not)."""
    if isinstance(sd, dict) and "state_dict" in sd:
        sd = sd["state_dict"]

    model_keys = set(net.state_dict().keys())
    ckpt_keys = set(sd.keys())

    # Checkpoint has the prefix, model does not
    if any(k.startswith("module.") for k in ckpt_keys) and not any(k.startswith("module.") for k in model_keys):
        sd = {k.replace("module.", "", 1): v for k, v in sd.items()}

    # Model has the prefix, checkpoint does not
    if (not any(k.startswith("module.") for k in ckpt_keys)) and any(k.startswith("module.") for k in model_keys):
        sd = {"module." + k: v for k, v in sd.items()}

    return sd


@torch.no_grad()
def evaluate(net, loader, device, class_names):
    net.eval()

    all_y = []
    all_p = []
    all_prob = []

    total_loss = 0.0
    total_n = 0

    criterion = nn.CrossEntropyLoss()

    for x, y in tqdm(loader, desc="Evaluating", leave=False):
        x = x.to(device, non_blocking=True)
        y = y.to(device, non_blocking=True)

        logits = net(x)
        loss = criterion(logits, y)

        p = torch.argmax(logits, dim=1)

        all_y.append(y.cpu().numpy())
        all_p.append(p.cpu().numpy())
        all_prob.append(torch.softmax(logits, dim=1).cpu().numpy())

        bs = y.size(0)
        total_loss += float(loss.item()) * bs
        total_n += bs

    y_true = np.concatenate(all_y)
    y_pred = np.concatenate(all_p)
    y_prob = np.concatenate(all_prob)

    acc = accuracy_score(y_true, y_pred)
    cm = confusion_matrix(y_true, y_pred, labels=list(range(len(class_names))))

    # Per-class accuracy
    per_class_acc = []
    for i in range(len(class_names)):
        denom = cm[i].sum()
        per_class_acc.append((cm[i, i] / denom) if denom > 0 else float("nan"))

    mean_loss = total_loss / max(total_n, 1)

    return acc, mean_loss, cm, per_class_acc, y_true, y_pred, y_prob


def roc_analysis(y_true, y_prob, class_names):
    """One-vs-rest ROC curves and AUC per class, plus micro- and macro-averages.

    Returns (auc, curves). auc maps each class name, "micro" and "macro" to
    the AUC (NaN for a class missing from y_true); curves maps the class
    names and "micro" to (fpr, tpr) arrays.
    """
    n_classes = len(class_names)
    y_bin = label_binarize(y_true, classes=list(range(n_classes)))
    auc, curves = {}, {}

    for c, name in enumerate(class_names):
        if 0 < y_bin[:, c].sum() < len(y_bin):
            auc[name] = float(roc_auc_score(y_bin[:, c], y_prob[:, c]))
            fpr, tpr, _ = roc_curve(y_bin[:, c], y_prob[:, c])
            curves[name] = (fpr, tpr)
        else:
            auc[name] = float("nan")

    # Micro-average: every (sample, class) decision pooled into one binary problem
    fpr, tpr, _ = roc_curve(y_bin.ravel(), y_prob.ravel())
    auc["micro"] = float(roc_auc_score(y_bin.ravel(), y_prob.ravel()))
    curves["micro"] = (fpr, tpr)

    # Macro-average: unweighted mean of the per-class AUCs
    per_class = [auc[name] for name in class_names if not np.isnan(auc[name])]
    auc["macro"] = float(np.mean(per_class)) if per_class else float("nan")
    return auc, curves


def save_outputs(out_dir, paths, class_names, y_true, y_pred, y_prob, metrics, curves):
    """Write predictions.csv, roc_curves.csv, metrics.json and roc.png to out_dir."""
    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(out_dir, "predictions.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", "true", "pred"] + [f"p_{n}" for n in class_names])
        for path, t, p, prob in zip(paths, y_true, y_pred, y_prob):
            w.writerow([os.path.basename(path), class_names[t], class_names[p]] + [f"{v:.6f}" for v in prob])

    with open(os.path.join(out_dir, "roc_curves.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["curve", "fpr", "tpr"])
        for name, (fpr, tpr) in curves.items():
            for a, b in zip(fpr, tpr):
                w.writerow([name, f"{a:.6f}", f"{b:.6f}"])

    with open(os.path.join(out_dir, "metrics.json"), "w") as f:
        json.dump(metrics, f, indent=2)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(4, 4))
    for name, (fpr, tpr) in curves.items():
        ax.plot(fpr, tpr, label=f"{name} (AUC = {metrics['auc'][name]:.3f})")
    ax.plot([0, 1], [0, 1], "k--", linewidth=0.8)
    ax.set_xlabel("False positive rate")
    ax.set_ylabel("True positive rate")
    ax.legend(loc="lower right", fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "roc.png"), dpi=200)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data_dir", type=str, required=True,
                        help="Path to framesets root (contains training/, testing/)")
    parser.add_argument("--checkpoint", type=str, required=True,
                        help="Path to ckpt_best_*.pkl")
    parser.add_argument("--split", type=str, default="testing", choices=["training", "testing", "validation"],
                        help="Folder under data_dir to evaluate, or 'validation' for the held-out "
                             "part of training/ (same split as train.py)")
    parser.add_argument("--val_ratio", type=float, default=0.2,
                        help="validation ratio used in train.py (for --split validation)")
    parser.add_argument("--seed", type=int, default=42,
                        help="random seed used in train.py (for --split validation)")
    parser.add_argument("--out_dir", type=str, default=None,
                        help="Optional folder for predictions, ROC curves, metrics and roc.png")
    parser.add_argument("--batch_size", type=int, default=16)
    parser.add_argument("--num_worker", type=int, default=4)
    parser.add_argument("--gpu_ids", nargs="+", type=int, default=[0])
    parser.add_argument("--use_train_stats", action="store_true",
                        help="Compute mean/std from the training folder as in train.py and apply them")
    parser.add_argument("--mean", nargs=3, type=float, default=None,
                        help="Optional manual mean (3 floats)")
    parser.add_argument("--std", nargs=3, type=float, default=None,
                        help="Optional manual std (3 floats)")
    args = parser.parse_args()

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    class_names = ["exhale", "inhale", "pause"]

    # Input transform
    if args.mean is not None and args.std is not None:
        mean = torch.tensor(args.mean)
        std = torch.tensor(args.std)
        print("Using provided mean/std:", mean.tolist(), std.tolist())
    elif args.use_train_stats:
        tmp_ds = NpyBreathDataset(
            root=args.data_dir,
            split="training",
            input_transform=Compose([ToTensor()]),
            augment=False,
        )
        mean, std = get_mean_and_std(tmp_ds)
        # Earlier estimate, not used (pixel-pooled, differs slightly from train.py):
        # mean, std = compute_mean_std(tmp_ds, batch_size=args.batch_size, num_workers=args.num_worker)
        print("Computed mean/std from TRAIN:", mean.tolist(), std.tolist())
    else:
        # No statistics given: no normalisation
        mean, std = None, None
        print("WARNING: No normalization stats provided. Using ToTensor() only.")

    if mean is None:
        transform = Compose([ToTensor()])
    else:
        transform = Compose([ToTensor(), Normalize(mean, std)])

    # Dataset
    if args.split == "validation":
        # Same random split as train.py; it depends only on the number of
        # files, --val_ratio and --seed.
        full = NpyBreathDataset(
            root=args.data_dir,
            split="training",
            input_transform=transform,
            augment=False,
        )
        val_size = int(len(full) * float(args.val_ratio))
        _, ds = random_split(
            full,
            [len(full) - val_size, val_size],
            generator=torch.Generator().manual_seed(args.seed),
        )
        paths = [full.samples[i][0] for i in ds.indices]
    else:
        ds = NpyBreathDataset(
            root=args.data_dir,
            split=args.split,  # folder name under data_dir
            input_transform=transform,
            augment=False,
        )
        paths = [path for path, _ in ds.samples]
    loader = DataLoader(ds, batch_size=args.batch_size, shuffle=False, num_workers=args.num_worker)

    print(f"[Eval] split='{args.split}' size={len(ds)}")

    # Model
    net = model(num_classes=3)

    # DataParallel only for several GPUs
    if torch.cuda.is_available():
        net = net.to(device)
        if len(args.gpu_ids) > 1:
            net = nn.DataParallel(net, device_ids=args.gpu_ids)

    # Checkpoint
    ckpt = torch.load(args.checkpoint, map_location="cpu")
    ckpt = clean_state_dict_for_load(ckpt, net)

    msg = net.load_state_dict(ckpt, strict=True)
    print("Loaded checkpoint:", args.checkpoint)
    print("Missing keys:", msg.missing_keys)
    print("Unexpected keys:", msg.unexpected_keys)

    # Evaluation
    acc, loss, cm, per_cls, y_true, y_pred, y_prob = evaluate(net, loader, device, class_names)
    auc, curves = roc_analysis(y_true, y_prob, class_names)
    cm_norm = cm.astype(float) / np.maximum(cm.sum(axis=1, keepdims=True), 1)

    print("\n==================== RESULTS ====================")
    print(f"Split: {args.split}")
    print(f"Accuracy: {acc:.6f}")
    print(f"CE Loss:  {loss:.6f}")
    print("\nConfusion Matrix (rows=true, cols=pred):\n", cm)

    print("\nConfusion Matrix, row-normalised:\n", np.round(cm_norm, 3))

    print("\nPer-class accuracy (recall):")
    for i, name in enumerate(class_names):
        print(f"  {name}: {per_cls[i]:.6f}  (n={cm[i].sum()})")

    print("\nROC AUC (one-vs-rest):")
    for name in class_names:
        print(f"  {name}: {auc[name]:.4f}")
    print(f"  micro-average: {auc['micro']:.4f}")
    print(f"  macro-average: {auc['macro']:.4f}")
    print("=================================================\n")

    if args.out_dir is not None:
        metrics = {
            "split": args.split,
            "n": int(len(y_true)),
            "accuracy": float(acc),
            "ce_loss": float(loss),
            "class_names": class_names,
            "confusion_matrix": cm.tolist(),
            "confusion_matrix_row_normalised": cm_norm.tolist(),
            "recall": {name: float(per_cls[i]) for i, name in enumerate(class_names)},
            "auc": auc,
        }
        save_outputs(args.out_dir, paths, class_names, y_true, y_pred, y_prob, metrics, curves)
        print(f"Saved predictions, ROC curves, metrics and roc.png to {args.out_dir}")


if __name__ == "__main__":
    main()

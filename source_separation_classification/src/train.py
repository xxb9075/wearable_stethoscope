"""Train the three-class respiratory phase classifier (exhale / inhale / pause).

Transfer learning from RespireNet: a ResNet-34 model initialised with the
RespireNet checkpoint (trained on ICBHI lung sounds). conv1 and the residual
blocks up to layer3 are frozen; layer4 and the classification head are
fine-tuned on mel-spectrogram images of segmented breath phases with a
class-weighted cross-entropy loss (SGD, momentum 0.9, learning rate 1e-3,
weight decay 5e-4, batch size 16).

Based on the RespireNet training script by Siddhartha Gairola (Microsoft,
MIT License). Adapted for three-class breath phase classification with a
validation split, TensorBoard logging and Grad-CAM export.

Example, run from src/:

    python train.py --data_dir ../data/framesets --model_path ../outputs --checkpoint ../models/ckpt_best.pkl
"""

import os
import argparse
import random
from datetime import datetime
from tqdm import tqdm

import numpy as np

import torch
import torch.nn as nn
from torch.utils.data import DataLoader, random_split
from torch.utils.tensorboard import SummaryWriter

import torchvision
from torchvision.transforms import Compose, Normalize, ToTensor

import matplotlib
matplotlib.use("Agg")

from npy_dataloader import NpyBreathDataset
from utils import get_mean_and_std
from nets.network_cnn import model
from sklearn.metrics import confusion_matrix


# Command line arguments
parser = argparse.ArgumentParser(description="RespireNet: Breath Phase Classification (3-class)")

parser.add_argument("--lr", default=1e-3, type=float, help="learning rate")
parser.add_argument("--weight_decay", default=5e-4, type=float, help="weight decay value")
parser.add_argument("--gpu_ids", nargs="+", type=int, default=[0], help="GPU ids, e.g. --gpu_ids 0 1")
parser.add_argument("--num_worker", default=4, type=int, help="numbers of worker")
parser.add_argument("--batch_size", default=16, type=int, help="batch size")
parser.add_argument("--epochs", default=40, type=int, help="epochs")
parser.add_argument("--start_epochs", default=0, type=int, help="start epochs")

parser.add_argument("--data_dir", type=str, required=True, help="data directory")
parser.add_argument("--model_path", type=str, required=True, help="model saving directory")
parser.add_argument("--checkpoint", default=None, type=str, help="load checkpoint")
parser.add_argument("--stetho_id", default=-1, type=int,
                    help="device id from RespireNet; only used in the checkpoint name")

# TensorBoard and Grad-CAM
parser.add_argument("--run_name", default=None, type=str, help="tensorboard run name override")
parser.add_argument("--log_interval", default=999999, type=int, help="unused")
parser.add_argument("--save_cam_every", default=1, type=int, help="export Grad-CAM every N epochs")
parser.add_argument("--cam_samples", default=24, type=int, help="how many val samples to export per epoch")
parser.add_argument("--cam_dirname", default="gradcam", type=str, help="folder name under model_path to save CAMs")

# Train/validation split
parser.add_argument("--val_ratio", default=0.2, type=float, help="validation ratio from training split")
parser.add_argument("--seed", default=42, type=int, help="random seed")

args = parser.parse_args()

# Helpers

CLASS_NAMES = ["exhale", "inhale", "pause"]
NUM_CLASSES = 3


def seed_everything(seed: int):
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)


def safe_torch_load(path: str, map_location="cpu"):
    """Load a checkpoint, with weights_only=True where PyTorch supports it."""
    try:
        return torch.load(path, map_location=map_location, weights_only=True)
    except TypeError:
        return torch.load(path, map_location=map_location)


def denormalize(img: torch.Tensor, mean: torch.Tensor, std: torch.Tensor) -> torch.Tensor:
    """Undo Normalize. img: (B, C, H, W); mean, std: (C,)."""
    mean = mean.view(1, -1, 1, 1).to(img.device)
    std = std.view(1, -1, 1, 1).to(img.device)
    return img * std + mean


def find_last_conv(module: nn.Module) -> nn.Module:
    """Return the last nn.Conv2d in the module (the Grad-CAM target)."""
    last = None
    for _, m in module.named_modules():
        if isinstance(m, nn.Conv2d):
            last = m
    if last is None:
        raise RuntimeError("Could not find any nn.Conv2d layer for Grad-CAM.")
    return last


class GradCAM:
    """Grad-CAM on a given convolution layer.

    Also works with a DataParallel model (pass the DataParallel object).
    """
    def __init__(self, model_dp: nn.Module, target_layer: nn.Module):
        self.model = model_dp
        self.target_layer = target_layer
        self.activations = None
        self.gradients = None
        self._register_hooks()

    def _register_hooks(self):
        def fwd_hook(_, __, output):
            self.activations = output

        def bwd_hook(_, grad_input, grad_output):
            self.gradients = grad_output[0]  # (B, C, H, W)

        self.target_layer.register_forward_hook(fwd_hook)
        self.target_layer.register_full_backward_hook(bwd_hook)

    def __call__(self, x: torch.Tensor, class_idx: torch.Tensor) -> torch.Tensor:
        """Return Grad-CAM maps (B, 1, h, w) in [0, 1].

        x: (B, C, H, W) input; class_idx: (B,) target class per sample.
        """
        self.model.zero_grad(set_to_none=True)

        logits = self.model(x)  # (B, num_classes)
        score = logits.gather(1, class_idx.view(-1, 1)).sum()
        score.backward(retain_graph=False)

        A = self.activations  # (B, C, h, w)
        dA = self.gradients

        # Channel weights: spatial mean of the gradients, (B, C, 1, 1)
        w = dA.mean(dim=(2, 3), keepdim=True)

        cam = (w * A).sum(dim=1, keepdim=True)
        cam = torch.relu(cam)

        # Scale each map to [0, 1]
        B = cam.size(0)
        cam_flat = cam.view(B, -1)
        cam_min = cam_flat.min(dim=1)[0].view(B, 1, 1, 1)
        cam_max = cam_flat.max(dim=1)[0].view(B, 1, 1, 1)
        cam = (cam - cam_min) / (cam_max - cam_min + 1e-8)

        return cam.detach()


def overlay_cam_on_image(img_rgb: torch.Tensor, cam_1: torch.Tensor) -> torch.Tensor:
    """Alpha-blend a CAM (1, H, W) onto an RGB image (3, H, W), both in [0, 1]."""
    cam_rgb = cam_1.repeat(3, 1, 1)
    alpha = 0.45
    out = (1 - alpha) * img_rgb + alpha * cam_rgb
    return out.clamp(0, 1)


# Trainer

class Trainer:
    def __init__(self):
        self.args = args
        seed_everything(self.args.seed)

        self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

        # A time stamp in the run name gives each run its own TensorBoard
        # directory, so curves from different runs are not merged.
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        if self.args.run_name is None:
            run_name = f"lr{self.args.lr}_bs{self.args.batch_size}_{stamp}"
        else:
            run_name = f"{self.args.run_name}_{stamp}"

        self.tb_logdir = os.path.join(self.args.model_path, "runs", run_name)
        os.makedirs(self.tb_logdir, exist_ok=True)
        self.writer = SummaryWriter(log_dir=self.tb_logdir)
        print(f"[TensorBoard] logdir = {self.tb_logdir}")

        # Normalisation statistics from the training images
        tmp_dataset = NpyBreathDataset(
            root=self.args.data_dir,
            split="training",
            input_transform=Compose([ToTensor()]),
            augment=False,
        )
        mean, std = get_mean_and_std(tmp_dataset)
        print("MEAN", mean, "STD", std)

        # Keep mean/std as tensors for de-normalising the Grad-CAM images
        if not torch.is_tensor(mean):
            mean = torch.tensor(mean, dtype=torch.float32)
        if not torch.is_tensor(std):
            std = torch.tensor(std, dtype=torch.float32)

        self.mean = mean
        self.std = std

        # Input transform
        self.input_transform = Compose([ToTensor(), Normalize(mean, std)])

        # Training data with augmentation
        full_dataset = NpyBreathDataset(
            root=self.args.data_dir,
            split="training",
            input_transform=self.input_transform,
            augment=True,
        )

        # Random train/validation split
        val_ratio = float(self.args.val_ratio)
        val_size = int(len(full_dataset) * val_ratio)
        train_size = len(full_dataset) - val_size

        train_dataset, val_dataset = random_split(
            full_dataset,
            [train_size, val_size],
            generator=torch.Generator().manual_seed(self.args.seed),
        )
        print(f"Train size: {train_size}, Val size: {val_size}")

        self.train_dataset = train_dataset
        self.val_dataset = val_dataset

        # Data loaders
        self.train_data_loader = DataLoader(
            train_dataset,
            num_workers=self.args.num_worker,
            batch_size=self.args.batch_size,
            shuffle=True,
            pin_memory=True,
        )
        self.val_data_loader = DataLoader(
            val_dataset,
            num_workers=self.args.num_worker,
            batch_size=self.args.batch_size,
            shuffle=False,
            pin_memory=True,
        )

        # Model
        self.net = model(num_classes=NUM_CLASSES).to(self.device)

        # Load the RespireNet weights except its 4-class output layer
        if self.args.checkpoint is not None and self.args.checkpoint != "None":
            ckpt = safe_torch_load(self.args.checkpoint, map_location="cpu")

            # Drop the classifier weights if present
            for k in ["cls_fc.weight", "cls_fc.bias", "module.cls_fc.weight", "module.cls_fc.bias"]:
                if isinstance(ckpt, dict) and k in ckpt:
                    del ckpt[k]

            msg = self.net.load_state_dict(ckpt, strict=False)
            print("Pre-trained backbone loaded (classifier skipped):", self.args.checkpoint)
            print("Missing keys:", msg.missing_keys)
            print("Unexpected keys:", msg.unexpected_keys)

        # Freeze conv1 and layer1-3; fine-tune layer4 and the classification head
        self.net.freeze_early_layers()

        # Earlier set-up, not used: freeze the whole ResNet and train only cls_fc.
        # if hasattr(self.net, "freeze_backbone"):
        #     self.net.freeze_backbone()
        # else:
        #     for name, p in self.net.named_parameters():
        #         p.requires_grad = ("cls" in name or "fc" in name)

        # DataParallel
        self.net = nn.DataParallel(self.net, device_ids=self.args.gpu_ids)

        # Trainable parameters
        params_to_update = [p for p in self.net.parameters() if p.requires_grad]
        print("Trainable params:", sum(p.numel() for p in params_to_update))

        # Optimiser
        self.optimizer = torch.optim.SGD(
            params_to_update,
            lr=self.args.lr,
            momentum=0.9,
            weight_decay=float(self.args.weight_decay),
        )

        # Class-weighted cross-entropy (inverse class frequency, mean weight 1)
        counts = np.zeros(NUM_CLASSES, dtype=np.int64)
        for _, y in train_dataset:
            counts[int(y)] += 1

        weights = 1.0 / np.maximum(counts, 1)
        weights = weights / weights.mean()
        class_weights = torch.tensor(weights, dtype=torch.float32, device=self.device)

        print("Train class counts:", counts)
        print("Class weights:", class_weights.detach().cpu().numpy())

        self.loss_func = nn.CrossEntropyLoss(weight=class_weights)
        self.loss_nored = nn.CrossEntropyLoss(weight=class_weights, reduction="none")

        # Grad-CAM on the last convolution layer
        target_layer = find_last_conv(self.net.module)
        print(f"[Grad-CAM] Using target layer: {target_layer}")
        self.grad_cam = GradCAM(self.net, target_layer)

        # Output folder for Grad-CAM images
        self.cam_root = os.path.join(self.args.model_path, self.args.cam_dirname)
        os.makedirs(self.cam_root, exist_ok=True)

    def evaluate(self, epoch: int):
        self.net.eval()

        test_losses = []
        class_hits = [0.0] * NUM_CLASSES
        class_counts = [0.0] * NUM_CLASSES
        running_corrects = 0.0
        denom = 0.0

        conf_label, conf_pred = [], []

        with torch.no_grad():
            for _, (image, label) in tqdm(enumerate(self.val_data_loader), total=len(self.val_data_loader)):
                image = image.to(self.device, non_blocking=True)
                label = label.to(self.device, non_blocking=True)

                output = self.net(image)

                loss = self.loss_func(output, label)
                loss_nored = self.loss_nored(output, label)
                test_losses.append(float(loss.item()))

                preds = output.argmax(dim=1)
                running_corrects += (preds == label).sum().item()
                denom += label.size(0)

                for i in range(preds.size(0)):
                    y = int(label[i].item())
                    p = int(preds[i].item())
                    class_counts[y] += 1.0
                    if p == y:
                        class_hits[y] += 1.0
                    conf_label.append(y)
                    conf_pred.append(p)
                    _ = float(loss_nored[i].item())

        acc = running_corrects / max(denom, 1.0)
        mean_loss = float(np.mean(test_losses)) if len(test_losses) else float("nan")

        print(f"Val Accuracy: {acc:.6f}")
        print(f"epoch {epoch}, Validation CE loss: {mean_loss:.6f}")

        conf_matrix = confusion_matrix(np.array(conf_label), np.array(conf_pred))
        print("Confusion Matrix\n", conf_matrix)

        row_sums = conf_matrix.sum(axis=1, keepdims=True)
        conf_matrix_norm = conf_matrix.astype(float) / np.maximum(row_sums, 1.0)
        print("Classwise Scores", np.diag(conf_matrix_norm))

        for c in range(NUM_CLASSES):
            if class_counts[c] > 0:
                print(f"Class {c} ({CLASS_NAMES[c]}): acc={class_hits[c]/class_counts[c]:.4f} (n={int(class_counts[c])})")
            else:
                print(f"Class {c} ({CLASS_NAMES[c]}): acc=nan (n=0)")

        self.net.train()
        return acc, mean_loss

    def export_gradcam_by_class(self, epoch: int):
        """Save Grad-CAM images for one validation batch.

        Images go to model_path/gradcam/epoch_XXX/{exhale,inhale,pause}/ and a
        grid is logged to TensorBoard under Images/GradCAM_triplets.
        """
        self.net.eval()

        epoch_dir = os.path.join(self.cam_root, f"epoch_{epoch:03d}")
        for cname in CLASS_NAMES:
            os.makedirs(os.path.join(epoch_dir, cname), exist_ok=True)

        # First validation batch
        images, labels = next(iter(self.val_data_loader))
        images = images.to(self.device)
        labels = labels.to(self.device)

        # Grad-CAM needs gradients
        images = images.requires_grad_(True)

        # CAM for the true class, upsampled from the conv resolution to the input
        cam = self.grad_cam(images, labels)
        cam = torch.nn.functional.interpolate(cam, size=images.shape[-2:], mode="bilinear", align_corners=False)

        # Undo normalisation for saving
        img_vis = denormalize(images.detach(), self.mean, self.std).clamp(0, 1)

        B = images.size(0)
        k = min(B, int(self.args.cam_samples))
        saved_tensors = []  # for the TensorBoard grid

        for i in range(k):
            y = int(labels[i].item())
            cname = CLASS_NAMES[y]

            img_i = img_vis[i]           # (C, H, W)
            cam_i = cam[i]               # (1, H, W)

            # Make the image 3-channel
            if img_i.size(0) == 1:
                img_i = img_i.repeat(3, 1, 1)
            elif img_i.size(0) != 3:
                # Other channel counts: keep the first three
                img_i = img_i[:3, :, :]

            overlay = overlay_cam_on_image(img_i, cam_i)

            # Save [image | CAM | overlay] side by side
            cam_rgb = cam_i.repeat(3, 1, 1)
            triplet = torch.cat([img_i, cam_rgb, overlay], dim=2)

            fname = os.path.join(epoch_dir, cname, f"val_e{epoch:03d}_i{i:04d}_true{y}.png")
            torchvision.utils.save_image(triplet, fname)
            saved_tensors.append(triplet)

        # Log a grid to TensorBoard
        if len(saved_tensors) > 0:
            grid = torchvision.utils.make_grid(saved_tensors, nrow=1)
            self.writer.add_image("Images/GradCAM_triplets", grid, global_step=epoch)

        self.net.train()

    def train(self):
        best_acc = -1.0

        global_step_train = 0

        for epoch in range(self.args.start_epochs, self.args.epochs):
            self.net.train()

            losses = []
            class_hits = [0.0] * NUM_CLASSES
            class_counts = [0.0] * NUM_CLASSES
            classwise_train_losses = [[] for _ in range(NUM_CLASSES)]
            running_corrects = 0.0
            denom = 0.0

            for i, (image, label) in tqdm(enumerate(self.train_data_loader), total=len(self.train_data_loader)):
                image = image.to(self.device, non_blocking=True)
                label = label.to(self.device, non_blocking=True)

                output = self.net(image)
                loss = self.loss_func(output, label)
                loss_nored = self.loss_nored(output, label)

                preds = output.argmax(dim=1)
                running_corrects += (preds == label).sum().item()
                denom += label.size(0)

                for b in range(preds.size(0)):
                    y = int(label[b].item())
                    p = int(preds[b].item())
                    class_counts[y] += 1.0
                    if p == y:
                        class_hits[y] += 1.0
                    classwise_train_losses[y].append(float(loss_nored[b].item()))

                self.optimizer.zero_grad(set_to_none=True)
                loss.backward()
                self.optimizer.step()

                losses.append(float(loss.item()))
                global_step_train += 1

                # Print training metrics at the end of each epoch
                if i == len(self.train_data_loader) - 1:
                    train_acc = running_corrects / max(denom, 1.0)
                    mean_train_loss = float(np.mean(losses)) if len(losses) else float("nan")

                    print("---------------------------------------------")
                    print(f"epoch {epoch} iter {i}/{len(self.train_data_loader)-1} Train CE loss: {mean_train_loss:.6f}")
                    print(f"Train Accuracy: {train_acc:.6f}")

                    for c in range(NUM_CLASSES):
                        if class_counts[c] > 0:
                            cls_acc = class_hits[c] / class_counts[c]
                            cls_loss = float(np.mean(classwise_train_losses[c])) if len(classwise_train_losses[c]) else float("nan")
                            print(f"  {CLASS_NAMES[c]}: acc={cls_acc:.4f} (n={int(class_counts[c])}), loss={cls_loss:.4f}")
                        else:
                            print(f"  {CLASS_NAMES[c]}: acc=nan (n=0), loss=nan")

                    # TensorBoard, one step per epoch
                    self.writer.add_scalar("Train/Loss", mean_train_loss, global_step=epoch)
                    self.writer.add_scalar("Train/Accuracy", train_acc, global_step=epoch)

            # Validation
            print("Validation:")
            val_acc, val_loss = self.evaluate(epoch)

            self.writer.add_scalar("Val/Loss", val_loss, global_step=epoch)
            self.writer.add_scalar("Val/Accuracy", val_acc, global_step=epoch)

            # Keep the checkpoint with the best validation accuracy
            if val_acc > best_acc:
                best_acc = val_acc
                os.makedirs(self.args.model_path, exist_ok=True)
                ckpt_path = os.path.join(
                    self.args.model_path,
                    f"ckpt_best_{self.args.epochs}_{self.args.stetho_id}.pkl",
                )
                # Save the weights without the DataParallel wrapper
                torch.save(self.net.module.state_dict(), ckpt_path)
                print("New best validation accuracy:", best_acc)
                print("Saved:", ckpt_path)

            print("Best validation accuracy so far:", best_acc)

            # Grad-CAM export
            if self.args.save_cam_every > 0 and (epoch % self.args.save_cam_every == 0):
                self.export_gradcam_by_class(epoch)

        self.writer.flush()
        self.writer.close()


if __name__ == "__main__":
    trainer = Trainer()
    trainer.train()
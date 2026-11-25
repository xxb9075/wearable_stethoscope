"""ResNet-34 classifier used for respiratory phase classification.

Same architecture as the RespireNet CNN (Siddhartha Gairola, Microsoft, MIT
License): an ImageNet ResNet-34 whose final layer is replaced by a two-layer
head (512 -> 128 -> 128), followed by a linear classification layer. The
RespireNet checkpoint loads into this model except for ``cls_fc``.
"""

import torch.nn as nn
from torchvision.models import resnet34


class model(nn.Module):
    def __init__(self, num_classes=3, drop_prob=0.5):
        super(model, self).__init__()

        # Encoder: ResNet-34 with a 128-dimensional head
        self.model_ft = resnet34(pretrained=True)
        num_ftrs = self.model_ft.fc.in_features
        self.model_ft.fc = nn.Sequential(nn.Dropout(drop_prob), nn.Linear(num_ftrs, 128), nn.ReLU(True),
                nn.Dropout(drop_prob), nn.Linear(128, 128), nn.ReLU(True))
        self.cls_fc = nn.Linear(128, num_classes)

    def forward(self, x):
        x = self.model_ft(x)
        x = self.cls_fc(x)
        return x

    def fine_tune(self, block_layer=5):
        """Freeze the first ``block_layer + 1`` child modules of the ResNet."""
        for idx, child in enumerate(self.model_ft.children()):
            if idx>block_layer:
                break
            for param in child.parameters():
                param.requires_grad = False

    def freeze_early_layers(self):
        """Freeze conv1 and residual blocks up to layer3; train layer4 and the head.

        This is the fine-tuning set-up described in the paper: the early
        convolutional layers keep the RespireNet features, while layer4, the
        128-d head (model_ft.fc) and the classification layer (cls_fc) are
        fine-tuned.
        """
        for param in self.parameters():
            param.requires_grad = False

        for module in (self.model_ft.layer4, self.model_ft.fc, self.cls_fc):
            for param in module.parameters():
                param.requires_grad = True

    def freeze_backbone(self):
        """Freeze the whole ResNet, including its 128-d head; only cls_fc is trained.

        Not used by train.py any more (see freeze_early_layers).
        """
        for param in self.model_ft.parameters():
            param.requires_grad = False

        for param in self.cls_fc.parameters():
            param.requires_grad = True

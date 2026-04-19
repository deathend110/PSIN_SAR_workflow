import random
from pathlib import Path

import cv2
import numpy as np
import matplotlib.pyplot as plt
import torch
import torch.nn as nn
import segmentation_models_pytorch as smp


# =========================================================
# Config
# =========================================================
ROOT_DIR = Path(r"G:\VSCODE-G\AIRPolarSARSeg_process\Prepared\test")
# ROOT_DIR = Path(r"G:\VSCODE-G\AIRPolarSARSeg_process\Prepared\train")

BIT1_INPUT_DIR = ROOT_DIR / "lq_1bit"
GT_INPUT_DIR = ROOT_DIR / "gt_16bit"
LABEL_DIR = ROOT_DIR / "label"

# ===== 恢复模型权重 =====
RESTORE_NET_WEIGHT = r"output\Model(RAAUNet_DeepCA)-Dataset(AIRPolarSARSeg_restore_singlepol)-Loss(L1+TV+lamda0.001)-Epochs180-Batch_size32-ValBatch_size16-lr0.0003-minlr1e-06-F48\best_final_state_dict.pth"
# RESTORE_NET_WEIGHT = r"./output_edgeft/Finetune(RAAUNet_DeepCA)-Dataset(AIRPolarSARSeg_restore_singlepol)-Loss(L1+TV+0.2Edge)-Epochs40-Batch_size32-ValBatch_size16-lr5e-05-minlr1e-06-F48/best_ssim.pth"

# ===== 三个分割模型权重 =====
RESTORE_SEG_MODEL_WEIGHT = r"output\Model(MobileNetV3UNet)-Dataset(AIRPolarSARSeg_seg_1bit_restore)-Loss(WeightedCE+0.5Dice)-Epochs180-Batch_size32-ValBatch_size16-lr0.0003-minlr1e-06-C6\best_miou.pth"
# RESTORE_SEG_MODEL_WEIGHT = r"./output/Model(MobileNetV3UNet)-Dataset(AIRPolarSARSeg_seg_1bit_restore_edge)-Loss(WeightedCE+0.5Dice)-Epochs180-Batch_size32-ValBatch_size16-lr0.0003-minlr1e-06-C6/best_final_state_dict.pth"

BIT1_SEG_MODEL_WEIGHT = r"output_1bit\Model(MobileNetV3UNet)-Dataset(AIRPolarSARSeg_seg_lq_1bit)-Loss(WeightedCE+0.5Dice)-Epochs180-Batch_size32-ValBatch_size16-lr0.0003-minlr1e-06-C6\best_miou.pth"
GT_SEG_MODEL_WEIGHT = r"output_gt\Model(MobileNetV3UNet)-Dataset(AIRPolarSARSeg_seg_gt_16bit)-Loss(WeightedCE+0.5Dice)-Epochs180-Batch_size32-ValBatch_size16-lr0.0003-minlr1e-06-C6\best_miou.pth"

# 可手动指定 patch；None 则随机取一个公共 patch
PATCH_NAME = None
# 例如：
# PATCH_NAME = "base2_hh_amp_frame_053.png"

NUM_CLASSES = 6
IGNORE_INDEX = 255
SEG_IN_CHANNELS = 1
RESTORE_IN_CHANNELS = 1
RESTORE_FILTER_NUM = 48
DEVICE = "cuda" if torch.cuda.is_available() else "cpu"
SEED = int(input("输入SEED: "))



# =========================================================
# Segmentation Model
# =========================================================
class MobileNetV3UNet(nn.Module):
    def __init__(self, num_classes=NUM_CLASSES, in_channels=SEG_IN_CHANNELS):
        super().__init__()
        self.net = smp.Unet(
            encoder_name="tu-tf_mobilenetv3_small_100",
            encoder_weights=None,
            decoder_use_norm="batchnorm",
            in_channels=in_channels,
            classes=num_classes,
            activation=None,
        )

    def forward(self, x):
        return self.net(x)


# =========================================================
# Restore Model
# =========================================================
from RAAUNet_DeepCA_V3 import RAAUNet_DeepCA


# =========================================================
# Load Utils
# =========================================================
def clean_state_dict_keys(state_dict):
    new_state_dict = {}
    for k, v in state_dict.items():
        if k.startswith("module."):
            k = k[len("module."):]
        new_state_dict[k] = v
    return new_state_dict


def load_seg_model(weight_path: str, device: str = DEVICE):
    model = MobileNetV3UNet(num_classes=NUM_CLASSES, in_channels=SEG_IN_CHANNELS).to(device)

    ckpt = torch.load(weight_path, map_location=device)
    if isinstance(ckpt, dict) and "model_state_dict" in ckpt:
        state_dict = ckpt["model_state_dict"]
    elif isinstance(ckpt, dict):
        state_dict = ckpt
    else:
        raise ValueError(f"无法识别的分割权重格式: {weight_path}")

    state_dict = clean_state_dict_keys(state_dict)
    missing, unexpected = model.load_state_dict(state_dict, strict=False)

    print(f"[Load Seg] {weight_path}")
    print(f"  missing keys   : {len(missing)}")
    print(f"  unexpected keys: {len(unexpected)}")

    model.eval()
    return model


def load_restore_model(
    weight_path: str,
    in_channels: int = RESTORE_IN_CHANNELS,
    filter_num: int = RESTORE_FILTER_NUM,
    device: str = DEVICE,
):
    model = RAAUNet_DeepCA(
        in_channels=in_channels,
        filter_num=filter_num,
    ).to(device)

    ckpt = torch.load(weight_path, map_location=device)
    if isinstance(ckpt, dict) and "model_state_dict" in ckpt:
        state_dict = ckpt["model_state_dict"]
    elif isinstance(ckpt, dict):
        state_dict = ckpt
    else:
        raise ValueError(f"无法识别的恢复权重格式: {weight_path}")

    state_dict = clean_state_dict_keys(state_dict)
    missing, unexpected = model.load_state_dict(state_dict, strict=False)

    print(f"[Load Restore] {weight_path}")
    print(f"  missing keys   : {len(missing)}")
    print(f"  unexpected keys: {len(unexpected)}")

    model.eval()
    return model


# =========================================================
# IO
# =========================================================
def read_input_image(path: Path) -> np.ndarray:
    """
    与你当前 infer.py 保持一致：
    - 单通道读取
    - uint16 或 max>255 -> /65535
    - 否则 /255
    """
    img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError(f"读取输入图失败: {path}")

    if img.ndim == 3:
        img = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    img = img.astype(np.float32)
    img = img / 255.0
    return img


def read_label_mask(path: Path) -> np.ndarray:
    """
    原始标签：
        1~6 -> 训练标签 0~5
        其他 -> IGNORE_INDEX
    """
    mask = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if mask is None:
        raise RuntimeError(f"读取标签失败: {path}")

    mask = mask.astype(np.int64)

    out = np.full_like(mask, IGNORE_INDEX, dtype=np.int64)
    valid = (mask >= 1) & (mask <= NUM_CLASSES)
    out[valid] = mask[valid] - 1
    return out


# =========================================================
# Visualization
# =========================================================
def mask_to_rgb(mask: np.ndarray) -> np.ndarray:
    """
    训练标签 mask(0~5) -> RGB 可视化图
    """
    if mask.ndim != 2:
        raise ValueError(f"mask 必须是单通道二维数组，当前 shape={mask.shape}")

    color_map = np.array([
        [0,   0, 255],   # 0 水体
        [0, 255,   0],   # 1 植被
        [255, 0,   0],   # 2 裸地
        [0, 255, 255],   # 3 道路
        [255, 255, 0],   # 4 建筑
        [255, 0, 255],   # 5 山脉
    ], dtype=np.uint8)

    if mask.min() < 0 or mask.max() > 5:
        raise ValueError(f"mask 中存在超出 0~5 范围的像素值，min={mask.min()}, max={mask.max()}")

    return color_map[mask]


# =========================================================
# Metrics
# =========================================================
def compute_miou(pred_mask: np.ndarray, gt_mask: np.ndarray, num_classes: int = NUM_CLASSES, ignore_index: int = IGNORE_INDEX) -> float:
    valid = gt_mask != ignore_index
    pred = pred_mask[valid]
    gt = gt_mask[valid]

    if pred.size == 0:
        return 0.0

    ious = []
    for cls_id in range(num_classes):
        pred_c = pred == cls_id
        gt_c = gt == cls_id

        inter = np.logical_and(pred_c, gt_c).sum()
        union = np.logical_or(pred_c, gt_c).sum()

        if union > 0:
            ious.append(inter / union)

    if len(ious) == 0:
        return 0.0

    return float(np.mean(ious))


def calc_psnr(img1: np.ndarray, img2: np.ndarray, data_range: float = 1.0) -> float:
    mse = np.mean((img1 - img2) ** 2, dtype=np.float64)
    if mse <= 1e-12:
        return float("inf")
    return 10.0 * np.log10((data_range ** 2) / mse)


def calc_ssim(
    img1: np.ndarray,
    img2: np.ndarray,
    data_range: float = 1.0,
    k1: float = 0.01,
    k2: float = 0.03,
    win_size: int = 11,
    sigma: float = 1.5,
) -> float:
    if img1.shape != img2.shape:
        raise ValueError(f"SSIM 输入尺寸不一致: {img1.shape} vs {img2.shape}")

    c1 = (k1 * data_range) ** 2
    c2 = (k2 * data_range) ** 2

    mu1 = cv2.GaussianBlur(img1.astype(np.float32), (win_size, win_size), sigma)
    mu2 = cv2.GaussianBlur(img2.astype(np.float32), (win_size, win_size), sigma)

    mu1_sq = mu1 * mu1
    mu2_sq = mu2 * mu2
    mu1_mu2 = mu1 * mu2

    sigma1_sq = cv2.GaussianBlur(img1 * img1, (win_size, win_size), sigma) - mu1_sq
    sigma2_sq = cv2.GaussianBlur(img2 * img2, (win_size, win_size), sigma) - mu2_sq
    sigma12 = cv2.GaussianBlur(img1 * img2, (win_size, win_size), sigma) - mu1_mu2

    ssim_map = (
        (2 * mu1_mu2 + c1) * (2 * sigma12 + c2)
    ) / (
        (mu1_sq + mu2_sq + c1) * (sigma1_sq + sigma2_sq + c2)
    )

    return float(np.mean(ssim_map, dtype=np.float64))


# =========================================================
# Inference
# =========================================================
@torch.no_grad()
def infer_seg_one(model, image: np.ndarray, device: str = DEVICE) -> np.ndarray:
    x = torch.from_numpy(image).unsqueeze(0).unsqueeze(0).float().to(device)  # [1,1,H,W]
    logits = model(x)
    pred = torch.argmax(logits, dim=1).squeeze(0).cpu().numpy().astype(np.int64)
    return pred


@torch.no_grad()
def infer_restore_one(model, image: np.ndarray, device: str = DEVICE) -> np.ndarray:
    """
    image: [H,W], float32 in [0,1]
    return: restored [H,W], float32 in [0,1]
    """
    x = torch.from_numpy(image).unsqueeze(0).unsqueeze(0).float().to(device)
    pred = model(x)
    pred = pred.clamp(0.0, 1.0)
    pred = pred.squeeze(0).squeeze(0).float().cpu().numpy()
    return pred


# =========================================================
# Patch Picker
# =========================================================
def pick_common_patch_name(seed: int = None) -> str:
    bit1_names = {p.name for p in BIT1_INPUT_DIR.iterdir() if p.is_file()}
    gt_names = {p.name for p in GT_INPUT_DIR.iterdir() if p.is_file()}
    label_names = {p.name for p in LABEL_DIR.iterdir() if p.is_file()}

    common = sorted(list(bit1_names & gt_names & label_names))
    if len(common) == 0:
        raise RuntimeError("lq_1bit、gt_16bit、label 三个目录下没有公共 patch 文件名")

    rng = random.Random(seed)
    return rng.choice(common)


# =========================================================
# Main
# =========================================================
def main():
    patch_name = PATCH_NAME if PATCH_NAME is not None else pick_common_patch_name(seed=SEED)
    print(f"[Patch] using patch: {patch_name}")

    bit1_img_path = BIT1_INPUT_DIR / patch_name
    gt_img_path = GT_INPUT_DIR / patch_name
    label_path = LABEL_DIR / patch_name

    if not bit1_img_path.exists():
        raise FileNotFoundError(bit1_img_path)
    if not gt_img_path.exists():
        raise FileNotFoundError(gt_img_path)
    if not label_path.exists():
        raise FileNotFoundError(label_path)

    # 读取图像与标签
    bit1_img = read_input_image(bit1_img_path)
    gt_img = read_input_image(gt_img_path)
    gt_mask = read_label_mask(label_path)

    # 加载模型
    restore_net = load_restore_model(RESTORE_NET_WEIGHT, device=DEVICE)
    restore_seg_model = load_seg_model(RESTORE_SEG_MODEL_WEIGHT, DEVICE)
    bit1_seg_model = load_seg_model(BIT1_SEG_MODEL_WEIGHT, DEVICE)
    gt_seg_model = load_seg_model(GT_SEG_MODEL_WEIGHT, DEVICE)

    # 现场恢复
    restore_img = infer_restore_one(restore_net, bit1_img, DEVICE)

    # 分割推理
    pred_restore = infer_seg_one(restore_seg_model, restore_img, DEVICE)
    pred_bit1 = infer_seg_one(bit1_seg_model, bit1_img, DEVICE)
    pred_gt = infer_seg_one(gt_seg_model, gt_img, DEVICE)

    # mIoU
    miou_restore = compute_miou(pred_restore, gt_mask)
    miou_bit1 = compute_miou(pred_bit1, gt_mask)
    miou_gt = compute_miou(pred_gt, gt_mask)

    # 恢复质量指标
    psnr_restore_vs_gt = calc_psnr(restore_img, gt_img)
    ssim_restore_vs_gt = calc_ssim(restore_img, gt_img)

    psnr_1bit_vs_gt = calc_psnr(bit1_img, gt_img)
    ssim_1bit_vs_gt = calc_ssim(bit1_img, gt_img)

    print(f"[mIoU] restore_input = {miou_restore:.4f}")
    print(f"[mIoU] 1bit_input    = {miou_bit1:.4f}")
    print(f"[mIoU] gt_input      = {miou_gt:.4f}")

    print(f"[Restore vs GT] PSNR={psnr_restore_vs_gt:.4f}, SSIM={ssim_restore_vs_gt:.4f}")
    print(f"[1bit vs GT]    PSNR={psnr_1bit_vs_gt:.4f}, SSIM={ssim_1bit_vs_gt:.4f}")

    # RGB mask
    pred_restore_rgb = mask_to_rgb(pred_restore)
    pred_bit1_rgb = mask_to_rgb(pred_bit1)
    pred_gt_rgb = mask_to_rgb(pred_gt)
    gt_label_rgb = mask_to_rgb(gt_mask)

    # 显示
    fig, axes = plt.subplots(2, 4, figsize=(24, 10))

    # 第一行：输入 SAR 图 + GT Label RGB
    axes[0, 0].imshow(restore_img, cmap="gray", vmin=0.0, vmax=1.0)
    axes[0, 0].set_title(
        "Restore Input (on-the-fly)\n"
        f"PSNR={psnr_restore_vs_gt:.4f}, SSIM={ssim_restore_vs_gt:.4f}"
    )
    axes[0, 0].axis("off")

    axes[0, 1].imshow(bit1_img, cmap="gray", vmin=0.0, vmax=1.0)
    axes[0, 1].set_title(
        "1bit Input\n"
        f"PSNR={psnr_1bit_vs_gt:.4f}, SSIM={ssim_1bit_vs_gt:.4f}"
    )
    axes[0, 1].axis("off")

    axes[0, 2].imshow(gt_img, cmap="gray", vmin=0.0, vmax=1.0)
    axes[0, 2].set_title("GT Input")
    axes[0, 2].axis("off")

    axes[0, 3].imshow(gt_label_rgb, interpolation="nearest")
    axes[0, 3].set_title("GT Label RGB")
    axes[0, 3].axis("off")

    # 第二行：预测 mask RGB
    axes[1, 0].imshow(pred_restore_rgb, interpolation="nearest")
    axes[1, 0].set_title(f"Restore Pred Mask RGB\nmIoU={miou_restore:.4f}")
    axes[1, 0].axis("off")

    axes[1, 1].imshow(pred_bit1_rgb, interpolation="nearest")
    axes[1, 1].set_title(f"1bit Pred Mask RGB\nmIoU={miou_bit1:.4f}")
    axes[1, 1].axis("off")

    axes[1, 2].imshow(pred_gt_rgb, interpolation="nearest")
    axes[1, 2].set_title(f"GT Pred Mask RGB\nmIoU={miou_gt:.4f}")
    axes[1, 2].axis("off")

    axes[1, 3].imshow(gt_label_rgb, interpolation="nearest")
    axes[1, 3].set_title("GT Label RGB")
    axes[1, 3].axis("off")

    plt.suptitle(f"Patch: {patch_name}", fontsize=14)
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
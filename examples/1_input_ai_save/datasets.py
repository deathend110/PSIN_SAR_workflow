from pathlib import Path
from typing import List, Tuple

import cv2
import numpy as np
import torch
from torch.utils.data import Dataset, DataLoader


IMG_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"}


# =========================================================
# 通用工具
# =========================================================
def _is_image_file(p: Path) -> bool:
    return p.is_file() and p.suffix.lower() in IMG_EXTS


def _resolve_split(root_dir: Path, split: str) -> str:
    """
    兼容 split='val' 但目录下只有 test 的情况
    """
    split_dir = root_dir / split
    if split_dir.exists():
        return split

    if split == "val":
        test_dir = root_dir / "test"
        if test_dir.exists():
            print("[Info] split='val' 不存在，自动使用 split='test' 作为验证集")
            return "test"

    raise FileNotFoundError(f"split 路径不存在: {split_dir}")


def _build_paired_samples(input_dir: Path, target_dir: Path) -> List[Tuple[Path, Path]]:
    """
    按相对路径同名配对，支持平铺目录和子目录
    """
    samples = []

    input_paths = sorted([p for p in input_dir.rglob("*") if _is_image_file(p)])
    for input_path in input_paths:
        rel_path = input_path.relative_to(input_dir)
        target_path = target_dir / rel_path

        if not target_path.exists():
            print(f"[Warning] 配对目标不存在，跳过: {target_path}")
            continue

        samples.append((input_path, target_path))

    return samples


def _read_gray_float01(path: Path) -> np.ndarray:
    img = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if img is None:
        raise RuntimeError(f"读取灰度图失败: {path}")
    return img.astype(np.float32) / 255.0


def _read_gray_auto01(path: Path) -> np.ndarray:
    """
    读取 GT 这类可能是 uint8 / uint16 的单通道图，并归一化到 [0,1]
    """
    img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError(f"读取图像失败: {path}")

    if img.ndim == 3:
        img = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

    orig_dtype = img.dtype
    img = img.astype(np.float32)

    if orig_dtype == np.uint16 or img.max() > 255:
        img = img / 65535.0
    else:
        img = img / 255.0

    return img

def mask_to_rgb(mask: np.ndarray) -> np.ndarray:
    """
    单通道 mask -> RGB 可视化图

    类别映射：
    0 -> (0, 0, 255)     水体
    1 -> (0, 255, 0)     植被
    2 -> (255, 0, 0)     裸地
    3 -> (0, 255, 255)   道路
    4 -> (255, 255, 0)   建筑
    5 -> (255, 0, 255)   山脉
    """
    if mask.ndim != 2:
        raise ValueError(f"mask 必须是单通道二维数组，当前 shape={mask.shape}")

    color_map = np.array([
        [0,   0, 255],      # 0
        [0, 255,   0],      # 1
        [255, 0,   0],      # 2
        [0, 255, 255],      # 3
        [255, 255, 0],      # 4
        [255, 0, 255],      # 5
    ], dtype=np.uint8)

    if mask.min() < 0 or mask.max() > 5:
        raise ValueError(f"mask 中存在超出 0~6 范围的像素值，当前 min={mask.min()}, max={mask.max()}")

    rgb = color_map[mask]
    return rgb


# =========================================================
# 恢复任务 Dataset
# =========================================================
class SARRestoreDataset(Dataset):
    """
    当前恢复数据结构：

    root_dir/
        train/
            lq_1bit/
            gt_16bit/
            label/      # 恢复任务中不用
        test/
            lq_1bit/
            gt_16bit/
            label/
    """

    def __init__(
        self,
        root_dir: str,
        split: str = "train",
        return_path: bool = False,
    ):
        self.root_dir = Path(root_dir)
        self.requested_split = split
        self.return_path = return_path

        self.split = _resolve_split(self.root_dir, split)

        self.lq_dir = self.root_dir / self.split / "lq_1bit"
        self.gt_dir = self.root_dir / self.split / "gt_16bit"

        if not self.lq_dir.exists():
            raise FileNotFoundError(f"lq_1bit 路径不存在: {self.lq_dir}")
        if not self.gt_dir.exists():
            raise FileNotFoundError(f"gt_16bit 路径不存在: {self.gt_dir}")

        self.samples = _build_paired_samples(self.lq_dir, self.gt_dir)

        if len(self.samples) == 0:
            raise RuntimeError(f"在 {self.lq_dir} 下没有找到任何可用样本")

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx: int):
        lq_path, gt_path = self.samples[idx]

        lq = _read_gray_float01(lq_path)
        gt = _read_gray_auto01(gt_path)

        if lq.shape != gt.shape:
            raise ValueError(
                f"LQ 与 GT 尺寸不一致: "
                f"lq={lq.shape}, gt={gt.shape}, "
                f"lq_path={lq_path}, gt_path={gt_path}"
            )

        lq = torch.from_numpy(lq).unsqueeze(0).float()  # [1,H,W]
        gt = torch.from_numpy(gt).unsqueeze(0).float()  # [1,H,W]

        if self.return_path:
            return lq, gt, str(lq_path), str(gt_path)

        return lq, gt


def create_dataloader(
    root_dir: str,
    split: str,
    batch_size: int = 4,
    num_workers: int = 4,
    shuffle=None,
    return_path: bool = False,
    pin_memory: bool = True,
    persistent_workers: bool = False,
) -> DataLoader:
    dataset = SARRestoreDataset(
        root_dir=root_dir,
        split=split,
        return_path=return_path,
    )

    if shuffle is None:
        shuffle = (split == "train")

    loader_kwargs = dict(
        dataset=dataset,
        batch_size=batch_size,
        shuffle=shuffle,
        num_workers=num_workers,
        pin_memory=pin_memory,
        drop_last=(split == "train"),
        persistent_workers=(persistent_workers and num_workers > 0),
    )

    if num_workers > 0:
        loader_kwargs["prefetch_factor"] = 2

    return DataLoader(**loader_kwargs)


# =========================================================
# 分割任务 Dataset
# =========================================================
class SARSegDataset(Dataset):
    """
    当前分割数据结构（推荐）：

    root_dir/
        train/
            1bit_restore/
            label/
        test/
            1bit_restore/
            label/

    也支持把 input_dir_name 改成 lq_1bit / gt_16bit 等。
    """

    def __init__(
        self,
        root_dir: str,
        split: str = "train",
        input_dir_name: str = "1bit_restore",
        label_dir_name: str = "label",
        num_classes: int = 6,
        ignore_index: int = 255,
        return_path: bool = False,
    ):
        self.root_dir = Path(root_dir)
        self.requested_split = split
        self.return_path = return_path

        self.input_dir_name = input_dir_name
        self.label_dir_name = label_dir_name
        self.num_classes = num_classes
        self.ignore_index = ignore_index

        self.split = _resolve_split(self.root_dir, split)

        self.input_dir = self.root_dir / self.split / self.input_dir_name
        self.label_dir = self.root_dir / self.split / self.label_dir_name

        if not self.input_dir.exists():
            raise FileNotFoundError(f"分割输入路径不存在: {self.input_dir}")
        if not self.label_dir.exists():
            raise FileNotFoundError(f"标签路径不存在: {self.label_dir}")

        self.samples = _build_paired_samples(self.input_dir, self.label_dir)

        if len(self.samples) == 0:
            raise RuntimeError(f"在 {self.input_dir} 下没有找到任何可用分割样本")

    def __len__(self):
        return len(self.samples)

    def _read_input(self, path: Path) -> np.ndarray:
        """
        分割输入默认为单通道恢复图 / 1bit 图
        """
        img = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if img is None:
            raise RuntimeError(f"读取分割输入失败: {path}")
        img = img.astype(np.float32) / 255.0
        return img

    def _read_mask(self, path: Path) -> np.ndarray:
        """
        标签读取规则：
        - 输入应为单通道 mask
        - 1 ~ num_classes  -> 0 ~ num_classes-1
        - 其他值 -> ignore_index

        例如 num_classes=6 时：
        1,2,3,4,5,6 -> 0,1,2,3,4,5
        0 / 255 / 其他异常值 -> ignore_index
        """
        mask = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if mask is None:
            raise RuntimeError(f"读取标签失败: {path}")

        mask = mask.astype(np.int64)

        out = np.full_like(mask, self.ignore_index, dtype=np.int64)
        valid = (mask >= 1) & (mask <= self.num_classes)
        out[valid] = mask[valid] - 1

        return out

    def __getitem__(self, idx: int):
        input_path, label_path = self.samples[idx]

        image = self._read_input(input_path)
        mask = self._read_mask(label_path)

        if image.shape != mask.shape:
            raise ValueError(
                f"输入图与标签尺寸不一致: "
                f"image={image.shape}, mask={mask.shape}, "
                f"input_path={input_path}, label_path={label_path}"
            )

        image = torch.from_numpy(image).unsqueeze(0).float()  # [1,H,W]
        mask = torch.from_numpy(mask).long()                  # [H,W]

        if self.return_path:
            return image, mask, str(input_path), str(label_path)

        return image, mask


def create_seg_dataloader(
    root_dir: str,
    split: str,
    input_dir_name: str = "1bit_restore",
    label_dir_name: str = "label",
    num_classes: int = 6,
    ignore_index: int = 255,
    batch_size: int = 4,
    num_workers: int = 4,
    shuffle=None,
    return_path: bool = False,
    pin_memory: bool = True,
    persistent_workers: bool = False,
) -> DataLoader:
    dataset = SARSegDataset(
        root_dir=root_dir,
        split=split,
        input_dir_name=input_dir_name,
        label_dir_name=label_dir_name,
        num_classes=num_classes,
        ignore_index=ignore_index,
        return_path=return_path,
    )

    if shuffle is None:
        shuffle = (split == "train")

    loader_kwargs = dict(
        dataset=dataset,
        batch_size=batch_size,
        shuffle=shuffle,
        num_workers=num_workers,
        pin_memory=pin_memory,
        drop_last=(split == "train"),
        persistent_workers=(persistent_workers and num_workers > 0),
    )

    if num_workers > 0:
        loader_kwargs["prefetch_factor"] = 2

    return DataLoader(**loader_kwargs)


if __name__ == "__main__":
    import matplotlib.pyplot as plt

    root_dir = r"G:\VSCODE-G\AIRPolarSARSeg_process\Prepared"

    # -------------------------
    # 恢复 dataset 测试
    # -------------------------
    train_loader = create_dataloader(
        root_dir=root_dir,
        split="test",
        batch_size=2,
        num_workers=0,
        persistent_workers=False,
    )

    print(f"[Restore] train dataset size: {len(train_loader.dataset)}")
    lq, gt = next(iter(train_loader))
    print("[Restore] LQ shape:", lq.shape)
    print("[Restore] GT shape:", gt.shape)

    # -------------------------
    # 分割 dataset 测试
    # -------------------------
    seg_loader = create_seg_dataloader(
        root_dir=root_dir,
        split="test",
        input_dir_name="1bit_restore",   # 也可改成 lq_1bit
        label_dir_name="label",
        num_classes=6,
        batch_size=2,
        num_workers=0,
        persistent_workers=False,
        shuffle=True
    )

    print(f"[Seg] train dataset size: {len(seg_loader.dataset)}")
    img, mask = next(iter(seg_loader))
    print("[Seg] image shape:", img.shape)   # [B,1,H,W]
    print("[Seg] mask shape:", mask.shape)   # [B,H,W]
    print("[Seg] image min/max:", img.min().item(), img.max().item())
    print("[Seg] mask unique:", torch.unique(mask))

    num_show = min(2, img.shape[0])
    fig, axes = plt.subplots(2, num_show, figsize=(5 * num_show, 8))

    if num_show == 1:
        axes = np.array(axes).reshape(2, 1)

    for i in range(num_show):
        axes[0, i].imshow(img[i, 0].numpy(), cmap="gray", vmin=0.0, vmax=1.0)
        axes[0, i].set_title(f"Seg Input #{i}")
        axes[0, i].axis("off")

        axes[1, i].imshow(mask_to_rgb(mask[i].numpy()))
        axes[1, i].set_title(f"Mask #{i}")
        axes[1, i].axis("off")

    plt.tight_layout()
    plt.show()
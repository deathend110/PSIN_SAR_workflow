import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path


def mask_to_rgb(mask: np.ndarray) -> np.ndarray:
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


def read_restore_ftmp(ftmp_path: str, h: int = 512, w: int = 512) -> np.ndarray:
    data = np.fromfile(ftmp_path, dtype=np.float32)
    expected = 1 * h * w * 1
    if data.size != expected:
        raise ValueError(
            f"恢复图 ftmp 大小不匹配: data.size={data.size}, expected={expected}, file={ftmp_path}"
        )

    img = data.reshape(1, h, w, 1).squeeze().astype(np.float32)

    if img.max() > 2.0:
        img = img / 255.0

    img = np.clip(img, 0.0, 1.0)
    return img


def read_seg_ftmp(ftmp_path: str, c: int = 6, h: int = 512, w: int = 512) -> np.ndarray:
    data = np.fromfile(ftmp_path, dtype=np.float32)
    expected = 1 * h * w * c
    if data.size != expected:
        raise ValueError(
            f"分割图 ftmp 大小不匹配: data.size={data.size}, expected={expected}, file={ftmp_path}"
        )

    logits = data.reshape(1, h, w, c).astype(np.float32)   # NHWC
    return logits


def find_two_ftmps(ftmp_dir: Path):
    ftmp_files = sorted(ftmp_dir.glob("*.ftmp"))
    if not ftmp_files:
        raise FileNotFoundError(f"目录下未找到 ftmp 文件: {ftmp_dir}")

    restore_file = None
    seg_file = None

    restore_size = 1 * 512 * 512
    seg_size = 1 * 512 * 512 * 6

    for p in ftmp_files:
        n = p.stat().st_size // 4
        if n == restore_size:
            restore_file = p
        elif n == seg_size:
            seg_file = p

    return restore_file, seg_file, ftmp_files


if __name__ == "__main__":
    ftmp_dir = Path(r"2_compile\.icraft\logs\RAAUNet_DeepCA48_MobileNetV3Unet_V1\ftmpHFB")

    restore_ftmp, seg_ftmp, all_ftmps = find_two_ftmps(ftmp_dir)

    print("当前目录下 ftmp 文件：")
    for p in all_ftmps:
        elem_num = p.stat().st_size // 4
        print(f"  {p.name:<20s}  float32元素数={elem_num}")

    if restore_ftmp is None or seg_ftmp is None:
        raise RuntimeError(
            "未能自动识别恢复图和分割图对应的 ftmp 文件。\n"
            "请检查目录下是否确实存在：\n"
            "- 一个 1x512x512x1 的单通道恢复输出\n"
            "- 一个 1x512x512x6 的分割输出\n"
        )

    print(f"\n恢复图 ftmp: {restore_ftmp.name}")
    print(f"分割图 ftmp: {seg_ftmp.name}")

    restore_img = read_restore_ftmp(str(restore_ftmp), h=512, w=512)
    print(restore_img.min(), restore_img.max(), restore_img.mean())
    seg_logits = read_seg_ftmp(str(seg_ftmp), c=6, h=512, w=512)   # [1,512,512,6]
    seg_mask = np.argmax(seg_logits, axis=-1).squeeze(0).astype(np.uint8)   # [512,512]
    seg_rgb = mask_to_rgb(seg_mask)

    plt.figure(figsize=(12, 5))

    plt.subplot(1, 2, 1)
    plt.imshow(restore_img, cmap="gray")
    plt.title(f"Restore: {restore_ftmp.name}")
    plt.axis("off")

    plt.subplot(1, 2, 2)
    plt.imshow(seg_rgb)
    plt.title(f"Seg RGB: {seg_ftmp.name}")
    plt.axis("off")

    plt.tight_layout()
    plt.show()
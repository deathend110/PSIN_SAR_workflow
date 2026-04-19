import argparse
from pathlib import Path
from typing import Iterable, Tuple

import cv2
import numpy as np


DEFAULT_OUTPUT_DIR = Path("examples/1_input_ai_HDMI/io/output/3_hh_amp")
PATCH_SIZE = 512
NUM_CLASSES = 6


def mask_to_rgb(mask: np.ndarray) -> np.ndarray:
    """
    训练标签 mask(0~5) -> RGB 可视化图
    """
    if mask.ndim != 2:
        raise ValueError(f"mask 必须是单通道二维数组，当前 shape={mask.shape}")

    color_map = np.array(
        [
            [0, 0, 255],  # 0 水体
            [0, 255, 0],  # 1 植被
            [255, 0, 0],  # 2 裸地
            [0, 255, 255],  # 3 道路
            [255, 255, 0],  # 4 建筑
            [255, 0, 255],  # 5 山脉
        ],
        dtype=np.uint8,
    )

    if mask.min() < 0 or mask.max() > 5:
        raise ValueError(
            f"mask 中存在超出 0~5 范围的像素值，min={mask.min()}, max={mask.max()}"
        )

    return color_map[mask]


def iter_patch_dirs(output_dir: Path) -> Iterable[Path]:
    """按 patch_000000 这种目录名排序遍历推理输出。"""
    if not output_dir.exists():
        raise FileNotFoundError(f"输出目录不存在: {output_dir}")

    patch_dirs = [p for p in output_dir.iterdir() if p.is_dir() and p.name.startswith("patch_")]
    if not patch_dirs:
        raise FileNotFoundError(f"没有找到 patch_xxxxxx 子目录: {output_dir}")

    yield from sorted(patch_dirs, key=lambda p: p.name)


def read_ftmp_float32(path: Path, expected_count: int) -> np.ndarray:
    """读取当前 dump 出来的 SFB .ftmp。这里按纯 float32 裸数据处理。"""
    if not path.exists():
        raise FileNotFoundError(f"缺少文件: {path}")

    data = np.fromfile(path, dtype=np.float32)
    if data.size != expected_count:
        raise ValueError(
            f"{path} 数据量不匹配: got={data.size}, expected={expected_count}. "
            "如果 dump_format 不是 SFB 或输出 dtype 不是 FP32，需要按实际格式调整读取逻辑。"
        )
    return data


def normalize_gray_to_u8(gray: np.ndarray) -> np.ndarray:
    """把 float32 灰度图拉伸到 uint8，便于显示。"""
    gray = np.asarray(gray, dtype=np.float32)
    min_val = float(np.min(gray))
    max_val = float(np.max(gray))
    if max_val - min_val < 1e-12:
        return np.zeros(gray.shape, dtype=np.uint8)
    return ((gray - min_val) * (255.0 / (max_val - min_val))).clip(0, 255).astype(np.uint8)


def load_patch_pair(patch_dir: Path) -> Tuple[np.ndarray, np.ndarray]:
    """
    out_0.ftmp: [1,512,512,1] float32，显示为左侧灰度图。
    out_1.ftmp: [1,512,512,6] float32，按最后一维类别通道 argmax 得到 mask。

    注意：infer.py 里的 PyTorch logits 是 [1,C,H,W]，所以 argmax(dim=1)。
    这里 ZG dump 的模型输出日志是 [1,H,W,C]，所以要 reshape 成 [H,W,C]，
    再 argmax(axis=-1)，不能按 [C,H,W] 解释。
    """
    gray_data = read_ftmp_float32(patch_dir / "out_0.ftmp", PATCH_SIZE * PATCH_SIZE)
    logits_data = read_ftmp_float32(
        patch_dir / "out_1.ftmp", NUM_CLASSES * PATCH_SIZE * PATCH_SIZE
    )

    gray = gray_data.reshape(PATCH_SIZE, PATCH_SIZE)
    logits = logits_data.reshape(PATCH_SIZE, PATCH_SIZE, NUM_CLASSES)
    mask = np.argmax(logits, axis=-1).astype(np.uint8)
    mask_rgb = mask_to_rgb(mask)
    return normalize_gray_to_u8(gray), mask_rgb


def make_side_by_side(gray_u8: np.ndarray, mask_rgb: np.ndarray) -> np.ndarray:
    """左侧灰度图，右侧 RGB 分割图，拼成 OpenCV 可显示的 BGR 图。"""
    gray_bgr = cv2.cvtColor(gray_u8, cv2.COLOR_GRAY2BGR)
    mask_bgr = cv2.cvtColor(mask_rgb, cv2.COLOR_RGB2BGR)
    return np.hstack([gray_bgr, mask_bgr])


def main() -> None:
    parser = argparse.ArgumentParser(description="显示 ZG 推理 dump 的 .ftmp 分割结果")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help=f"包含 patch_xxxxxx/out_*.ftmp 的目录，默认: {DEFAULT_OUTPUT_DIR}",
    )
    parser.add_argument(
        "--delay",
        type=int,
        default=0,
        help="每张图显示等待毫秒数。0 表示手动按键切换，默认 0。",
    )
    args = parser.parse_args()

    window_name = "left: gray | right: segmentation mask"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)

    patch_dirs = list(iter_patch_dirs(args.output_dir))
    for index, patch_dir in enumerate(patch_dirs):
        gray_u8, mask_rgb = load_patch_pair(patch_dir)
        canvas = make_side_by_side(gray_u8, mask_rgb)

        cv2.imshow(window_name, canvas)
        print(f"[{index + 1}/{len(patch_dirs)}] showing {patch_dir.name}")

        key = cv2.waitKey(args.delay)
        if key in (27, ord("q")):
            break

    cv2.destroyAllWindows()


if __name__ == "__main__":
    x = np.fromfile(r"examples/1_input_ai_HDMI/io/output/3_hv_amp/patch_000000/out_0.ftmp", dtype=np.float32)
    print(x.min(), x.max(), x.mean())

    # main()

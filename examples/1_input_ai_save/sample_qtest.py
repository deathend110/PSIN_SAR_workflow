'''
从test测试集抽取qtest量化校准集
抽取逻辑：
从 Prepared/test 中按“同一位置组”抽样：
    - 每次随机抽取一个 group（base + patch_id）
    - 同时导出该 group 对应的 4 种极化 patch

    输出目录结构：
    qtest/
    ├── img/      保存 1bit PNG
    ├── ftmp/     保存 1bit 转出的 .ftmp
    ├── img_gt/   保存对应 GT PNG
    └── label/    保存对应 label PNG

    说明：
    - .ftmp 由 LQ 图生成
    - 灰度读取 LQ
    - 保存 .ftmp 时转 float32
    - 不做 /255，保持 0~255
    - NHWC: [1, H, W, 1]
    - 只生成一个 ftmp_list.txt
'''
import random
import shutil
from pathlib import Path
import os
import cv2
import numpy as np


def parse_group_key_and_pol(stem, pols):
    """
    例如:
        base1_hh_amp_frame_001 -> ('base1__frame_001', 'hh_amp')
        base2_vv_amp_frame_123 -> ('base2__frame_123', 'vv_amp')
    """
    for pol in pols:
        token = f"_{pol}_"
        if token in stem:
            group_key = stem.replace(token, "__", 1)
            return group_key, pol
    return None, None


def export_random_grouped_qtest_from_prepared(
    prepared_test_root,
    qtest_root,
    txt_list_path,
    num_groups=50,
    img_prefix="qtest",
    img_gt_prefix="qtest_gt",
    label_prefix="qtest_label",
    ftmp_prefix="qtest",
    seed=42,
    pols=("hh_amp", "hv_amp", "vh_amp", "vv_amp"),
    exts=(".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"),
):
    """
    从 Prepared/test 中按“同一位置组”抽样：
    - 每次随机抽取一个 group（base + patch_id）
    - 同时导出该 group 对应的 4 种极化 patch

    输出目录结构：
    qtest/
    ├── img/      保存 1bit PNG
    ├── ftmp/     保存 1bit 转出的 .ftmp
    ├── img_gt/   保存对应 GT PNG
    └── label/    保存对应 label PNG

    说明：
    - .ftmp 由 LQ 图生成
    - 灰度读取 LQ
    - 保存 .ftmp 时转 float32
    - 不做 /255，保持 0~255
    - NHWC: [1, H, W, 1]
    - 只生成一个 ftmp_list.txt
    """
    random.seed(seed)

    prepared_test_root = Path(prepared_test_root)
    lq_root = prepared_test_root / "lq_1bit"
    gt_root = prepared_test_root / "gt_16bit"
    label_root = prepared_test_root / "label"

    qtest_root = Path(qtest_root)
    img_output_dir = qtest_root / "img"
    ftmp_output_dir = qtest_root / "ftmp"
    img_gt_output_dir = qtest_root / "img_gt"
    label_output_dir = qtest_root / "label"
    txt_list_path = Path(txt_list_path)

    if not lq_root.exists():
        raise FileNotFoundError(f"lq_1bit 不存在: {lq_root}")
    if not gt_root.exists():
        raise FileNotFoundError(f"gt_16bit 不存在: {gt_root}")
    if not label_root.exists():
        raise FileNotFoundError(f"label 不存在: {label_root}")

    img_output_dir.mkdir(parents=True, exist_ok=True)
    ftmp_output_dir.mkdir(parents=True, exist_ok=True)
    img_gt_output_dir.mkdir(parents=True, exist_ok=True)
    label_output_dir.mkdir(parents=True, exist_ok=True)
    txt_list_path.parent.mkdir(parents=True, exist_ok=True)

    # --------------------------------------------------
    # 1) 从 lq_1bit 中构建 group
    # --------------------------------------------------
    group_map = {}  # group_key -> {pol: filename}
    lq_files = [
        p for p in lq_root.iterdir()
        if p.is_file() and p.suffix.lower() in exts
    ]

    if not lq_files:
        raise ValueError(f"{lq_root} 下没有图片文件")

    for p in lq_files:
        group_key, pol = parse_group_key_and_pol(p.stem, pols)
        if group_key is None:
            continue
        group_map.setdefault(group_key, {})
        group_map[group_key][pol] = p.name

    # 只保留四极化齐全且 gt / label 都存在的 group
    valid_groups = []

    for group_key, pol_dict in group_map.items():
        if not all(pol in pol_dict for pol in pols):
            continue

        ok = True
        for pol in pols:
            fname = pol_dict[pol]
            if not (gt_root / fname).exists():
                ok = False
                break
            if not (label_root / fname).exists():
                ok = False
                break

        if ok:
            valid_groups.append(group_key)

    if len(valid_groups) < num_groups:
        raise ValueError(
            f"可用完整 group 数量不足：需要 {num_groups} 个，但实际只有 {len(valid_groups)} 个。"
        )

    selected_groups = random.sample(valid_groups, num_groups)
    ftmp_names = []

    total_groups = len(selected_groups)
    total_samples = total_groups * len(pols)
    sample_counter = 0

    # --------------------------------------------------
    # 2) 导出每个 group 下的四极化样本
    # --------------------------------------------------
    for group_idx, group_key in enumerate(selected_groups, start=1):
        pol_dict = group_map[group_key]

        for pol in pols:
            sample_counter += 1
            src_name = pol_dict[pol]

            src_lq = lq_root / src_name
            src_gt = gt_root / src_name
            src_label = label_root / src_name

            # 保存文件名
            img_name = f"{img_prefix}{group_idx:03d}_{pol}.png"
            img_gt_name = f"{img_gt_prefix}{group_idx:03d}_{pol}.png"
            label_name = f"{label_prefix}{group_idx:03d}_{pol}.png"
            ftmp_name = f"{ftmp_prefix}_{group_idx:03d}_{pol}.ftmp"

            dst_lq = img_output_dir / img_name
            dst_gt = img_gt_output_dir / img_gt_name
            dst_label = label_output_dir / label_name
            dst_ftmp = ftmp_output_dir / ftmp_name

            # ---------- 1) 保存 1bit PNG ----------
            shutil.copy2(src_lq, dst_lq)

            # ---------- 2) 保存 GT PNG ----------
            shutil.copy2(src_gt, dst_gt)

            # ---------- 3) 保存 Label PNG ----------
            shutil.copy2(src_label, dst_label)

            # ---------- 4) 基于保存后的 LQ PNG 转 FTMP ----------
            img = cv2.imread(str(dst_lq), cv2.IMREAD_GRAYSCALE)
            if img is None:
                raise ValueError(f"无法读取图片：{dst_lq}")

            img = img.astype(np.float32)   # 保持 0~255
            h, w = img.shape
            img = img.reshape(1, h, w, 1)  # NHWC

            img.tofile(dst_ftmp)
            ftmp_names.append(ftmp_name)

            print(
                f"[Group {group_idx:03d}/{total_groups:03d}] "
                f"[Sample {sample_counter:03d}/{total_samples:03d}] "
                f"{pol} | "
                f"IMG: {dst_lq.name} | "
                f"FTMP: {dst_ftmp.name} | "
                f"GT: {dst_gt.name} | "
                f"Label: {dst_label.name}"
            )

    # --------------------------------------------------
    # 3) 保存 ftmp_list.txt
    # --------------------------------------------------
    with open(txt_list_path, "w", encoding="utf-8") as f:
        for name in ftmp_names:
            f.write(name + "\n")

    print(f"\n完成，共导出 {total_groups} 个 group，{len(ftmp_names)} 个 ftmp。")
    print(f"ftmp_list.txt 已保存到：{txt_list_path}")


if __name__ == "__main__":
    prepared_test_root = r"G:\VSCODE-G\AIRPolarSARSeg_process\Prepared\train"

    qtest_root = r".\2_compile\qtest"
    txt_list_path = r".\2_compile\qtest\ftmp_list.txt"

    os.makedirs(qtest_root, exist_ok=True)

    export_random_grouped_qtest_from_prepared(
        prepared_test_root=prepared_test_root,
        qtest_root=qtest_root,
        txt_list_path=txt_list_path,
        num_groups=10,   # 抽 50 个位置组，最终导出 50*4 = 200 个样本
        img_prefix="qtest",
        img_gt_prefix="qtest_gt",
        label_prefix="qtest_label",
        ftmp_prefix="qtest",
        seed=45,
    )
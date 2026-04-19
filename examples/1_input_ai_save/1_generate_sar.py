import os
import numpy as np
import cupy as cp
import tifffile as tiff
import imageio.v2 as imageio
from pathlib import Path
from tqdm import tqdm
from dataclasses import dataclass
from typing import Dict, Optional, Tuple
import matplotlib.pyplot as plt
import cv2 as cv
from skimage import color
from skimage.metrics import peak_signal_noise_ratio, structural_similarity
import gc

def calc_metrics(img_ref, img_pred):
    # PSNR
    psnr = peak_signal_noise_ratio(img_ref, img_pred, data_range=img_pred.max() - img_pred.min())

    # SSIM
    ssim = structural_similarity(img_ref, img_pred, data_range=img_pred.max() - img_pred.min())

    return psnr, ssim

# ==========================================
# 1. 参数配置类
# ==========================================


@dataclass
class RadarConfig:
    c: float = 3e8
    fc: float = 36.01e9
    B: float = 30e6
    Tp: float = 1e-6
    Fs: float = 4 * B
    PRF: float = 480
    v_platform: float = 10
    R0: float = 400
    noise_enable: bool = True  # 新增：是否注入回波噪声
    snr_db: float = -2.0  # 新增：信噪比 (dB)，仅在 noise_enable=True 时生效
    

    @property
    def lam(self): return self.c / self.fc
    @property
    def gamma(self): return self.B / self.Tp
    @property
    def noise_factor(self):
        """根据SNR计算噪声系数"""
        return 10**(-self.snr_db / 10)


@dataclass
class MotionConfig:
    motion_blur_enable: bool = False  # 新增：是否开启运动干扰
    v_std: float = 0.001
    v_bias: float = 0.001
    rtk_enable: bool = True
    rtk_interval_seconds: float = 0.5
    rtk_accuracy: float = 0.01
    correlation_length: int = 100

# ==========================================
# 2. 运动模拟模块
# ==========================================


class MotionSimulator:
    def __init__(self, radar_cfg: RadarConfig, motion_cfg: MotionConfig):
        self.r = radar_cfg
        self.m = motion_cfg

    def generate_trajectory(self, na: int) -> Tuple[cp.ndarray, cp.ndarray]:
        """
        生成平台轨迹。
        如果 motion_blur_enable 为 False，则返回理想的线性轨迹。
        """
        t_a = np.arange(na) / self.r.PRF

        # 理想情况：恒定速度，无位置偏差
        if not self.m.motion_blur_enable:
            v_actual = np.full(na, self.r.v_platform, dtype=np.float32)
            # 理想位移 x = v * t，并做中心化处理
            x_actual = self.r.v_platform * t_a
            x_actual -= x_actual[na // 2]
            return cp.asarray(x_actual, dtype=cp.float32), cp.asarray(v_actual, dtype=cp.float32)

        # 干扰情况：生成相关噪声扰动
        alpha = np.exp(-1.0 / self.m.correlation_length)
        white_noise = np.random.randn(na)
        v_corr = np.zeros(na)
        v_corr[0] = white_noise[0]
        for i in range(1, na):
            v_corr[i] = alpha * v_corr[i-1] + \
                np.sqrt(1 - alpha**2) * white_noise[i]

        v_perturb = (v_corr / np.std(v_corr) * self.m.v_std) + self.m.v_bias

        # RTK 约束处理
        if self.m.rtk_enable:
            rtk_points = np.arange(0, t_a[-1], self.m.rtk_interval_seconds)
            for pt in rtk_points:
                mask = np.abs(t_a - pt) < 0.1
                v_perturb[mask] *= (self.m.rtk_accuracy *
                                    10 * (1 - np.abs(t_a[mask] - pt)/0.1))

        v_actual = self.r.v_platform + v_perturb
        dt = 1.0 / self.r.PRF

        # 积分生成实际轨迹
        x_actual = np.zeros(na)
        if self.m.rtk_enable:
            rtk_indices = (rtk_points * self.r.PRF).astype(int)
            curr_pos = 0
            start_i = 0
            for end_i in rtk_indices:
                if end_i >= na:
                    break
                if end_i > start_i:
                    seg_pos = np.cumsum(
                        v_actual[start_i:end_i] * dt) + curr_pos
                    x_actual[start_i:end_i] = seg_pos
                    curr_pos = seg_pos[-1]
                ideal_pos = self.r.v_platform * (end_i * dt)
                curr_pos = 0.9 * ideal_pos + 0.1 * curr_pos
                x_actual[min(end_i, na-1)] = curr_pos
                start_i = end_i
            if start_i < na:
                x_actual[start_i:] = np.cumsum(
                    v_actual[start_i:] * dt) + curr_pos
        else:
            x_actual = np.cumsum(v_actual * dt)

        x_actual -= x_actual[na // 2]
        return cp.asarray(x_actual, dtype=cp.float32), cp.asarray(v_actual, dtype=cp.float32)

# ==========================================
# 3. SAR 核心处理模块
# ==========================================


class SARProcessor:
    def __init__(self, cfg: RadarConfig):
        self.cfg = cfg

    def _get_f_axes(self, nr: int, na: int):
        f_r = cp.fft.fftshift(cp.fft.fftfreq(nr, d=1/self.cfg.Fs))
        f_a = cp.fft.fftshift(cp.fft.fftfreq(na, d=1/self.cfg.PRF))
        return f_r, f_a

    def vectorized_rcmc(self, data_fa: cp.ndarray, f_a: cp.ndarray, v_actual: cp.ndarray, inverse=False):
        """全向量化距离徙动处理"""
        nr, na = data_fa.shape
        r_idx = cp.arange(nr, dtype=cp.float32)
        delta_r = self.cfg.c / (2 * self.cfg.Fs)

        # 使用实际速度计算徙动量
        delta_R = self.cfg.R0 * (self.cfg.lam * f_a)**2 / (8 * v_actual**2)
        shift = delta_R / delta_r

        map_r = r_idx[:, None] + \
            (shift[None, :] if not inverse else -shift[None, :])

        idx0 = cp.floor(map_r).astype(cp.int32)
        w = map_r - idx0
        valid = (idx0 >= 0) & (idx0 < nr - 1)

        output = cp.zeros_like(data_fa)
        col_indices = cp.tile(cp.arange(na), (nr, 1))

        output[valid] = (1 - w[valid]) * data_fa[idx0[valid], col_indices[valid]] + \
            w[valid] * data_fa[idx0[valid] + 1, col_indices[valid]]
        return output

    def run_inverse(self, patch: np.ndarray, v_actual: cp.ndarray) -> np.ndarray:
        """从图像反演回波"""
        data = cp.asarray(patch, dtype=cp.complex64)
        nr, na = data.shape
        f_r, f_a = self._get_f_axes(nr, na)

        # 方位向处理
        data_fa = cp.fft.fftshift(cp.fft.fft(
            cp.fft.fftshift(data, 1), axis=1), 1)
        Ka = 2 * v_actual.mean()**2 / (self.cfg.lam * self.cfg.R0)
        Ha_conj = cp.exp(1j * cp.pi * f_a**2 / Ka)

        # 逆 RCMC
        data_ircmc = self.vectorized_rcmc(
            data_fa * Ha_conj[None, :], f_a, v_actual, inverse=True)
        data_az_decomp = cp.fft.ifft(cp.fft.ifftshift(data_ircmc, 1), axis=1)

        # 距离向处理
        Hr_conj = cp.exp(1j * cp.pi * f_r**2 / self.cfg.gamma)
        echo = cp.fft.ifft(cp.fft.ifftshift(cp.fft.fft(cp.fft.fftshift(
            data_az_decomp, 0), axis=0) * Hr_conj[:, None], 0), axis=0)

        return echo

    def run_imaging(self, data: cp.ndarray, v_actual: cp.ndarray) -> np.ndarray:
            """
            统一成像接口
            :param data: 回波数据 (cupy array)
            :param v_actual: 实际方位向速度轨迹 (cupy array)
            """
            # data = cp.asarray(data, dtype=cp.complex64)
            nr, na = data.shape
            f_r, f_a = self._get_f_axes(nr, na)

            # --- 标准 16bit 距离压缩 ---
            Hr = cp.exp(-1j * cp.pi * f_r**2 / self.cfg.gamma)
            data_rc = cp.fft.ifft(cp.fft.fftshift(cp.fft.fft(cp.fft.fftshift(data, 0), axis=0), 0) * Hr[:, None], axis=0)

            # --- 后续 RD 算法流程 (16bit/1bit 通用) ---
            
            # 1. 方位向 FFT
            data_fa = cp.fft.fftshift(cp.fft.fft(
                cp.fft.fftshift(data_rc, 1), axis=1), 1)
            
            # 2. 距离徙动校正 (RCMC)
            data_rcmc = self.vectorized_rcmc(data_fa, f_a, v_actual)

            # 3. 方位压缩
            # 使用平均速度计算方位向调频率 Ka
            Ka = 2 * self.cfg.v_platform**2 / (self.cfg.lam * self.cfg.R0)
            Ha = cp.exp(-1j * cp.pi * f_a**2 / Ka)
            img = cp.fft.ifft(cp.fft.ifftshift(data_rcmc * Ha[None, :], 1), axis=1)

            # 返回complex的img
            return cp.asnumpy(img)
    

# ==========================================
# 4. 视频流生成管线
# ==========================================


class SARVideoPipeline:
    def __init__(self, radar_cfg: RadarConfig, motion_cfg: MotionConfig, black_frame_threshold=0.2):
        self.radar_cfg = radar_cfg
        self.motion_cfg = motion_cfg
        self.black_frame_threshold = black_frame_threshold
        self.motion_sim = MotionSimulator(radar_cfg, motion_cfg)
        self.processor = SARProcessor(radar_cfg)

    def gaussian_noisy(self, signal, factor):
        # 正确计算回波幅度
        sigma = cp.std(cp.abs(signal))

        # 设置阈值幅度 (As = 1.4 * σ)
        target_noise_std = factor * sigma
        
        # 3. 设定伸缩因子
        # 这里的关键是除以 sqrt(2)，因为复数高斯噪声由实部和虚部组成。
        # 若实部和虚部均服从 N(0, σ^2/2)，则总复数方差才是 σ^2
        scale_factor = target_noise_std / cp.sqrt(2.0)
        
        # 4. 生成复高斯白噪声
        # cp.random.randn 生成标准正态分布 (均值0，方差1)
        # *signal.shape 可以完美适配 1D, 2D 甚至 N 维度的信号矩阵
        noise = scale_factor * (cp.random.randn(*signal.shape) + 1j * cp.random.randn(*signal.shape))
        
        # 强制让噪声均值为 0 (对应 MATLAB 的 mean(Noise, "all"))
        noise = noise - cp.mean(noise)
        
        # 5. 叠加噪声
        signal_noisy = signal + noise
        return signal_noisy

    def minmaxnorm(self, img_rec):
        img_rec = np.abs(img_rec)
        v_max = np.max(img_rec)
        v_min = np.min(img_rec)
        img_norm = (img_rec - v_min) / (v_max - v_min)
        return img_norm

    def cut_save_img(self, img, patch_size, stride_rate, tif_path, output_dir):
        save_path = os.path.join(output_dir, f"{Path(tif_path).stem}")
        os.makedirs(save_path, exist_ok=True)

        H, W = img.shape
        ph, pw = patch_size
        stride = int(pw * stride_rate)
        # 每行不重叠
        ph_stride = int(ph)
        
        i = 0
        for _, y in enumerate(range(0, H - ph + 1, ph_stride)):
            for x in range(0, W - pw + 1, stride):
                patch = img[y:y+ph, x:x+pw]

                # plt.imshow(patch, cmap='gray'),plt.axis('off'),plt.title("1bit")
                # plt.gca().set_aspect('equal', adjustable='box')
                # plt.show()

                imageio.imwrite(os.path.join(save_path, f"frame_{i:03d}.png"), patch)
                i+=1

    def save_img(self, img, patch_size, stride_rate, tif_path, output_dir):
        
        save_path = os.path.join(output_dir, f"{Path(tif_path).stem}"+".png")
        os.makedirs(output_dir, exist_ok=True)
        imageio.imwrite(save_path, img)


    def clear_cupy_memory(self):
        gc.collect()
        cp.get_default_memory_pool().free_all_blocks()
        cp.get_default_pinned_memory_pool().free_all_blocks()

    def run(self, input_dir: str, output_dir: str, patch_size=(512, 512), stride_rate=0.5):
        '''
        一次读取直接生成三个，gt_16bit gt_16bit_noisy lq_1bit
        '''
        tif_files = sorted([str(p) for p in Path(input_dir).glob("*.tif*")])
        for tif_path in tqdm(tif_files, desc="Processing Files", ncols=100):
            # 0 清显存CUDA
            self.clear_cupy_memory()

            # 1 一次读一张完整的img图，反演回信号echo
            img = tiff.imread(tif_path)
            
            img = np.float32(img) / 255

            # 反演回波
            # 基础预处理
            img_c = (img * np.exp(1j * 2 * np.pi *np.random.rand(*img.shape))).astype(np.complex64)
            # 1. 获取轨迹 (由 motion_blur_enable 决定是否理想)
            _, v_act = self.motion_sim.generate_trajectory(img.shape[1])
            
            # 2. 生成回波
            factor = 1
            echo = self.processor.run_inverse(img_c, v_act)
            echo_noisy = self.gaussian_noisy(echo, factor=factor)
            echo_noisy_1bit = cp.sign(cp.real(echo_noisy)) + 1j * cp.sign(cp.imag(echo_noisy))

            # 3. 生成RD图
            img_rec = self.processor.run_imaging(echo, v_act)
            # img_rec_noisy = self.processor.run_imaging(echo_noisy, v_act)
            img_rec_1bit = self.processor.run_imaging(echo_noisy_1bit, v_act)

            # 以整张图为单位做minmax归一化，得到img_norm
            # 在x255转uint8保存
            img_norm = np.uint8(self.minmaxnorm(img_rec) * 255)
            # img_norm_noisy = np.uint8(self.minmaxnorm(img_rec_noisy) * 255)
            img_norm_1bit = np.uint8(self.minmaxnorm(img_rec_1bit) * 255)

            gt_path = os.path.join(output_dir, "gt_16bit")
            self.cut_save_img(img_norm, patch_size, stride_rate=stride_rate, tif_path=tif_path, output_dir=gt_path)

            # gt_noisy_path = os.path.join(output_dir, "gt_16bit_noisy")
            # self.cut_save_img(img_norm_noisy, patch_size, stride_rate=stride_rate, tif_path=tif_path, output_dir=gt_noisy_path)
            
            lq_path = os.path.join(output_dir, "lq_1bit")
            self.cut_save_img(img_norm_1bit, patch_size, stride_rate=stride_rate, tif_path=tif_path, output_dir=lq_path)

            
            pass

        return


# ==========================================
# 主入口
# ==========================================


if __name__ == "__main__":
    '''
    1024版本
    '''
    # 配置理想环境：关闭运动模糊，关闭噪声
    r_cfg = RadarConfig(noise_enable=False, snr_db=4)  # 注入回波噪声以增加真实感
    m_cfg = MotionConfig(motion_blur_enable=False)  # 开启运动干扰以模拟真实轨迹

    pipeline = SARVideoPipeline(r_cfg, m_cfg, black_frame_threshold=0.2,)
    
    
    mode = "test"
    input_dir = "G:\VSCODE-G\AIRPolarSARSeg_process\AIRPolarSARSeg"
    # input_dir = input_dir.replace("tmp", mode)
    input_dir = Path(input_dir)
    input_dir.mkdir(parents=True, exist_ok=True)

    # 输出目录，分为trian，test，val
    # 会自动在目录下生成：gt_16bit gt_16bit_noisy lq_1bit
    output_dir = "/root/autodl-tmp/AIRPolarSARSeg_process/Data"
    # output_dir = output_dir.replace("tmp", mode)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # 重复率2分之一，提高数据集差异
    pipeline.run(
        input_dir=input_dir,
        output_dir=output_dir,
        patch_size=(512, 512),
        stride_rate=0.5,
    )


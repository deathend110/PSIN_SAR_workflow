import numpy as np
from scipy.ndimage import map_coordinates
from typing import Tuple
import cv2  # OpenCV 是跨平台的
import os   # os 模块也是跨平台的

class SARProcessor:
    def __init__(self, 
                 c: float = 3e8,
                 fc: float = 36.01e9,
                 B: float = 30e6,
                 Tp: float = 1e-6,
                 Fs: float = 4 * 30e6,
                 PRF: float = 480,
                 v_platform: float = 10,
                 R0: float = 400,
                 noise_enable: bool = False,
                 snr_db: float = 20.0,
                 na: int = 512,
                 nr: int = 512,
                ):
        
        # 雷达配置参数
        self.c = c
        self.fc = fc
        self.B = B
        self.Tp = Tp
        self.Fs = Fs
        self.PRF = PRF
        self.v_platform = v_platform
        self.R0 = R0
        self.noise_enable = noise_enable
        self.snr_db = snr_db
        self.nr = nr
        self.na = na
        self.v_platform = v_platform
        
        # 派生参数
        self.lam = c / fc
        self.gamma = B / Tp
        self.noise_factor = 10 ** (-snr_db / 10)
        self.v_actual = np.full(na, self.v_platform, dtype=np.float32)
        
    def _get_f_axes(self):
        f_r = np.fft.fftshift(np.fft.fftfreq(self.nr, d=1/self.Fs))
        f_a = np.fft.fftshift(np.fft.fftfreq(self.na, d=1/self.PRF))
        return f_r, f_a

    def vectorized_rcmc(self, data_fa: np.ndarray, f_a: np.ndarray, v_actual: np.ndarray, inverse=False):
        nr, na = data_fa.shape
        r_idx = np.arange(nr, dtype=np.float32)
        delta_r = self.c / (2 * self.Fs)
        delta_R = self.R0 * (self.lam * f_a)**2 / (8 * v_actual**2)
        shift = delta_R / delta_r
        map_r = r_idx[:, None] + (shift[None, :] if not inverse else -shift[None, :])
        
        # 使用scipy进行线性插值
        valid_mask = (map_r >= 0) & (map_r < nr - 1)
        output = np.zeros_like(data_fa, dtype=np.complex64)
        
        # 为每个方位向位置创建插值坐标
        for idx_a in range(na):
            # 只对有效的距离向索引进行插值
            valid_r = valid_mask[:, idx_a]
            if np.any(valid_r):
                coords = [map_r[valid_r, idx_a], np.full(np.sum(valid_r), idx_a)]
                output[valid_r, idx_a] = map_coordinates(
                    data_fa, 
                    coords, 
                    order=1, 
                    mode='constant', 
                    cval=0.0,
                    prefilter=False
                )
        return output

    def run_inverse(self, patch: np.ndarray, v_actual: np.ndarray) -> np.ndarray:
        data = np.asarray(patch, dtype=np.complex64)
        nr, na = data.shape
        f_r, f_a = self._get_f_axes()

        data_fa = np.fft.fftshift(np.fft.fft(np.fft.fftshift(data, axes=1), axis=1), axes=1)
        Ka = 2 * v_actual.mean()**2 / (self.lam * self.R0)
        Ha_conj = np.exp(1j * np.pi * f_a**2 / Ka)

        data_ircmc = self.vectorized_rcmc(data_fa * Ha_conj[None, :], f_a, v_actual, inverse=True)
        data_az_decomp = np.fft.ifft(np.fft.ifftshift(data_ircmc, axes=1), axis=1)

        Hr_conj = np.exp(1j * np.pi * f_r**2 / self.gamma)
        echo = np.fft.ifft(np.fft.ifftshift(
            np.fft.fft(np.fft.fftshift(data_az_decomp, axes=0), axis=0) * Hr_conj[:, None], 
            axes=0
        ), axis=0)
        return echo

    def run_imaging(self, echo: np.ndarray) -> np.ndarray:
        v_actual = self.v_actual
        data = np.asarray(echo, dtype=np.complex64)
        nr, na = data.shape
        f_r, f_a = self._get_f_axes()

        Hr = np.exp(-1j * np.pi * f_r**2 / self.gamma)
        data_rc = np.fft.ifft(
            np.fft.fftshift(
                np.fft.fft(np.fft.fftshift(data, axes=0), axis=0), 
                axes=0
            ) * Hr[:, None], 
            axis=0
        )

        data_fa = np.fft.fftshift(np.fft.fft(np.fft.fftshift(data_rc, axes=1), axis=1), axes=1)
        data_rcmc = self.vectorized_rcmc(data_fa, f_a, v_actual)

        Ka = 2 * self.v_platform**2 / (self.lam * self.R0)
        Ha = np.exp(-1j * np.pi * f_a**2 / Ka)
        img = np.fft.ifft(np.fft.ifftshift(data_rcmc * Ha[None, :], axes=1), axis=1)
        return img

def minmaxnorm(data):
    """
    对数据进行min-max归一化
    """
    data_abs = np.abs(data)
    min_val = data_abs.min()
    max_val = data_abs.max()
    
    if max_val == min_val:
        return np.zeros_like(data_abs)
    else:
        return (data_abs - min_val) / (max_val - min_val)

def load_complex_from_binary(filepath, shape=None):
    """
    从二进制文件加载复数数据
    
    Args:
        filepath: 二进制文件路径
        shape: 原始数据的形状（可选，如果不提供则从文件头读取尺寸信息）
    Returns:
        重新构建的复数数组
    """
    with open(filepath, 'rb') as f:
        # 先读取尺寸信息
        size_h_bytes = f.read(4)  # 读取4字节的height
        size_w_bytes = f.read(4)  # 读取4字节的width
        
        # 转换为numpy数组获取尺寸
        size_h = np.frombuffer(size_h_bytes, dtype='<i4')[0]
        size_w = np.frombuffer(size_w_bytes, dtype='<i4')[0]
        
        # 检查shape参数是否与文件中的尺寸信息一致
        if shape is not None:
            if shape != (size_h, size_w):
                print(f"警告: 文件中的尺寸信息 ({size_h}, {size_w}) 与提供的shape {shape} 不一致")
        
        # 读取剩余的复数数据
        remaining_data = f.read()
    
    # 转换为numpy数组（单精度浮点型，小端序）
    flat_data = np.frombuffer(remaining_data, dtype='<f4')
    
    # 计算预期的数据量（实部和虚部各占一半）
    expected_size = 2 * size_h * size_w
    
    if len(flat_data) != expected_size:
        raise ValueError(f"文件大小不匹配: 期望 {expected_size}, 实际 {len(flat_data)}")
    
    # 重新确定shape
    final_shape = (size_h, size_w)
    
    # 提取实部和虚部
    real_part = flat_data[0::2].reshape(final_shape)
    imag_part = flat_data[1::2].reshape(final_shape)
    
    # 重构复数数组
    complex_data = real_part + 1j * imag_part
    
    return complex_data

def process_single_sar_image(input_path, output_path):
    """
    处理单个SAR图像文件
    
    Args:
        input_path: 输入二进制文件路径
        output_path: 输出图像保存路径
    """
    try:
        print(f"Processing file: {input_path}")
        
        # 加载复数数据
        echo = load_complex_from_binary(input_path)
        
        data_h, data_w= echo.shape

        # 初始化SAR处理器
        processor = SARProcessor(na=data_w, nr=data_h)
        
        # 执行SAR成像
        img_complex = processor.run_imaging(echo)
        
        # 进行minmax归一化，转换为uint8保存
        img_norm = np.uint8(minmaxnorm(img_complex) * 255)

        # 使用OpenCV保存图像
        success = cv2.imwrite(output_path, img_norm)
        
        if success:
            print(f"Successfully saved: {output_path}")
        # else:
        #     print(f"Failed to save: {output_path}")
        #     return False
        return img_norm
            
    except Exception as e:
        print(f"Error occurred while processing file {input_path}: {str(e)}")
        return False
import time
if __name__ == "__main__":

    # Linux 路径
    # input_directory = "/root/Data/echo/"      # Linux 风格路径
    # output_directory = "/root/Data/output_png/"    # Linux 风格路径

    # 单张处理
    input_file = r"G:\VSCODE-G\AIRPolarSARSeg_process\Data_seed42\1_hh_amp_echo_1bit.bin"  # 输入文件路径
    output_file = r"G:\VSCODE-G\AIRPolarSARSeg_process\Data_seed42\1_hh_amp_echo_1bit.png"   # 输出文件路径
    
    # 确保输出目录存在
    output_dir = os.path.dirname(output_file)
    os.makedirs(output_dir, exist_ok=True)
    
    # 处理单张图像
    start = time.time()
    result = process_single_sar_image(input_file, output_file)
    end = time.time()
    print(f"python windows RD耗时: {end - start:.6f} 秒")

import cv2
import numpy as np
from skimage.metrics import structural_similarity as ssim

a = cv2.imread(r"G:\Docker_windows_disk\PLIN_pHDMI_detpost\examples\1_input_ai_HDMI\io\output\1_hh_amp\patch_000000\input.png", cv2.IMREAD_GRAYSCALE)
b = cv2.imread(r"G:\VSCODE-G\AIRPolarSARSeg_process\Prepared\train\lq_1bit\base1_hh_amp_frame_000.png", cv2.IMREAD_GRAYSCALE)

print(a.shape, b.shape)
print("max_abs_diff:", np.abs(a.astype(np.int16) - b.astype(np.int16)).max())
print("mean_abs_diff:", np.abs(a.astype(np.int16) - b.astype(np.int16)).mean())
print("ssim:", ssim(a, b, data_range=255.0))


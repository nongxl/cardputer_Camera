#!/usr/bin/env python3
"""
LUT 预计算生成器
将滤镜算法在 PC 端对 RGB565 全色域 (65536 种颜色) 预计算，输出 C 头文件。
每个 LUT 数组占用 128KB Flash，ESP32-S3 通过 MMU 直接映射访问，不占 SRAM。

Usage:
    python tools/generate_luts.py
    # 生成文件到 include/ 目录
"""
import os
import sys
import re

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'include')
LUTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'luts')


def rgb565_to_rgb888(c):
    """RGB565 (uint16) -> (R8, G8, B8) tuple"""
    r = ((c >> 11) & 0x1F) * 255 // 31
    g = ((c >> 5) & 0x3F) * 255 // 63
    b = (c & 0x1F) * 255 // 31
    return r, g, b


def rgb555_to_rgb888(c):
    """RGB555 (15-bit index) -> (R8, G8, B8) tuple"""
    r = ((c >> 10) & 0x1F) * 255 // 31
    g = ((c >> 5) & 0x1F) * 255 // 31
    b = (c & 0x1F) * 255 // 31
    return r, g, b


def rgb888_to_rgb565(r, g, b):
    """(R8, G8, B8) -> RGB565 uint16"""
    r = max(0, min(255, r))
    g = max(0, min(255, g))
    b = max(0, min(255, b))
    return ((r * 31 // 255) << 11) | ((g * 63 // 255) << 5) | (b * 31 // 255)


def clamp(v, lo=0, hi=255):
    return max(lo, min(hi, v))


# ═══════════════════════════════════════════════════════════════
# 滤镜 1: GameBoy 经典绿 (4色灰度量化 → 绿色调色板映射)
# ═══════════════════════════════════════════════════════════════
def generate_gameboy_lut():
    palette_rgb = [
        (15, 56, 15),    # Level 0 - 最暗 #0f380f
        (48, 98, 48),    # Level 1 - 次暗 #306230
        (139, 172, 15),  # Level 2 - 次亮 #8bac0f
        (155, 188, 15),  # Level 3 - 最亮 #9bbc0f
    ]
    palette_565 = [rgb888_to_rgb565(*c) for c in palette_rgb]

    lut = []
    for i in range(32768):
        r, g, b = rgb555_to_rgb888(i)
        # ITU-R BT.601 灰度公式 (整数近似)
        gray = (30 * r + 59 * g + 11 * b) // 100
        if gray < 64:
            level = 0
        elif gray < 128:
            level = 1
        elif gray < 192:
            level = 2
        else:
            level = 3
        lut.append(palette_565[level])
    return lut


# ═══════════════════════════════════════════════════════════════
# 滤镜 2: Pixelate 像素风 (PICO-8 16色调色板映射)
# 注意: 空间像素化(block fill)仍在 FilterManager.cpp 中算法实现
#       此 LUT 仅负责颜色映射: 鲜艳度增强 → PICO-8 最近色匹配
# ═══════════════════════════════════════════════════════════════
PICO8_PALETTE_RGB = [
    (0, 0, 0),        # 0  black
    (29, 43, 83),     # 1  dark-blue
    (126, 37, 83),    # 2  dark-purple
    (0, 135, 81),     # 3  dark-green
    (171, 82, 54),    # 4  brown
    (95, 87, 79),     # 5  dark-grey
    (194, 195, 199),  # 6  light-grey
    (255, 241, 232),  # 7  white
    (255, 0, 77),     # 8  red
    (255, 163, 0),    # 9  orange
    (255, 236, 39),   # 10 yellow
    (0, 228, 54),     # 11 green
    (41, 173, 255),   # 12 blue
    (131, 118, 156),  # 13 lavender
    (255, 119, 168),  # 14 pink
    (255, 204, 170),  # 15 light-peach
]


def boost_vibrance(r, g, b):
    """饱和度拉升 2.2x (与原算法一致)"""
    luma = (r * 3 + g * 6 + b * 1) // 10
    r = clamp(luma + (r - luma) * 22 // 10)
    g = clamp(luma + (g - luma) * 22 // 10)
    b = clamp(luma + (b - luma) * 22 // 10)
    return r, g, b


def find_closest_pico8(r, g, b):
    """在 PICO-8 16色调色板中找到最接近的颜色 (加权欧氏距离)"""
    best_dist = float('inf')
    best_color = PICO8_PALETTE_RGB[0]
    for pr, pg, pb in PICO8_PALETTE_RGB:
        dr = r - pr
        dg = g - pg
        db = b - pb
        # G通道 2x 权重 (与原算法一致)
        dist = dr * dr + dg * dg * 2 + db * db
        if dist < best_dist:
            best_dist = dist
            best_color = (pr, pg, pb)
    return best_color


def generate_pixelate_lut():
    lut = []
    for i in range(32768):
        r, g, b = rgb555_to_rgb888(i)
        r, g, b = boost_vibrance(r, g, b)
        pr, pg, pb = find_closest_pico8(r, g, b)
        lut.append(rgb888_to_rgb565(pr, pg, pb))
    return lut





# ═══════════════════════════════════════════════════════════════
# 3D LUT (.cube) 解析与插值
# ═══════════════════════════════════════════════════════════════
def parse_cube_file(filepath):
    """
    解析 .cube 3D LUT 文件。
    返回 (N, lut_data) 拓扑。lut_data 是 N^3 个 (R, G, B) 浮点元组列表。
    """
    N = 0
    lut_data = []
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) == 0:
                continue
            if parts[0] == 'LUT_3D_SIZE':
                N = int(parts[1])
            elif parts[0] in ('TITLE', 'DOMAIN_MIN', 'DOMAIN_MAX'):
                continue
            else:
                try:
                    if len(parts) >= 3:
                        r, g, b = float(parts[0]), float(parts[1]), float(parts[2])
                        lut_data.append((r, g, b))
                except ValueError:
                    pass
    return N, lut_data


def interpolate_cube_to_rgb565(N, lut_data):
    """
    对 3D LUT 进行三线性插值，映射到 32768 种 RGB555 颜色 (输出为 RGB565 uint16)。
    """
    if len(lut_data) != N * N * N:
        raise ValueError(f"LUT 数据长度 {len(lut_data)} 与声明尺寸 {N}^3 = {N*N*N} 不匹配！")

    def get_val(x, y, z):
        # .cube 排序通常为：R 最快，G 次之，B 最慢
        idx = z * N * N + y * N + x
        return lut_data[idx]

    out_lut = []
    for c in range(32768):
        # 1. 拆分 RGB555 (15-bit index)
        r_5 = (c >> 10) & 0x1F
        g_5 = (c >> 5) & 0x1F
        b_5 = c & 0x1F

        # 2. 归一化到 0.0 - 1.0 坐标系
        r_f = r_5 / 31.0
        g_f = g_5 / 31.0
        b_f = b_5 / 31.0

        # 3. 映射到 3D LUT 的索引空间范围 [0, N-1]
        x_val = r_f * (N - 1)
        y_val = g_f * (N - 1)
        z_val = b_f * (N - 1)

        x0 = int(x_val)
        x1 = min(x0 + 1, N - 1)
        y0 = int(y_val)
        y1 = min(y0 + 1, N - 1)
        z0 = int(z_val)
        z1 = min(z0 + 1, N - 1)

        dx = x_val - x0
        dy = y_val - y0
        dz = z_val - z0

        # 4. 获取 3D 网格 of 8 个顶点颜色值
        c000 = get_val(x0, y0, z0)
        c100 = get_val(x1, y0, z0)
        c010 = get_val(x0, y1, z0)
        c110 = get_val(x1, y1, z0)
        c001 = get_val(x0, y0, z1)
        c101 = get_val(x1, y0, z1)
        c011 = get_val(x0, y1, z1)
        c111 = get_val(x1, y1, z1)

        # 5. 三线性插值
        channels = []
        for i in range(3): # R, G, B
            c00 = c000[i] * (1.0 - dx) + c100[i] * dx
            c01 = c001[i] * (1.0 - dx) + c101[i] * dx
            c10 = c010[i] * (1.0 - dx) + c110[i] * dx
            c11 = c011[i] * (1.0 - dx) + c111[i] * dx
            
            c0 = c00 * (1.0 - dy) + c10 * dy
            c1 = c01 * (1.0 - dy) + c11 * dy
            
            val = c0 * (1.0 - dz) + c1 * dz
            channels.append(val)

        # 6. 转为 8-bit 整数并限制在 0-255，再生成 RGB565
        r_8 = max(0, min(255, int(channels[0] * 255.0 + 0.5)))
        g_8 = max(0, min(255, int(channels[1] * 255.0 + 0.5)))
        b_8 = max(0, min(255, int(channels[2] * 255.0 + 0.5)))

        out_lut.append(rgb888_to_rgb565(r_8, g_8, b_8))

    return out_lut


def sanitize_name(name):
    """过滤特殊字符，只保留字母数字下划线，用于 C 变量/文件名"""
    sanitized = re.sub(r'[^a-zA-Z0-9]', '_', name.lower())
    sanitized = re.sub(r'_+', '_', sanitized)
    return sanitized.strip('_')


# ═══════════════════════════════════════════════════════════════
# 输出函数
# ═══════════════════════════════════════════════════════════════
def write_lut_header(filename, array_name, lut, description):
    filepath = os.path.join(OUTPUT_DIR, filename)
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(f"/**\n")
        f.write(f" * {filename}\n")
        f.write(f" * {description}\n")
        f.write(f" *\n")
        f.write(f" * Auto-generated by tools/generate_luts.py — DO NOT EDIT\n")
        f.write(f" * 32768 entries × 2 bytes = 64 KB, stored in Flash (.rodata)\n")
        f.write(f" */\n")
        f.write(f"#pragma once\n")
        f.write(f"#include <stdint.h>\n\n")
        f.write(f"static const uint16_t {array_name}[32768] = {{\n")
        for row_start in range(0, 32768, 16):
            row_end = min(row_start + 16, 32768)
            values = ', '.join(f'0x{lut[j]:04X}' for j in range(row_start, row_end))
            f.write(f"    {values},\n")
        f.write(f"}};\n")
    size_kb = os.path.getsize(filepath) / 1024
    print(f"  [OK] {filename} ({size_kb:.0f} KB)")


def main():
    if sys.stdout.encoding.lower() != 'utf-8':
        import io
        sys.stdout = io.TextIOWrapper(sys.stdout.detach(), encoding='utf-8')
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print("Generating LUT header files...")
    print()

    # 1. 编译内置滤镜
    print("[1] Generating Built-in Filters...")
    lut_gb = generate_gameboy_lut()
    write_lut_header('lut_gameboy.h', 'LUT_GAMEBOY', lut_gb,
                     'GameBoy 经典绿 4色调色板 LUT')

    lut_px = generate_pixelate_lut()
    write_lut_header('lut_pixelate.h', 'LUT_PIXELATE', lut_px,
                     'Pixelate 像素风 PICO-8 调色板映射 LUT (空间像素化在 FilterManager.cpp 中实现)')



    # 2. 扫描自定义 .cube 文件
    print()
    print("[2] Scanning tools/luts/ for custom .cube files...")
    custom_luts = []
    
    if os.path.exists(LUTS_DIR):
        files = sorted(os.listdir(LUTS_DIR))
        cube_files = [f for f in files if f.lower().endswith('.cube')]
        
        if cube_files:
            print(f"Found {len(cube_files)} custom .cube file(s). Processing...")
            for idx, filename in enumerate(cube_files, 1):
                filepath = os.path.join(LUTS_DIR, filename)
                base_name, _ = os.path.splitext(filename)
                
                # 清洗变量名和头文件名
                safe_base = sanitize_name(base_name)
                var_name = "LUT_CUSTOM_" + safe_base.upper()
                header_name = "lut_custom_" + safe_base.lower() + ".h"
                
                print(f"  [{idx}/{len(cube_files)}] Parsing '{filename}'...")
                try:
                    N, lut_data = parse_cube_file(filepath)
                    if N == 0 or not lut_data:
                        print(f"    [Warning] Failed to find LUT_3D_SIZE or data in '{filename}', skipping.")
                        continue
                    
                    print(f"    -> 3D LUT Size: {N}x{N}x{N} ({len(lut_data)} data points). Interpolating to RGB565...")
                    rgb565_lut = interpolate_cube_to_rgb565(N, lut_data)
                    
                    # 写入头文件
                    write_lut_header(header_name, var_name, rgb565_lut, f"Custom LUT from '{filename}'")
                    custom_luts.append((base_name, var_name, header_name))
                except Exception as e:
                    print(f"    [Error] Failed to process '{filename}': {e}")
        else:
            print("No custom .cube files found in tools/luts/.")
    else:
        print("Directory tools/luts/ does not exist. Created it.")
        os.makedirs(LUTS_DIR, exist_ok=True)

    # 3. 生成注册文件 lut_custom_registry.h
    print()
    print("[3] Generating lut_custom_registry.h...")
    registry_path = os.path.join(OUTPUT_DIR, 'lut_custom_registry.h')
    with open(registry_path, 'w', encoding='utf-8') as f:
        f.write("// Auto-generated by tools/generate_luts.py — DO NOT EDIT\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n")
        
        # 写入所有自定义头文件的 include
        for _, _, header_name in custom_luts:
            f.write(f'#include "{header_name}"\n')
        f.write("\n")
        
        # 写入注册结构体
        f.write("struct CustomLutEntry {\n")
        f.write("    const char* name;\n")
        f.write("    const uint16_t* lut;\n")
        f.write("};\n\n")
        
        f.write("static const CustomLutEntry CUSTOM_LUTS[] = {\n")
        for disp_name, var_name, _ in custom_luts:
            # 去掉显示名称中的特殊字符，避免双引号引起编译问题
            disp_name_clean = disp_name.replace('"', '\\"')
            # 缩短显示名称 (限定 30 字符，避免 LCD 溢出)
            if len(disp_name_clean) > 30:
                disp_name_clean = disp_name_clean[:27] + "..."
            f.write(f'    {{"{disp_name_clean}", {var_name}}},\n')
        f.write("};\n\n")
        
        f.write(f"static const int CUSTOM_LUT_COUNT = {len(custom_luts)};\n")
        
    print(f"  [OK] lut_custom_registry.h ({len(custom_luts)} registered custom LUTs)")
    print()
    print("Done! All LUT and registry files generated successfully.")


if __name__ == '__main__':
    main()

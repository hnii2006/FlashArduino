#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import os


def parse_intel_hex(hex_path):
    """
    Intel HEX を読み込み、アドレス→バイト値の dict を返す
    0x00: data record
    0x01: EOF
    0x04: extended linear address
    """
    memory = {}
    upper_addr = 0  # extended linear address (<<16)

    with open(hex_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if not line.startswith(":"):
                continue

            # :llaaaatt[dd...]cc
            try:
                length = int(line[1:3], 16)
                addr = int(line[3:7], 16)
                rectype = int(line[7:9], 16)
                data_str = line[9:9 + length * 2]
                # checksum は一旦無視（必要なら検証を足せる）
                # checksum = int(line[9 + length * 2: 9 + length * 2 + 2], 16)
            except ValueError:
                raise ValueError(f"HEX 行のパースに失敗しました: {line}")

            if rectype == 0x00:  # data
                base = (upper_addr << 16) + addr
                for i in range(length):
                    byte_val = int(data_str[i * 2:i * 2 + 2], 16)
                    memory[base + i] = byte_val

            elif rectype == 0x01:  # EOF
                break

            elif rectype == 0x04:  # extended linear address
                # 上位 16bit アドレス
                upper_addr = int(data_str[0:4], 16)

            else:
                # 他のレコードタイプは今回はスキップ
                continue

    if not memory:
        raise RuntimeError("HEX から有効なデータが読み取れませんでした。")

    # 0 から max アドレスまでのフラットなバイト列に変換
    min_addr = min(memory.keys())
    # AVR 向けなので基本は 0 からで良いが、念のため min_addr も取っておく
    base_addr = 0  # アプリケーションを 0 起点で扱うために固定
    max_addr = max(memory.keys())
    size = max_addr - base_addr + 1

    data = []
    for a in range(base_addr, base_addr + size):
        data.append(memory.get(a, 0xFF))  # 抜けアドレスは 0xFF 埋め

    return data


def write_header(h_path, data, var_name="firmware_bin", size_name="FIRMWARE_SIZE"):
    """
    C 用の .h ファイルを書き出す
    - FIRMWARE_SIZE: バイト数
    - firmware_bin[] PROGMEM: バイト配列
    """
    size = len(data)

    guard_name = os.path.basename(h_path).upper()
    guard_name = guard_name.replace(".", "_").replace("-", "_")

    with open(h_path, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from Intel HEX by hex2h.py\n")
        f.write("#pragma once\n\n")
        f.write("#include <avr/pgmspace.h>\n")
        f.write("#include <stdint.h>\n\n")

        f.write(f"const uint32_t {size_name} = {size}UL;\n\n")
        f.write(f"const uint8_t {var_name}[] PROGMEM = {{\n")

        # 16 バイト／行で整形
        bytes_per_line = 16
        for i, b in enumerate(data):
            if i % bytes_per_line == 0:
                f.write("    ")
            f.write(f"0x{b:02X}")
            if i != size - 1:
                f.write(", ")
            if (i % bytes_per_line) == (bytes_per_line - 1) or i == size - 1:
                f.write("\n")

        f.write("};\n")


def main():
    parser = argparse.ArgumentParser(
        description="Intel HEX を AVR 用の PROGMEM .h に変換するスクリプト"
    )
    parser.add_argument("input_hex", help="入力 Intel HEX ファイル (.hex)")
    parser.add_argument("output_h", help="出力ヘッダファイル (.h)")
    parser.add_argument(
        "--var",
        dest="var_name",
        default="firmware_bin",
        help="C 配列の変数名 (デフォルト: firmware_bin)",
    )
    parser.add_argument(
        "--size-name",
        dest="size_name",
        default="FIRMWARE_SIZE",
        help="サイズ定数の名前 (デフォルト: FIRMWARE_SIZE)",
    )

    args = parser.parse_args()

    data = parse_intel_hex(args.input_hex)
    write_header(args.output_h, data, var_name=args.var_name, size_name=args.size_name)


if __name__ == "__main__":
    main()

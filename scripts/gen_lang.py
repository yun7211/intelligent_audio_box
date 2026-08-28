#!/usr/bin/env python3
import argparse
import json
import os

HEADER_TEMPLATE = """// Auto-generated language config
// Language: {lang_code}
#pragma once

#include <string_view>

#ifndef {lang_code_for_font}
    #define {lang_code_for_font}  // 預設語言
#endif

namespace Lang {{
    // 语言元数据
    constexpr const char* CODE = "{lang_code}";

    // 字符串资源
    namespace Strings {{
{strings}
    }}

    // 音效资源
    namespace Sounds {{
{sounds}
    }}
}}
"""

def get_sound_files(directory):
    """获取目录中的音效文件列表"""
    if not os.path.exists(directory):
        return []
    return [f for f in os.listdir(directory) if f.endswith('.ogg')]

def generate_header(lang_code, output_path):
    # 从输出路径推导项目结构
    # output_path 通常是 main/assets/lang_config.h
    main_dir = os.path.dirname(output_path)  # main/assets
    if os.path.basename(main_dir) == 'assets':
        main_dir = os.path.dirname(main_dir)  # main
    project_dir = os.path.dirname(main_dir)  # 项目根目录
    assets_dir = os.path.join(main_dir, 'assets')

    # 构建语言JSON文件路径
    input_path = os.path.join(assets_dir, 'locales', lang_code, 'language.json')

    print(f"Processing language: {lang_code}")
    print(f"Input file path: {input_path}")
    print(f"Output file path: {output_path}")

    if not os.path.exists(input_path):
        raise FileNotFoundError(f"Language file not found: {input_path}")

    with open(input_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    # 验证数据结构
    if 'language' not in data or 'strings' not in data:
        raise ValueError("Invalid JSON structure")

    strings_data = data['strings']
    print(f"Language {lang_code} strings: {len(strings_data)}")

    # 生成字符串常量
    strings = []
    for key, value in strings_data.items():
        value = value.replace('"', '\\"')
        strings.append(f'        constexpr const char* {key.upper()} = "{value}";')

    # 收集音效文件
    current_lang_dir = os.path.join(assets_dir, 'locales', lang_code)
    common_dir = os.path.join(assets_dir, 'common')
    current_sounds = get_sound_files(current_lang_dir)
    common_sounds = get_sound_files(common_dir)

    print(f"Language {lang_code} sounds: {len(current_sounds)}")
    print(f"Common sounds: {len(common_sounds)}")

    # 生成语言特定音效常量
    sounds = []
    for file in sorted(current_sounds):
        base_name = os.path.splitext(file)[0]
        sounds.append(f'''
        extern const char ogg_{base_name}_start[] asm("_binary_{base_name}_ogg_start");
        extern const char ogg_{base_name}_end[] asm("_binary_{base_name}_ogg_end");
        static const std::string_view OGG_{base_name.upper()} {{
        static_cast<const char*>(ogg_{base_name}_start),
        static_cast<size_t>(ogg_{base_name}_end - ogg_{base_name}_start)
        }};''')

    # 生成公共音效常量
    for file in sorted(common_sounds):
        base_name = os.path.splitext(file)[0]
        sounds.append(f'''
        extern const char ogg_{base_name}_start[] asm("_binary_{base_name}_ogg_start");
        extern const char ogg_{base_name}_end[] asm("_binary_{base_name}_ogg_end");
        static const std::string_view OGG_{base_name.upper()} {{
        static_cast<const char*>(ogg_{base_name}_start),
        static_cast<size_t>(ogg_{base_name}_end - ogg_{base_name}_start)
        }};''')

    # 填充模板
    content = HEADER_TEMPLATE.format(
        lang_code=lang_code,
        lang_code_for_font=lang_code.replace('-', '_').lower(),
        strings="\n".join(sorted(strings)),
        sounds="\n".join(sorted(sounds))
    )

    # 写入文件
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(content)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate language configuration header file")
    parser.add_argument("--language", required=True, help="Language code (e.g: zh-CN)")
    parser.add_argument("--output", required=True, help="Output header file path")
    args = parser.parse_args()

    try:
        generate_header(args.language, args.output)
        print(f"Successfully generated language config file: {args.output}")
    except Exception as e:
        print(f"Error: {e}")
        exit(1)

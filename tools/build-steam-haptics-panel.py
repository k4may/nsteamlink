#!/usr/bin/env python3
"""Build a portable Windows x64 Steam haptics source panel (system DLLs only)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
README = """NSteamLink · Steam 实时震动控制面板

开始使用
1. 解压到一个固定文件夹。
2. Steam 左下角“添加游戏”→“添加非 Steam 游戏”→“浏览”，选择 steam-haptics-panel.exe。
3. Switch 使用正常 NSteamLink 客户端连接电脑。在 Steam 库里启动刚添加的程序。
   无需购买或启动其他游戏，不要从资源管理器直接双击来代替 Steam 启动。
4. 选中带“远程”标记的手柄，点击“播放”，拖动低频/高频力度；数字框可输入精确值。
   两路 0–65535 是普通游戏的真实输入范围。弱震10%/中等50%/满量程是快捷设置。
5. 可选持续、单次脉冲、重复脉冲；开/关时间和播放上限都能修改。
   切到其他窗口、停止按钮、空格/Esc、关闭程序会停止；切回后需重新点击播放。

发送方式
默认“普通游戏路径 (XInput)”使用游戏通用的震动请求，交给 Steam 及 Remote Play 传输。
“Steam Input 双路震动”用于对比 Steam 原生接口，两者都使用同一组力度/节奏控件。
程序继承 Steam 启动的真实应用上下文，不借用游戏AppID，不带steam_appid.txt。
如果没有游戏手柄，可切换到“Steam Input 双路震动”。如果两种方式都为空，检查
该非Steam程序的控制器设置：启用Steam输入，布局选择“手柄”，然后从Steam重新启动。

整条链路的记录
Switch 地址默认 10.10.10.111，可直接修改后点“连接自动采集”。
正常客户端的诊断功能需要启用；面板自动读取UDP诊断，不向Switch注入测试命令。
界面显示最近收到的是普通双路/Nintendo原生/Sony效果，以及Switch实际提交的幅度和频率。
本工具的两种发送方式都是普通双路源；原生效果标签是对实际收到数据的识别，不是伪造高级源。
普通路径的物理频率由客户端映射决定；力度数字不是机械加速度或主观强度百分比。

参数自动保存，下次打开继续使用。点击“打开记录文件夹”可找到当前session-*.jsonl，
其中同时保存主机请求及Switch回传，便于逐次核对。设置和记录保存在：
%LOCALAPPDATA%\\NSteamLink\\HapticsPanel

无需管理员权限、虚拟手柄驱动或安装包。只加载Windows系统组件和本机Steam自带DLL。
没有修改Steam配置、控制器能力或Switch生产代码。
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "steam-haptics-panel.exe"
    command = ["x86_64-w64-mingw32-gcc", "-std=c11", "-O2", "-s", "-Wall", "-Wextra", "-Werror",
        "-municode", "-mwindows", str(ROOT / "tools/steam-haptics-panel-win.c"), "-o", str(exe),
        "-lcomctl32", "-lshell32", "-ladvapi32", "-lws2_32", "-lgdi32"]
    subprocess.run(command, check=True)
    (out / "使用说明.txt").write_text(README, encoding="utf-8-sig")
    imports = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", str(exe)], text=True)
    dlls = [line.split("DLL Name:", 1)[1].strip() for line in imports.splitlines() if "DLL Name:" in line]
    assert set(d.lower() for d in dlls) <= {"advapi32.dll", "comctl32.dll", "gdi32.dll", "kernel32.dll",
        "msvcrt.dll", "shell32.dll", "user32.dll", "ws2_32.dll"}, dlls
    manifest = dict(schema=1, system_imports=dlls, compiler_command=command,
        source_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (exe, out / "使用说明.txt")},
        sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
                 (ROOT / "tools/steam-haptics-panel-win.c", ROOT / "tools/haptics-panel-state.h")},
        actual_steam_verified=False, physical_fidelity_verified=False)
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    archive = out.parent / "NSteamLink-Steam-Haptics-Panel-Windows-x64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for name in (exe.name, "使用说明.txt", "manifest.json"):
            z.write(out / name, name)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
    print(json.dumps(dict(archive=str(archive), sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                         executable_sha256=manifest["files"][exe.name]), indent=2))


if __name__ == "__main__":
    main()

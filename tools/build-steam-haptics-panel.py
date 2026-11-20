#!/usr/bin/env python3
"""Build a portable Windows x64 Steam haptics source panel (system DLLs only)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
README = """NSteamLink · Steam 力度 / Switch 频率实时控制面板

桌面直连（这次验证用）
保持Steam运行、Switch正常客户端连接Steam。解压后双击Start-Desktop-Panel.cmd，
无需添加非Steam游戏。默认选择“Steam 桌面直连”，找到Switch Pro后调A/B并点播放。
频率和输出位置继续在同一面板调节，Switch无需更新。
点停止归零，关闭面板后本文件夹自动生成desktop-results-日期.zip，包含本次日志及Steam错误。
桌面直连使用Steam自身的内部IPC，请求仍由Steam经Remote Play发给Switch。
它是诊断路径，不宣称代表普通游戏公开Steam Input API已成功。
当前适配的是此前提供的Steam DLL版本；版本不匹配会记录实际SHA，不猜接口。


原有XInput游戏路径
关闭旧面板，用本包同名EXE替换原文件夹中的EXE，继续用原来的Steam快捷方式。
首次使用：Steam左下“添加游戏”→“添加非Steam游戏”选择EXE。
Switch使用正常客户端连接Steam，再从Steam库启动面板。

一次只改频率的对照
1. 选“持续”；A=0、B=65535；“输出位置”选“仅右侧播放A+B”。
2. 幅度上限固定0.5571934，点击“B → 320Hz”，感受效果。
3. 保持其他值不变，点击“B → 640Hz”，对比这两个频率。
4. 如果要找640Hz的弱震阈值，保持640Hz再单独调B；如需超过现有映射峰值，
   保持B=65535、640Hz不变，再单独把幅度上限改成0.75或1.0并点“应用频率”。
两只手柄的低/高频带均可输出，输出位置也可选择双侧、仅左、仅右。

两组控制分别做什么
A/B是游戏原有的两路力度，始终经XInput或Steam Input → Steam → Remote Play发送。
输出Hz、幅度上限、输出位置是Switch普通震动渲染的实时配置，通过诊断UDP发送。
频率配置不是Steam原生反馈字段，不把脉冲开关节奏假称为640Hz载波。
应用配置先停止旧反馈，核对Switch回执，然后恢复原来的A/B力度。
窗口会显示已生效的Hz和两只震动器实际提交的幅频。
Nintendo原生效果与专用源按原参数解释，不套用普通映射的频率/增益。
支持实时配置的正常客户端需更新一次，此后改变数值不需重编或重新推送。

Steam Input枚举
桌面启动默认走桌面直连；从Steam库启动则保留XInput普通游戏路径和公开API对照。
公开列表为空时，会继续用Steam公开接口查询XInput槽对应的真实Steam句柄。
初始化阶段、公开列表数、关联句柄数及请求均自动写入记录。
启动连接失败后，切换Steam Input或点击播放会重试；不再永久沿用首次失败状态。
不借用其他游戏AppID，不安装驱动，不改Steam设备能力或布局。
公开API对照不走内部IPC；桌面直连明确使用版本绑定的Steam内部IPC。

使用与记录
力度支持滑块、精确0–65535值及快捷设置；支持持续、单次、重复脉冲。
开关时间可改；播放上限0表示不限。切换窗口可以继续播放。
停止按钮、空格/Esc、断线、换目标或关闭会归零，重连不自动恢复。
Switch地址默认10.10.10.111，可修改后连接采集。面板自动保存主机请求与回传；
点击“打开记录文件夹”可找到session-*.jsonl。
设置与日志位于 %LOCALAPPDATA%\\NSteamLink\\HapticsPanel。
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
        "-lcomctl32", "-lshell32", "-ladvapi32", "-lws2_32", "-lgdi32", "-lbcrypt"]
    subprocess.run(command, check=True)
    (out / "使用说明.txt").write_text(README, encoding="utf-8-sig")
    imports = subprocess.check_output(["x86_64-w64-mingw32-objdump", "-p", str(exe)], text=True)
    dlls = [line.split("DLL Name:", 1)[1].strip() for line in imports.splitlines() if "DLL Name:" in line]
    assert set(d.lower() for d in dlls) <= {"advapi32.dll", "comctl32.dll", "gdi32.dll", "kernel32.dll",
        "msvcrt.dll", "shell32.dll", "user32.dll", "ws2_32.dll", "bcrypt.dll"}, dlls
    launcher=out / "Start-Desktop-Panel.cmd"
    launcher.write_bytes(b'@echo off\r\ncd /d "%~dp0"\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-desktop-panel.ps1"\r\n')
    collector=out / "run-desktop-panel.ps1"
    collector.write_text((ROOT / "tools/run-desktop-panel.ps1").read_text(),encoding="utf-8-sig")
    manifest = dict(schema=1, system_imports=dlls, compiler_command=command,
        source_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        files={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (exe, out / "使用说明.txt", launcher, collector)},
        sources={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
                 (ROOT / "tools/steam-haptics-panel-win.c", ROOT / "tools/haptics-panel-state.h", ROOT / "tools/run-desktop-panel.ps1")},
        actual_steam_verified=False, physical_fidelity_verified=False)
    (out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    archive = out.parent / "NSteamLink-Steam-Haptics-Panel-Windows-x64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for name in (exe.name, "使用说明.txt", "manifest.json", launcher.name, collector.name):
            z.write(out / name, name)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
    print(json.dumps(dict(archive=str(archive), sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                         executable_sha256=manifest["files"][exe.name]), indent=2))


if __name__ == "__main__":
    main()

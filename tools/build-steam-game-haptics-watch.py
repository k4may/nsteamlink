#!/usr/bin/env python3
"""Build the portable passive game observer, without rebuilding Switch."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
README = """NSteamLink 游戏震动只读记录器

1. 关闭之前的震动测试面板，保持 Switch 正常串流连接 Steam，进入游戏可操作的场景。
2. 在主机电脑解压本包，双击 Start-Game-Capture.cmd；不用加到 Steam 库。
3. 切回游戏玩三分钟：先停留几秒，再战斗/受击/交互，重复同一事件数次。
4. 结束后自动生成并选中 game-results-*.zip，发回这个 ZIP。
每个游戏单独运行一次记录器。已有《剑星》即可先测，不必等下载。

这是采集器，不会主动震动，不安装东西，不改 Steam 设置、设备身份或游戏文件。
本包不要求更新 Switch。Steam 主机状态适配此前提供的 DLL 版本；不同版本只记录未知。
主机原生 producer 时间戳变化包括停止，不能当成已收到的非零波形。
Switch 诊断接口可访问时，原包类型和系统提交记录可验证接收/输出链路；
不可访问时仍保存主机记录和 Steam 日志，不把采集失败说成游戏不支持。
不承诺《剑星》通过当前 Switch 身份就有 DualSense 高级触觉；需看实际记录。
包里“采集说明.txt”和原始 host.jsonl / switch.jsonl 区分证据强度。

如需改时长/IP，在 PowerShell 执行：
.\\run-game-haptics-capture.ps1 -Seconds 180 -Address 10.10.10.111
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'steam-game-haptics-watch.exe'
    command = ['x86_64-w64-mingw32-gcc', '-std=c11', '-O2', '-s', '-Wall', '-Wextra', '-Werror',
               str(ROOT / 'tools/steam-game-haptics-watch-win.c'), '-o', str(exe), '-lbcrypt']
    subprocess.run(command, check=True)
    imports = subprocess.check_output(['x86_64-w64-mingw32-objdump', '-p', str(exe)], text=True)
    dlls = [line.split('DLL Name:', 1)[1].strip() for line in imports.splitlines() if 'DLL Name:' in line]
    assert {d.lower() for d in dlls} <= {'kernel32.dll', 'msvcrt.dll', 'bcrypt.dll'}, dlls
    (out / '使用说明.txt').write_text(README, encoding='utf-8-sig')
    (out / 'Start-Game-Capture.cmd').write_bytes(
        b'@echo off\r\ncd /d "%~dp0"\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-game-haptics-capture.ps1"\r\nif errorlevel 1 pause\r\n')
    (out / 'run-game-haptics-capture.ps1').write_text(
        (ROOT / 'tools/run-game-haptics-capture.ps1').read_text(), encoding='utf-8-sig')
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = dict(schema=1, read_only=True, system_imports=dlls,
                    source_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    files={p.name: sha(p) for p in out.iterdir() if p.is_file() and p.name != 'manifest.json'},
                    sources={str(p.relative_to(ROOT)): sha(p) for p in [
                        ROOT / 'tools/steam-game-haptics-watch-win.c', ROOT / 'tools/steam-haptics-host-state.h',
                        ROOT / 'tools/steam-haptics-host-inspect-win.h', ROOT / 'tools/run-game-haptics-capture.ps1']},
                    actual_game_capture_verified=False, physical_fidelity_verified=False)
    (out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    archive = out.parent / 'NSteamLink-Game-Haptics-Capture-Windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in [*manifest['files'], 'manifest.json']:
            z.write(out / name, name)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
    print(json.dumps(dict(archive=str(archive), sha256=sha(archive)), indent=2))


if __name__ == '__main__':
    main()

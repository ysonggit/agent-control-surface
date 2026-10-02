#!/usr/bin/env python3
"""Walk the real device through every screen, printing what it should show. Needs acsd running."""
import json, os, socket, subprocess, sys, time

HOOK = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "acs_hook.py")]
CWD = "/Users/yang/Github/agent-control-surface"


def send(event, wait=False):
    event = {"session_id": event.pop("sid", "demo0001"), "cwd": event.pop("cwd", CWD), **event}
    p = subprocess.Popen(HOOK, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    p.stdin.write(json.dumps(event)); p.stdin.close()
    if wait:
        return p
    p.wait()


def step(title, expect, seconds=6):
    print(f"\n▶ {title}\n  屏幕应显示: {expect}")
    time.sleep(seconds)


def ask(command, expect):
    p = send({"hook_event_name": "PermissionRequest", "tool_name": "Bash", "tool_input": {"command": command}}, wait=True)
    print(f"\n▶ 权限请求: {command!r}\n  屏幕应显示: {expect}\n  请在设备上操作 (最多等 60 秒)…")
    try:
        out = p.stdout.read().strip()
        p.wait(timeout=60)
    except subprocess.TimeoutExpired:
        p.kill(); out = ""
    print("  主机收到:", json.loads(out)["hookSpecificOutput"]["decision"] if out else "无决定 (超时或设备未连接)")


send({"hook_event_name": "SessionStart"})
step("1 空闲", "橙色 Claude 图标+CLAUDE, 右上 1/1, 项目名 agent-control-surface, 灰色大条 IDLE, 下方 5h/7d 用量条, 右下 USB 或电量")
send({"hook_event_name": "UserPromptSubmit"})
send({"hook_event_name": "PreToolUse", "tool_name": "Edit", "tool_input": {"file_path": CWD + "/firmware/acs/acs.ino"}})
step("2 工作中", "蓝色大条 WORKING, 下方白色大字 Edit, 灰色小字 firmware/acs/acs.ino")
send({"sid": "demo0002", "cwd": "/tmp/other-repo", "hook_event_name": "Stop"})
step("3 第二个会话", "右上变 1/2, 底部多一行: 绿点 other-repo。短按 USR2 可切换到它 (绿色 DONE)", 8)
ask("ls -la firmware", "PERMISSION + Bash + 大字命令, 倒计时, 绿色 ALLOW (hold here or USR1), 红色 DENY (tap / USR2)。请按住 USR1 约1秒")
ask("git push origin main", "右上红色 HIGH, 绿色按钮变灰 'APPROVE AT KEYBOARD'。按住也无效; 请短按 USR2")
ask("touch demo.txt", "普通请求。请长按 USR2 约1.5秒 (拒绝并停止, 主机应收到 interrupt: True)")
ask("echo " + "x" * 260, "小字命令 + 红色 '...+N chars', 按钮为灰色 APPROVE AT KEYBOARD。按住 USR1 应提示 Approve at keyboard, 然后点屏幕 DENY")
send({"hook_event_name": "Stop"})
send({"sid": "demo0002", "hook_event_name": "SessionEnd"})
step("4 完成", "绿色大条 DONE, 右上 1/1", 4)
print("\n演示结束。关掉 acsd 12 秒后, 屏幕应回到 'Hello Agent / waiting for acsd'。")

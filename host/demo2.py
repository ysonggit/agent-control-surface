#!/usr/bin/env python3
"""Try the v0.4 features on the real device: your-turn alert, request queue, allow-for-session. Needs acsd running."""
import json, os, subprocess, sys, threading, time

HOOK = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "acs_hook.py")]


def send(sid, cwd, event, agent="claude", wait=False):
    p = subprocess.Popen(HOOK + ["--agent", agent], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    p.stdin.write(json.dumps({"session_id": sid, "cwd": cwd, **event}))
    p.stdin.close()
    if not wait:
        p.wait()
    return p


def result(p, timeout=90):
    try:
        out = p.stdout.read().strip()
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out = ""
    return json.loads(out)["hookSpecificOutput"]["decision"] if out else "无决定"


def perm(sid, cwd, cmd, agent="claude"):
    return send(sid, cwd, {"hook_event_name": "PermissionRequest", "tool_name": "Bash", "tool_input": {"command": cmd}},
                agent, wait=True)


print("▶ 功能3 轮到你了")
send("turn0001", "/Users/yang/Github/web-app", {"hook_event_name": "UserPromptSubmit"})
send("turn0001", "/Users/yang/Github/web-app", {"hook_event_name": "Stop"})
print("  屏幕应显示: Claude 圆环底色绿色闪烁, 底部 'YOUR TURN' 闪烁, 屏幕不会自动熄灭。")
input("  点一下屏幕 (或按任意键) 确认后应停止闪烁。看完按回车继续…")

print("\n▶ 功能1 请求队列: 同时发出 3 个请求 (来自两个会话)")
reqs = [("api-server", "aaaa1111", "ls -la src"), ("web-app", "bbbb2222", "ls -la public"), ("web-app", "bbbb2222", "ls -la tests")]
procs = []
for label, sid, cmd in reqs:
    procs.append((cmd, perm(sid, f"/Users/yang/Github/{label}", cmd)))
    time.sleep(1.2)
print("  屏幕应显示: 第一个请求, 左下 '#aaaa  Ns ago', 右下 '< 1/3 >'。")
print("  左右滑动命令区域, 或点 '<' '>' 切换; 标题下方项目名和 # 编号会变。")
print("  请依次处理: 在 'ls -la tests' 上短按 USR2 拒绝, 再在 'ls -la src' 上按住 USR1 批准, 最后点 DENY 拒绝剩下那个。")
threads = []
res = {}
for cmd, p in procs:
    t = threading.Thread(target=lambda c=cmd, q=p: res.__setitem__(c, result(q)))
    t.start()
    threads.append(t)
for t in threads:
    t.join()
for cmd, _ in procs:
    print(f"  {cmd!r:18} -> {res[cmd]}")

print("\n▶ 功能2 本会话总是允许")
p = perm("sess0001", "/Users/yang/Github/api-server", "npm test")
print("  屏幕应显示: 两个按钮, 左 'Allow / once / USR1' (绿), 右 'Always / this session' (青)。")
print("  请按住右边青色按钮约 1.5 秒。")
print("  主机收到:", result(p))
p = perm("sess0001", "/Users/yang/Github/api-server", "git push origin main")
print("  高风险命令 'git push' 应只显示灰色 APPROVE AT KEYBOARD, 没有 Always 按钮。请点 DENY。")
print("  主机收到:", result(p))
print("\n演示结束。")

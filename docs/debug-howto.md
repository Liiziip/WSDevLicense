# 抓一次 x64.exe 的真实 RPC 调用

目标：拿到 `NdrClientCall3` 在 **proc2（安装/删除）** 那一次命中的完整参数表，用它把
x86 侧的 `NdrClientCall2` 参数形状对齐。

x64.exe 在 Win8 x64 虚拟机上已验证可用（install / check / remove 都成功），所以它发出的
调用就是"正确答案"。

---

## 0. 准备

1. 在**虚拟机里**准备 x64dbg（绿色版，解压即用）
   - 有网：直接下 `x64dbg` 的 snapshot zip
   - 没网：在宿主机下载，通过共享文件夹/拖拽拷进 VM
2. **用管理员身份**启动 `x64dbg.exe`（因为 x64.exe 需要提权，非管理员会提前失败）
3. 确认 `x64.exe` 和许可证 xml 的位置，例如：
   - `D:\ModernApps\x64.exe`
   - `D:\devlic\2011.7.1.xml`

---

## 1. 下断点

x64dbg 菜单：`File → Open` → 选 `D:\ModernApps\x64.exe`

在窗口底部**命令框**（Command）依次输入：

```
bp rpcrt4!NdrClientCall3
```

回车。断点列表里应出现 `rpcrt4.NdrClientCall3`。

> 如果提示找不到符号：改成 `bp NdrClientCall3`（rpcrt4 导出这个函数名）。
> 仍失败就用 `bp rpcrt4.NdrClientCall3`。

---

## 2. 填参数并运行

菜单 `File → Change Command Line`（或在"Command line"框里）填：

```
-install D:\devlic\2011.7.1.xml
```

然后按 **F9** 运行。

---

## 3. 每个断点命中要看什么

每命中一次断点，程序会停在 `NdrClientCall3` 的第一条指令。**先看 RDX**：

| RDX 的值 | 含义 |
|---|---|
| 0 | proc0（"ping"/初始化），可以跳过 |
| 1 | proc1（关闭），可以跳过 |
| **2** | **proc2 —— 这就是我们要的** |

跳过时直接 **F9** 继续，直到 RDX = 2 停住。

### 在 RDX = 2 的这一停，记录 4 个寄存器

x64dbg 右上角寄存器面板，或命令框输入：

```
r rcx
r rdx
r r8
r r9
```

### 再记录 6 个栈参数

命令框输入：

```
dq rsp+20
```

会显示 `rsp+20` 起的 8 个 8 字节。我需要前 6 个：
`[rsp+20] [rsp+28] [rsp+30] [rsp+38] [rsp+40] [rsp+48]`

### 最后看这些"指针"指向什么

上面记下的 R9 和 6 个栈值，**多半都是指针**（MIDL 传参都是传地址）。对每一个：

- 命令框输入 `dump <那个值>`（例如 `dump 0000000000A1B2C3`）
- 或右键 → Follow in Dump

每个 dump 只要**前 32 字节**。重点看：
- 有没有 `0x00000000`（session 传的就是 0）
- 有没有 `0x0000000c` / `0x00000000`（opcode：remove 是 0xc）
- 有没有一个很小的数（比如 `0x4ac`=1196，就是许可证文件长度）
- 有没有一个指向一大段数据的指针（就是请求体缓冲区）

---

## 4. 再抓一次 remove（强烈建议）

`-remove` 的请求体是空的，两次对比就能确定哪个参数是"长度"、哪个是"缓冲区"：

1. 关掉 x64dbg 里被调试的进程（或让它跑完）
2. 重新 `File → Open`，命令框填 `-remove`
3. F9，同样在 RDX=2 停住，重复上面的记录

---

## 5. 把结果发我

两种方式都行：

- **截图**：CPU 窗口（寄存器 + 栈）+ 每个 dump 窗口
- **文本**：断点日志窗口（Log）全选复制

我需要的最小信息：

```
proc2 hit:
  rcx = ...
  rdx = 2
  r8  = ...
  r9  = ...
  [rsp+20] = ...
  [rsp+28] = ...
  [rsp+30] = ...
  [rsp+38] = ...
  [rsp+40] = ...
  [rsp+48] = ...
每个指针指向的前 32 字节
```

---

## 备选：WinDbg 命令行（更省事，如果 VM 里有 windbg）

启动时带初始命令，把一切记到日志：

```
windbg -g -c ".logopen /t /u C:\ndr.log; bp rpcrt4!NdrClientCall3 \"kb 0n8; r rcx,rdx,r8,r9; dq rsp+20 L8; g\""
```

跑完 `-install` 后，把 `C:\ndr.log` 拷出来。日志里每次命中都有寄存器和栈，找 `rdx=2`
那一段即可。

---

## 注意

- x64dbg 必须在**管理员**下运行，否则 x64.exe 会在装许可证前就失败。
- 先 `-remove` 再 `-install`，保证状态干净。
- 用 **2011.7.1** 那份 xml（2012 那份能装但查不到，说明和服务版本不匹配，不适合当基线）。
- 断点命中很多次是正常的（RPC 内部还会调别的），认准 `rdx == 2`。

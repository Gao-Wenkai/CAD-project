# Engineering CAD v1.0 — 项目文档

## 概述

一款仿 AutoCAD 的 2D CAD 桌面应用，基于 C++ 和 MFC（Microsoft Foundation Classes）构建。支持通过鼠标点击绘图区或命令行输入坐标来交互式绘制几何图形。

- **平台：** Windows (x64)
- **语言：** C++ (MFC, Win32 API)
- **构建：** Visual Studio 2019+ MSBuild
- **架构：** MFC 文档/视图 (SDI)

---

## 架构

```
CWinApp (CLargeHWApp)
  └── CSingleDocTemplate
        ├── CDocument (CLargeHWDoc)    → 数据模型（实体、撤销/重做、图层）
        ├── CFrameWnd (CMainFrame)     → 窗口框架（工具栏、命令行、状态栏）
        └── CView (CLargeHWView)       → 渲染引擎 + 交互状态机
```

---

## 文件结构

| 文件 | 说明 |
|------|------|
| `Entity.h / Entity.cpp` | 8 种几何实体（直线、圆、弧、矩形、多边形、椭圆、多段线、文字） |
| `LargeHWDoc.h / LargeHWDoc.cpp` | 文档层：实体列表、撤销/重做、图层、颜色/线型管理 |
| `LargeHWView.h / LargeHWView.cpp` | 视图层：GDI 渲染（深色背景、网格、十字光标、捕捉）、鼠标/键盘状态机 |
| `MainFrm.h / MainFrm.cpp` | 框架层：工具栏、AutoCAD 风格命令行、状态栏、菜单动态构建 |
| `LargeHW.h / LargeHW.cpp` | 应用入口 (CWinApp) |
| `LargeHW.rc` | 资源文件：对话框、菜单、图标、工具栏 |
| `Resource.h` | 资源 ID 宏定义 |

---

## 功能列表

### 1. 绘图实体

所有实体通过鼠标在深色模型空间画布上点击绘制：

| 实体 | 菜单 | 命令行 | 操作步骤 |
|------|------|--------|----------|
| 直线 | Line | `L` | 点击 P1 → 点击 P2 |
| 多段线 | Polyline | `PL` | 连续点击各点，Enter 结束 |
| 圆 | Circle | `C` | 点击圆心 → 点击半径 |
| 弧 | Arc | `A` | 点击起点、圆心、终点；Ctrl 翻转半弧 |
| 矩形 | Rectangle | `REC` | 点击第一角点 → 对角点 |
| 多边形 | Polygon | `POL` | 点击中心 → 半径（默认 6 边） |
| 椭圆 | Ellipse | `EL` | 点击中心 → 轴端点 |
| 文字 | Text | `T` | 点击位置 → 弹出对话框输入文字和高度 |

### 2. 修改操作

| 命令 | 快捷键 | 操作步骤 |
|------|--------|----------|
| 移动 | `M` | 选择对象 → 基点 → 目标点 |
| 复制 | `CO` | 选择对象 → 基点 → 目标点 |
| 旋转 | `RO` | 选择对象 → 中心点 → 角度 |
| 缩放 | `SC` | 选择对象 → 基点 → 比例因子 |
| 镜像 | `MI` | 选择对象 → 镜像线 P1 → P2 |
| 偏移 | `O` | 选择实体 → 点击偏移侧 |
| 删除 | `E` / Del | 删除已选中的实体 |

### 3. 撤销 / 重做

- **撤销** `Ctrl+Z` / `U` — 回退上一步操作（最多 50 步）
- **重做** `Ctrl+Y` / `REDO` — 恢复被撤销的操作
- 支持多实体操作的批量撤销/重做

### 4. AutoCAD 风格命令行

- **高度 60px**，Consolas 20pt 等宽字体
- **深色背景** (RGB 40,40,44) 配白色文字，类似 AutoCAD
- **光标保护** — 始终定位在提示文字末尾，不可覆盖提示
- **Esc** 取消当前命令
- **Enter** 提交命令或确认操作（多段线/弧的完成）

### 5. 命令行输入

完整的交互式命令调度，与 AutoCAD 别名一致：

#### 绘图命令
| 别名 | 全称 | 功能 |
|------|------|------|
| `L` | LINE | 画直线 |
| `PL` | PLINE | 画多段线 |
| `C` | CIRCLE | 画圆 |
| `A` | ARC | 画弧 |
| `REC` | RECTANGLE | 画矩形 |
| `POL` | POLYGON | 画多边形 |
| `EL` | ELLIPSE | 画椭圆 |
| `T` | TEXT | 画文字（弹出对话框） |

#### 修改命令
| 别名 | 全称 | 功能 |
|------|------|------|
| `M` | MOVE | 移动实体 |
| `CO` | COPY | 复制实体 |
| `RO` | ROTATE | 旋转实体 |
| `SC` | SCALE | 缩放实体 |
| `MI` | MIRROR | 镜像实体 |
| `O` | OFFSET | 偏移实体 |
| `E` | ERASE | 删除选中 |

#### 视图 / 开关命令
| 命令 | 功能 |
|------|------|
| `Z` / `ZOOM` | 窗口缩放 |
| `ZE` / `ZOOME` | 范围缩放（显示全部） |
| `P` / `PAN` | 平移模式 |
| `U` / `UNDO` | 撤销 |
| `REDO` | 重做 |
| `GRID` / `F7` | 切换网格 |
| `SNAP` / `F9` | 切换吸附 |
| `ORTHO` / `F8` | 切换正交 |
| `OSNAP` / `F3` | 切换对象捕捉 |
| `SCR` / `SCRIPT` | 读取并执行 SCR 脚本 |
| `SCRREC` / `SCRIPTREC` | 开始录制 SCR 脚本 |
| `SCRSTOP` / `SCRIPTSTOP` | 停止录制 SCR 脚本 |

### 6. 坐标输入

直接在命令行输入坐标（AutoCAD 风格）：

| 格式 | 示例 | 含义 |
|------|------|------|
| 绝对坐标 | `100,200` | 世界坐标 (100, 200) |
| 相对坐标 | `@50,0` | 相对上一点 X 偏移 50 |
| 极坐标 | `100<45` | 距离 100，角度 45° |
| 数值 | `50` | 半径/角度/比例因子（根据上下文） |

**操作示例：**
```
L [Enter]              → 启动 LINE 命令
100,200 [Enter]        → 第一点 (100, 200)
300,400 [Enter]        → 第二点 (300, 400)，直线绘制完成
```

### 7. 视图控制

| 操作 | 功能 |
|------|------|
| 鼠标滚轮 | 以光标为中心缩放 |
| 鼠标中键拖拽 | 平移视图 |
| `Z` → 拖拽窗口 | 窗口缩放 |
| `ZE` | 缩放至全部实体范围 |

### 8. 网格与捕捉

| 开关 | 按键 | 说明 |
|------|------|------|
| 网格 | F7 | 浅灰色点状网格 |
| 吸附网格 | F9 | 鼠标点吸附到网格交点 |
| 正交 | F8 | 约束到水平或垂直方向 |
| 对象捕捉 | F3 | 捕捉端点、中点、圆心、象限点、最近点 |

### 9. 图层

- 通过 格式 > 图层 菜单打开图层操作
- 每个实体可分配到指定图层
- 支持按图层控制可见性
- 当前图层颜色应用于新实体

### 10. 夹点编辑

- 点击实体选中 → 显示青色夹点
- 拖拽夹点可改变几何形状
- Ctrl+Z 可撤销夹点编辑

### 11. 属性设置

- **线条颜色** — 7 种预设颜色（红/黄/绿/青/蓝/品红/白）
- **线型** — 实线、虚线、点线、点划线
- **线宽** — 4 级粗细 (1–4)

### 12. SCR 脚本录制与读取

- 通过 脚本 > 开始录制 / 停止录制 / 运行脚本 菜单管理 `.scr` 文件
- `SCRREC` / `SCRIPTREC` / `RECORDSCRIPT` 录制后续命令行输入、菜单命令和鼠标拾取坐标
- `SCRSTOP` / `SCRIPTSTOP` / `STOPSCRIPT` 关闭当前录制文件
- `SCR` / `SCRIPT` 读取 `.scr` 文件，并复用现有命令行解析与坐标输入流程执行
- 脚本采用逐行命令格式，空行表示 Enter，可用于结束多段线或确认圆弧
- 支持 `;` / `#` 注释，以及 `TEXT x,y height "content"` 形式的文字脚本输入
- 读取端兼容常见 AutoCAD 前缀（如 `_.LINE`），并支持直线、圆、圆弧、多段线、矩形、椭圆、正多边形、点、文字、图层、颜色、线型、线宽和显示开关等脚本命令

---

## 构建说明

```powershell
msbuild LargeHW.sln /p:Configuration=Debug /p:Platform=x64
```

输出：`x64\Debug\LargeHW.exe`

---

## 快捷键参考

| 按键 | 功能 |
|------|------|
| `Ctrl+Z` | 撤销 |
| `Ctrl+Y` | 重做 |
| `Ctrl+A` | 全选 |
| `Delete` | 删除选中 |
| `Esc` | 取消当前命令 |
| `Enter` | 确认 / 完成多段线或弧 |
| `F3` | 切换对象捕捉 |
| `F7` | 切换网格显示 |
| `F8` | 切换正交模式 |
| `F9` | 切换吸附网格 |

## Current Command Additions

| Command | Alias | Steps |
|------|------|------|
| `CHAMFER` | `CHA` | Select first `LINE` -> optionally enter distance -> select second `LINE`. |
| `ARRAY` / `ARRAYRECT` | `AR` | Select objects -> rows -> columns -> row spacing -> column spacing. |
| `ZOOM E` | `ZE` / `ZOOME` / `Z` then `E` | Zoom extents, fitting all visible entities. |

SCR/direct command forms:

```scr
ARRAY ALL 3 4 100 200
CHAMFER 20
ZOOM E
```

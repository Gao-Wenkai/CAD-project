# LargeHW — 2D CAD 绘图系统

基于 **MFC (Microsoft Foundation Classes)** 的单文档界面 (SDI) 二维 CAD 应用程序，仿 AutoCAD 交互风格，支持多种图元的绘制与编辑。

## 功能特性

### 绘图命令
| 命令 | 说明 |
|------|------|
| Line | 直线（两点） |
| Polyline | 多段线（连续点击添加顶点，支持闭合） |
| Circle | 圆（圆心 + 半径） |
| Arc | 圆弧（三点定弧） |
| Rectangle | 矩形（对角两点） |
| Polygon | 正多边形（3-12 边，圆心 + 半径） |
| Ellipse | 椭圆（圆心 + X/Y 半径） |
| Text | 单行文字 |

### 编辑命令
- **Move** — 移动选中图元（基点 + 目标点）
- **Copy** — 复制选中图元
- **Rotate** — 旋转选中图元（中心 + 角度）
- **Scale** — 缩放选中图元
- **Delete** — 删除选中图元
- **Mirror** — 镜像
- **Offset** — 偏移

### 视图控制
- 鼠标滚轮缩放
- 右键拖动平移
- Zoom Extents / Zoom Window
- 网格显示 (Grid)、对象捕捉 (Snap)、正交锁定 (Ortho)

### 属性控制
- **7 种颜色**：红、黄、绿、青、蓝、品红、白
- **4 种线型**：实线、虚线、点线、点划线
- **4 种线宽**
- 图层支持

### 其他
- 撤销/重做 (Undo/Redo)
- 图元选中高亮 + 夹点 (Grips) 拖拽编辑
- 文件序列化保存/打开 (CArchive)
- SCR 脚本录制与读取
- AutoCAD 风格深色背景 + 十字光标 + UCS 图标

## 图元体系 (Entity Hierarchy)

```
CEntity (抽象基类)
├── CLineEntity         直线
├── CCircleEntity       圆
├── CArcEntity          圆弧（三点）
├── CRectangleEntity    矩形
├── CPolygonEntity      正多边形
├── CEllipseEntity      椭圆
├── CPolylineEntity     多段线
└── CTextEntity         文字
```

所有图元通过 `DECLARE_SERIAL` / `IMPLEMENT_SERIAL` 宏支持 MFC 序列化。

## 交互状态机

采用 AutoCAD 风格的命令行交互流程，通过 `CadDrawState` 枚举定义各命令的状态跳转（如 `STATE_DRAW_LINE_P1 → STATE_DRAW_LINE_P2`），模拟"输入命令 → 指定第一点 → 指定第二点 → 完成"的流程。

## 技术栈

- **语言**: C++ (MFC)
- **IDE**: Visual Studio 2019+ (vc142 工具集)
- **平台**: Windows
- **界面框架**: MFC SDI (单文档界面)

## 构建与运行

1. 使用 Visual Studio 2019 或更高版本打开 `LargeHW.sln`
2. 选择 Debug 或 Release 配置
3. 生成解决方案 (F7)
4. 运行 (F5)

编译输出位于 `Debug/LargeHW.exe`。

## 文件结构

| 文件 | 说明 |
|------|------|
| `Entity.h / Entity.cpp` | 图元类层次结构定义与实现 |
| `LargeHWView.h / LargeHWView.cpp` | 视图层：渲染引擎 + 交互状态机 |
| `LargeHWDoc.h / LargeHWDoc.cpp` | 文档层：图元数据管理 |
| `MainFrm.h / MainFrm.cpp` | 主框架窗口 |
| `LargeHW.h / LargeHW.cpp` | 应用程序入口 |
| `res/` | 图标、位图等资源文件 |

## Current Command Additions

| Command | Alias | Behavior |
|------|------|------|
| Chamfer | `CHA` | Select two `LINE` entities to trim and connect them with a chamfer segment. Enter a number at the first prompt to set the chamfer distance. |
| Array | `AR` | Rectangular array for selected entities: rows, columns, row spacing, then column spacing. |
| Zoom Extents | `ZE`, `ZOOME`, `ZOOM E`, `Z` then `E` | Fits all visible entities in the current view. |

SCR/direct examples:

```scr
ARRAY ALL 3 4 100 200
CHAMFER 20
ZOOM E
```

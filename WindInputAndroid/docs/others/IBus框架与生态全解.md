# IBus 框架与生态全解

> 目标：读完能理解 IBus 的运行时结构、进程模型、引擎生命周期、完整 API 面、生态现状，并能动手写一个能上屏的引擎。
> 环境说明：本机未安装 IBus 开发包，本文的 API 名称与调用方式以 `deps/ibus-rime` 的真实源码、鲸鱼 `whale-engine` 的 GObject 虚表实现为基准；D-Bus 方法签名以 `busctl introspect` 结果为准，文中给出的是稳定骨架。

---

## 1. IBus 是什么

### 1.1 一句话

IBus（Intelligent Input Bus）是一个**基于 D-Bus 的输入法框架**：由常驻的 `ibus-daemon` 做总线与注册中心，每个输入法是一个**独立进程**（engine），客户端应用通过 GTK/Qt 的 immodule 或 XIM 协议把按键发给 daemon，daemon 再转发给当前引擎。

### 1.2 历史与定位

| 项 | 内容 |
|---|---|
| 诞生 | 2008 年，Red Hat 的 Huang Peng 发起，目标是取代 SCIM |
| 维护 | IBus 社区 / 各大发行版（Red Hat、Ubuntu、Debian 均有参与） |
| 许可 | LGPL-2.1+（框架），引擎各自许可 |
| 语言 | C + GLib/GObject + D-Bus |
| 现状 | GNOME 桌面的事实默认输入法框架；Fedora、Ubuntu、Debian 默认 |
| 主要竞争者 | Fcitx5（KDE/Arch/中文社区更常用）、macOS/Windows 各自原生框架 |

### 1.3 设计哲学

1. **框架与引擎分离**：框架只负责"按键送进来、候选送出去"，不懂任何语言。
2. **进程隔离**：引擎是独立进程，崩溃不会带走 daemon 和整个桌面（这是相对 Fcitx5 进程内 addon 的最大优势）。
3. **声明式注册**：引擎通过一个 XML 文件向系统登记自己，不需要改框架代码。
4. **面向对象**：引擎是 GObject 子类，通过覆写虚函数接入；输出通过 signal 发回框架。
5. **总线即协议**：所有跨进程交互都是 D-Bus 方法/信号，任何语言只要能说 D-Bus 就能写引擎。

---

## 2. 运行时架构

### 2.1 五个角色

```
┌────────────────┐        D-Bus        ┌─────────────────────┐
│ 客户端应用      │◄───────────────────►│  ibus-daemon        │
│ (GTK/Qt imodule│                     │  · 注册表/引擎管理   │
│  或 XIM)       │                     │  · 输入上下文管理    │
└────────────────┘                     │  · 全局快捷键        │
                                       └──────┬──────┬───────┘
                                              │      │
                         ┌────────────────────┘      └──────────────┐
                         │ D-Bus                                    │ D-Bus
                  ┌──────▼───────┐                          ┌────────▼────────┐
                  │ 引擎进程      │                          │ 面板进程         │
                  │ ibus-engine-* │                          │ ibus-ui-gtk3     │
                  │ (每引擎一个)  │                          │ 或 GNOME Shell   │
                  └──────────────┘                          └─────────────────┘
```

| 角色 | 进程 | 职责 |
|---|---|---|
| `ibus-daemon` | 独立常驻 | 读组件注册表、按需启动引擎、维护 InputContext、转发按键、处理全局快捷键、管理面板 |
| engine | 每个引擎一个进程 | 接收按键、维护组合状态、产生候选、发提交/预编辑/候选表 |
| panel | 独立进程（或 shell 内建） | 画候选窗、状态图标、输入法菜单；不参与输入逻辑 |
| 客户端 | 应用自身 | 通过 immodule 创建 InputContext，把按键交给 IBus，接收提交文本与预编辑 |
| 配置 | GSettings / dconf | 全局配置、面板配置、引擎私有配置 |

### 2.2 D-Bus 拓扑

IBus 在会话总线上注册若干名字，核心是这几类（路径/接口名稳定，方法签名以 `busctl introspect` 为准）：

| 名字 / 接口 | 归属 | 作用 |
|---|---|---|
| `org.freedesktop.IBus` | daemon | 创建 InputContext、注册组件、查询/切换引擎、退出/重启 |
| `org.freedesktop.IBus.InputContext` | 每个客户端上下文一个对象 | 收按键、焦点、光标位置、能力位；返回提交文本/预编辑 |
| `org.freedesktop.IBus.Engine` | 引擎侧对象 | 引擎→框架的信号：提交、预编辑、候选表、属性、转发按键 |
| `org.freedesktop.IBus.Factory` | 引擎侧对象 | daemon 调 `CreateEngine` 创建引擎实例 |
| `org.freedesktop.IBus.Panel` | 面板 | 候选窗/状态区/属性菜单的远程控制 |
| `org.freedesktop.IBus.Config` | daemon | 旧配置接口（已弃用，改用 GSettings） |
| `org.freedesktop.IBus.Service` | 通用 | 服务生命周期（Destroy） |
| `org.freedesktop.portal.IBus` | portal | Flatpak/Snap 沙箱内的受限入口 |

**重要理解**：引擎并不直接和客户端应用说话，也不直接画面板。引擎只和 daemon 说话；面板是 daemon 的另一个客户端。这条"星型"拓扑决定了：引擎拿不到应用的窗口句柄，只能拿到 daemon 转发来的光标矩形和能力位。

### 2.3 一次按键的完整路径

```
应用收到键盘事件
  → GTK/Qt immodule 过滤（或 XIM）
  → InputContext.ProcessKeyEvent(keyval, keycode, state)  [D-Bus]
  → daemon 先匹配全局快捷键（可能截获）
  → 转发给当前 engine 的 process_key_event
  → 引擎返回 TRUE/FALSE
      TRUE  → 按键被消费，不再下发
      FALSE → daemon 让客户端自行处理（或 XIM forward）
  → 引擎在需要时发：
      CommitText / UpdatePreeditText / UpdateLookupTable ...
  → daemon 更新 InputContext 状态并通知 panel
  → panel 画候选窗；客户端收到 commit 并插入文本
```

注意：**引擎返回 TRUE 只表示"我处理了"**，上屏必须显式调 `commit_text`；两者是独立的。

### 2.4 启动时序

1. 桌面会话启动 `ibus-daemon -drx`（`-d` 后台、`-r` 替换旧实例、`-x` 启动 XIM 服务）。
2. daemon 扫描组件目录，把每个 XML 解析成 `IBusComponent`/`IBusEngineDesc`，写入注册表缓存。
3. 用户切换输入法（快捷键或面板），daemon 选定 engine。
4. daemon 按 component 的 `<exec>` **fork/exec** 引擎进程，并传 `--ibus`（以及 `--desktop` 等）。
5. 引擎初始化 GLib、`ibus_init()`、连接会话总线。
6. 引擎请求自己的总线名（例：`im.rime.Rime`），创建 `IBusFactory`，调 `ibus_factory_add_engine()` 注册 engine 类型。
7. daemon 调 Factory 的 `CreateEngine(name)`，拿到 Engine 对象并绑定到当前 InputContext。
8. 按键开始流动；`focus_out`/切换引擎时引擎实例可能被销毁，下次按需重建。

---

## 3. 组件注册：component XML

### 3.1 安装位置

| 范围 | 路径 |
|---|---|
| 系统 | `/usr/share/ibus/component/*.xml` |
| 用户 | `~/.local/share/ibus/component/*.xml` |
| 注册表缓存 | `~/.cache/ibus/bus/registry`（由 daemon 生成） |

改完 XML 后需要让 daemon 重新扫描：`ibus write-cache --force` 或重启 daemon（`ibus restart`）。

### 3.2 完整字段

```xml
<?xml version="1.0" encoding="utf-8"?>
<component>
  <name>org.example.IBus.Demo</name>          <!-- 组件唯一名，建议反向域名 -->
  <description>Demo Component</description>
  <exec>/usr/libexec/ibus-engine-demo --ibus</exec>
  <version>0.1</version>
  <author>Your Name &lt;you@example.com&gt;</author>
  <license>GPL-3.0</license>
  <homepage>https://example.org</homepage>
  <textdomain>ibus-demo</textdomain>          <!-- gettext 域 -->
  <setup>/usr/libexec/ibus-setup-demo</setup> <!-- 可选：配置程序 -->
  <engines>
    <engine>
      <name>demo</name>                       <!-- 短名，ibus engine demo 用它 -->
      <language>zh</language>
      <license>GPL-3.0</license>
      <author>Your Name</author>
      <icon>/usr/share/ibus-demo/icons/demo.png</icon>
      <layout>default</layout>                <!-- 建议 default，别改用户键盘布局 -->
      <longname>Demo 拼音</longname>
      <description>Demo Pinyin Engine</description>
      <rank>1</rank>                          <!-- 面板排序，越小越前 -->
      <symbol>示</symbol>                      <!-- 状态栏显示的一两个字 -->
    </engine>
  </engines>
</component>
```

### 3.3 三个容易搞混的名字

| 名字 | 例子 | 用途 |
|---|---|---|
| component `<name>` | `org.example.IBus.Demo` | 组件/总线名，daemon 用它标识"这是哪个组件" |
| engine `<name>` | `demo` | 用户可见的引擎 ID，`ibus engine demo`、`IBUS_ENGINE_NAME=demo` |
| 总线名（代码里 request） | `org.freedesktop.IBus.Demo` | 引擎进程在总线上占的名字，必须唯一 |

三者不一致是最常见的"装了但列表里没有"原因。

### 3.4 注册与调试命令

```bash
ibus list-engine                 # 列出所有引擎（短名 + 长名）
ibus engine                      # 当前引擎
ibus engine demo                 # 切换
ibus restart                     # 重启 daemon
ibus write-cache --force         # 重建注册表缓存
ibus read-config                 # 读当前配置
ibus setup                       # 打开 ibus-setup
```

---

## 4. 引擎对象模型与生命周期

### 4.1 GObject 子类

引擎必须继承 `IBUS_TYPE_ENGINE`：

```c
typedef struct _DemoEngine      { IBusEngine parent; } DemoEngine;
typedef struct _DemoEngineClass { IBusEngineClass parent; } DemoEngineClass;
G_DEFINE_TYPE (DemoEngine, demo_engine, IBUS_TYPE_ENGINE)
```

- `class_init` 里把函数指针挂到 `IBusEngineClass`；
- `init` 里做实例初始化（不要在这里加载大词典，见性能章）；
- `destroy`（`IBusObjectClass`）里释放资源；
- 用 `g_object_new(demo_engine_get_type(), "engine-name", name, NULL)` 创建。

### 4.2 生命周期回调的语义差别

这是最容易写错的部分。四个回调看起来都像"重置"，实际语义不同：

| 回调 | 触发时机 | 应该做什么 | 不应该做什么 |
|---|---|---|---|
| `enable` | 引擎被启用（切到该引擎） | 初始化引擎级资源、注册属性 | 假设有焦点/有 context |
| `disable` | 引擎被停用 | 保存状态、释放引擎级资源 | 提交文本 |
| `focus_in` | 某个客户端输入框获得焦点 | 为该 InputContext 建/绑 session，刷新 UI | 重置用户词库等全局状态 |
| `focus_out` | 输入框失焦 | 通常提交或丢弃组合串，销毁 session | 假设还会收到同一个 IC |
| `reset` | 应用/框架要求丢弃组合状态 | **立即清空 buffer、候选、预编辑** | 保留"下次还能接着打"的状态 |

鲸鱼的实现里 `reset` 直接调核心的 `reset()` 并清 UI，注释写得很明白："IBus 规范：客户端应用请求 reset 时必须立即丢弃全部组合状态。"

### 4.3 实例粒度

- 通常是**每个 InputContext 一个 engine 实例**（ibus-rime 在 `enable`/`focus_in` 时建 Rime session，`disable`/`focus_out` 时销毁）。
- 也有引擎做成"一个进程一个全局状态"，但多窗口/多应用切换时会串状态，不推荐。
- 引擎实例销毁后 daemon 可能再次 `CreateEngine`，所以**不要把"只初始化一次"的东西放在实例构造里**；进程级单例要单独管理。

---

## 5. `IBusEngine` 虚函数表

以下是 `IBusEngineClass` 里可覆写的核心方法。签名以 `deps/ibus-rime/rime_engine.c` 的实现为准。

| 虚函数 | 签名要点 | 说明 |
|---|---|---|
| `process_key_event` | `(engine, guint keyval, guint keycode, guint state) -> gboolean` | 主入口；返回 TRUE 表示消费 |
| `focus_in` / `focus_out` | `(engine)` | 焦点变化 |
| `reset` | `(engine)` | 丢弃组合状态 |
| `enable` / `disable` | `(engine)` | 引擎启用/停用 |
| `set_cursor_location` | `(engine, gint x, gint y, gint w, gint h)` | 光标矩形（相对窗口，含缩放） |
| `set_capabilities` | `(engine, guint caps)` | 客户端能力位，决定能否用预编辑/周围文本 |
| `property_activate` | `(engine, const gchar *name, guint state)` | 属性菜单项被点击（中英切换、简繁等） |
| `property_show` / `property_hide` | `(engine, const gchar *name)` | 属性显示/隐藏 |
| `candidate_clicked` | `(engine, guint index, guint button, guint state)` | 鼠标点候选 |
| `page_up` / `page_down` | `(engine)` | 翻页（面板按钮触发） |
| `cursor_up` / `cursor_down` | `(engine)` | 上下键导航（可选） |
| `set_surrounding_text` | `(engine, IBusText *text, guint cursor, guint anchor)` | 应用把光标前后文本给你 |
| `set_content_type` | `(engine, guint purpose, guint hints)` | 密码框/邮箱/数字等输入目的 |
| `process_hand_writing_event` | `(engine, ...)` | 手写输入（很少用） |

**返回值语义**：
- `TRUE`：这个键我处理了，不要再给应用。
- `FALSE`：不处理，交给 daemon/应用。
- 注意 release 事件：`state & IBUS_RELEASE_MASK` 表示按键抬起；大部分引擎只处理按下，但**修饰键的抬起必须正确返回**，否则 Shift/Ctrl 会卡住。

---

## 6. 输出 API：把结果送回框架

引擎不能直接改 UI，必须调用 `ibus_engine_*` 系列函数（内部发 D-Bus 信号）。

### 6.1 提交与预编辑

| 函数 | 作用 | 关键参数 |
|---|---|---|
| `ibus_engine_commit_text(engine, IBusText *text)` | 上屏 | text 的所有权转移（floating ref 被消耗） |
| `ibus_engine_update_preedit_text(engine, IBusText *text, guint cursor_pos, gboolean visible)` | 更新预编辑 | `cursor_pos` 是**字符**位置，不是字节 |
| `ibus_engine_update_preedit_text_with_mode(engine, text, cursor, visible, mode)` | 带模式 | mode 见下 |
| `ibus_engine_show_preedit_text` / `hide_preedit_text` | 显示/隐藏 | — |
| `ibus_engine_update_auxiliary_text(engine, IBusText *text, gboolean visible)` | 辅助文本（候选窗下方提示） | — |
| `ibus_engine_update_lookup_table(engine, IBusLookupTable *table, gboolean visible)` | 候选表 | — |
| `ibus_engine_show_lookup_table` / `hide_lookup_table` | 显示/隐藏候选 | — |
| `ibus_engine_register_properties(engine, IBusPropList *props)` | 注册状态区属性 | 中英、简繁、全半角 |
| `ibus_engine_update_property(engine, IBusProperty *prop)` | 更新单个属性 | — |
| `ibus_engine_forward_key_event(engine, keyval, keycode, state)` | 把键转发给应用 | 用于"引擎想吃但最终决定不消费" |
| `ibus_engine_delete_surrounding_text(engine, gint offset, guint nchars)` | 删除光标周围文本 | — |
| `ibus_engine_require_surrounding_text(engine)` | 请求周围文本 | — |

预编辑模式（`IBusEnginePreeditMode`）：

| 值 | 含义 |
|---|---|
| `IBUS_ENGINE_PREEDIT_CLEAR` | 预编辑清空，不在应用里显示 |
| `IBUS_ENGINE_PREEDIT_COMMIT` | 预编辑内容直接提交（行内预编辑） |
| `IBUS_ENGINE_PREEDIT_NOTHING` | 保持现状，只更新候选窗 |

### 6.2 数据结构

**IBusText / IBusAttribute**

```c
IBusText *text = ibus_text_new_from_string ("ni hao");
ibus_text_append_attribute (text, IBUS_ATTR_TYPE_UNDERLINE,
                            IBUS_ATTR_UNDERLINE_SINGLE, 0, 5);
```

- `IBUS_ATTR_TYPE_UNDERLINE / FOREGROUND / BACKGROUND`
- `start_index` / `end_index` 以**字符**计；
- 下划线样式：`SINGLE / DOUBLE / LOW / ERROR`。

**IBusLookupTable**

```c
IBusLookupTable *table = ibus_lookup_table_new (page_size, cursor_pos,
                                                cursor_visible, round);
ibus_lookup_table_set_orientation (table, IBUS_ORIENTATION_HORIZONTAL);
ibus_lookup_table_append_candidate (table, text);
ibus_engine_update_lookup_table (engine, table, TRUE);
```

- `page_size` 通常 5–9；`round` 表示翻页是否循环；
- `cursor_pos` 是当前高亮项（在本页内）；
- 方向：`IBUS_ORIENTATION_HORIZONTAL` / `VERTICAL`；
- `labels` 可自定义序号（默认 1–9）。

**IBusProperty / IBusPropList**

- 类型：`NORMAL / TOGGLE / RADIO / MENU`
- 常用：中英切换（TOGGLE，`state` 表示开/关）、简繁（TOGGLE）、全半角（TOGGLE）、方案选择（RADIO/MENU + `sub_props`）
- 每个属性要有唯一 `key`，`property_activate` 里按 key 分发。

### 6.3 内存与引用

- `ibus_text_new_from_string()` 返回**浮引用**；传给 `commit_text`/`update_preedit_text` 后所有权被消耗，不要再 `g_object_unref`。
- 自己保留的 text 要 `g_object_ref_sink()`。
- `IBusLookupTable` 同理，传出去后不要再改（下次重新建一个更安全）。

---

## 7. 按键模型与输入语义

### 7.1 keyval / keycode / state

- `keyval`：X11 keysym（如 `IBUS_space=0x20`、`IBUS_Return=0xff0d`、`IBUS_Shift_L=0xffe1`）。
- `keycode`：硬件扫描码（X11 keycode），不同键盘可能不同，**不要用它判断字符**。
- `state`：修饰键位掩码，和 X11 一致：
  - `IBUS_SHIFT_MASK = 1 << 0`
  - `IBUS_LOCK_MASK = 1 << 1`（CapsLock）
  - `IBUS_CONTROL_MASK = 1 << 2`
  - `IBUS_MOD1_MASK = 1 << 3`（Alt）
  - `IBUS_MOD4_MASK = 1 << 6`（Super）
  - `IBUS_RELEASE_MASK = 1 << 30`
- 因为掩码与 X11 一致，**引擎几乎不需要做键位转换**；这也是 librime 能直接吃 IBus 按键的原因。

### 7.2 判断按键的常见写法

```c
if (state & IBUS_RELEASE_MASK) return FALSE;      // 只处理按下
if (state & (IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_MOD4_MASK))
    return FALSE;                                  // 带 Ctrl/Alt/Super 的透传
if (keyval >= 'a' && keyval <= 'z') { ... }
```

`IBUS_Shift_L` 的按下/抬起要单独处理：很多输入法用"单击 Shift 切中英"，必须记录按下时间并在抬起时判断是否单独按下。

### 7.3 谁先拿到键

1. 桌面环境/daemon 的全局快捷键（切输入法、切引擎）；
2. 引擎的 `process_key_event`；
3. 未消费 → 客户端应用；
4. XIM 场景下由 `ibus-x11` 负责转发。

### 7.4 预编辑与 surrounding text

- `IBUS_CAP_PREEDIT_TEXT`：客户端支持预编辑（大多数 GTK/Qt 支持）。
- `IBUS_CAP_SURROUNDING_TEXT`：客户端愿意把光标前后文本给你（浏览器、编辑器常见；终端/密码框常常不给）。
- `IBUS_CAP_FOCUS`：客户端会正确发 focus in/out。
- `set_content_type` 的 `purpose` 包含 `IBUS_INPUT_PURPOSE_PASSWORD`、`EMAIL`、`NUMBER` 等；**密码框里不要记录、不要云请求、不要联想**。

---

## 8. 配置系统

### 8.1 GSettings

IBus 1.5 之后使用 GSettings（dconf 后端）：

| schema | 内容 |
|---|---|
| `org.freedesktop.ibus.general` | 引擎列表、切换快捷键、预加载引擎、面板行为 |
| `org.freedesktop.ibus.panel` | 候选窗位置、方向、字体、页面大小、显示方式 |
| `org.freedesktop.ibus.engine.<engine>` | 引擎私有配置（引擎自己安装 schema） |

```bash
gsettings list-recursively org.freedesktop.ibus.panel
gsettings set org.freedesktop.ibus.panel lookup-table-orientation 1
```

### 8.2 引擎自己的配置

- 在 `component` 里加 `<setup>/path/to/ibus-setup-demo</setup>`，`ibus-setup` 会显示"首选项"入口。
- 引擎可以安装自己的 GSettings schema（`/usr/share/glib-2.0/schemas/`），也可以退而求其次用 `dconf`/配置文件。
- 旧的 `org.freedesktop.IBus.Config` D-Bus 接口已弃用，新项目不要再依赖它。

---

## 9. 面板与 UI

### 9.1 谁画候选窗

| 环境 | 面板实现 | 说明 |
|---|---|---|
| 传统 X11 桌面 | `ibus-ui-gtk3` | IBus 自带 GTK3 面板，读 `org.freedesktop.ibus.panel` 配置 |
| GNOME | GNOME Shell 内建 | Shell 通过自己的 IBus 集成画候选窗；`ibus-ui-gtk3` 通常不启动 |
| KDE Plasma | 可用 kimpanel 或 Plasma 的 IBus 集成 | 与 Fcitx5 共存时通常二选一 |
| 其他轻量桌面 | `ibus-ui-gtk3` 或第三方 | 取决于会话配置 |

**引擎不应该自己画候选窗**（除非做沉浸式皮肤/悬浮窗）。理由：坐标系、缩放、多显示器、Wayland 都由面板处理；自绘会把这些坑全部接过来。

### 9.2 面板能显示什么

- 预编辑（组合串）
- 辅助文本（提示、拼音原串）
- 候选表（横/竖、页码、序号、注释）
- 状态区属性（中英、简繁、全半角、方案菜单）
- 输入法图标/名称

### 9.3 候选窗定位

- 引擎通过 `set_cursor_location(x, y, w, h)` 拿到光标矩形；
- 面板负责把矩形转成屏幕坐标并避让屏幕边缘；
- 引擎**不要**假设坐标是全局屏幕坐标；多显示器/分数缩放时尤其要注意。

---

## 10. 生态全景

### 10.1 主流引擎

| 引擎 | 语言/类型 | 说明 |
|---|---|---|
| `ibus-libpinyin` | C++，libpinyin | 最主流的拼音引擎，支持全拼/双拼/简拼、云候选（可选） |
| `ibus-rime` | C，librime | Rime 生态入口，方案/词库最丰富；本目录有源码 |
| `ibus-table` | C，表码 | 形码/表码通用引擎（五笔、仓颉、郑码等） |
| `ibus-anthy` | C++/Python | 日语 |
| `ibus-mozc` | C++ | 日语（Google Mozc） |
| `ibus-kkc` | C | 日语假名 |
| `ibus-skk` | C++ | 日语 SKK |
| `ibus-hangul` | C | 韩语 |
| `ibus-chewing` | C | 注音 |
| `ibus-unikey` | C++ | 越南语 |
| `ibus-typing-booster` | Python | 预测输入/自动补全，很好的 Python 引擎范例 |
| `ibus-m17n` | C | 通过 m17n 支持大量语言 |
| `ibus-emoji` | C | Emoji 输入 |
| `ibus-keyman` | C++ | Keyman 键盘布局 |

### 10.2 发行版与桌面

| 发行版/桌面 | 默认 | 备注 |
|---|---|---|
| Fedora / RHEL | IBus | GNOME 集成最完整 |
| Ubuntu / Debian | IBus | `im-config` 选择框架 |
| Arch Linux | 无默认（用户选） | 中文用户常选 Fcitx5 |
| KDE Plasma | 可用 Fcitx5 或 IBus | Plasma 自身有输入法配置模块 |
| Xfce / LXQt | 常见 IBus 或 Fcitx | 取决于发行版 |
| GNOME | IBus（内建） | Shell 自带候选窗，不走 `ibus-ui-gtk3` |

### 10.3 框架选择工具

- Debian/Ubuntu：`im-config`（写 `~/.xinputrc`）
- Fedora：`imsettings`
- 手动：环境变量 `GTK_IM_MODULE=ibus`、`QT_IM_MODULE=ibus`、`XMODIFIERS=@im=ibus`

### 10.4 Flatpak / Snap

- 沙箱内应用需要 `--talk-name=org.freedesktop.IBus` 或通过 `ibus-portal`；
- 引擎跑在沙箱外，提交文本经 portal 转发；
- 打包时要同时提供 portal 的 `.ini` 配置。

---

## 11. Wayland 与 X11

### 11.1 两种客户端接入方式

| 方式 | 适用 | 说明 |
|---|---|---|
| immodule（GTK/Qt） | 原生 GTK/Qt 应用 | 直接调 IBus 客户端库，走 D-Bus，最可靠 |
| XIM | 老应用、XWayland 应用 | `ibus-x11` 提供 XIM 服务，功能受限（预编辑/候选支持差） |

### 11.2 Wayland 下的现实

- IBus 本身是 **D-Bus 框架，不绑定显示服务器**，所以引擎代码不需要改。
- Wayland 客户端仍可通过 GTK/Qt immodule 使用 IBus（这是主流路径）。
- GNOME 由 Mutter/Shell 负责把 Wayland 输入法协议桥接到 IBus；候选窗由 Shell 画。
- 非 GNOME 合成器（Sway/Hyprland 等）通常**不用 IBus**，而用 Fcitx5（因为它实现了 input-method 协议）。
- 全局快捷键、窗口位置、焦点在 Wayland 下受限；`set_cursor_location` 拿到的坐标可能不精确，面板要做边界处理。

### 11.3 一句话

**IBus 的强项是"桌面集成好、进程隔离好、GNOME 默认"，弱项是"Wayland 自主性差、UI 定制弱、非 GNOME 桌面存在感低"。**

---

## 12. 开发实战：最小可运行引擎

### 12.1 C 版本（GObject）

```c
#include <ibus.h>

typedef struct _DemoEngine      { IBusEngine parent; } DemoEngine;
typedef struct _DemoEngineClass { IBusEngineClass parent; } DemoEngineClass;

G_DEFINE_TYPE (DemoEngine, demo_engine, IBUS_TYPE_ENGINE)

static gboolean
demo_process_key_event (IBusEngine *engine, guint keyval, guint keycode, guint state)
{
    if (state & IBUS_RELEASE_MASK)
        return FALSE;
    if (state & (IBUS_CONTROL_MASK | IBUS_MOD1_MASK | IBUS_MOD4_MASK))
        return FALSE;

    if (keyval == IBUS_Return) {
        IBusText *text = ibus_text_new_from_static_string ("你好");
        ibus_engine_commit_text (engine, text);   /* 所有权转移 */
        return TRUE;
    }
    return FALSE;
}

static void
demo_engine_class_init (DemoEngineClass *klass)
{
    IBUS_ENGINE_CLASS (klass)->process_key_event = demo_process_key_event;
}

static void
demo_engine_init (DemoEngine *self) { }

static IBusEngine *
demo_create_engine (IBusFactory *factory, const gchar *engine_name, gpointer user_data)
{
    return g_object_new (demo_engine_get_type (), "engine-name", engine_name, NULL);
}

gint
main (gint argc, gchar **argv)
{
    ibus_init ();
    IBusBus *bus = ibus_bus_new ();
    g_return_val_if_fail (ibus_bus_is_connected (bus), 1);

    IBusFactory *factory = ibus_factory_new (ibus_bus_get_connection (bus));
    ibus_factory_add_engine (factory, "demo", demo_engine_get_type ());

    if (!ibus_bus_request_name (bus, "org.freedesktop.IBus.Demo", 0))
        g_warning ("failed to request bus name");

    ibus_main ();
    return 0;
}
```

### 12.2 构建

```cmake
cmake_minimum_required(VERSION 3.16)
project(ibus-demo C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(IBUS REQUIRED ibus-1.0>=1.5.20)

add_executable(ibus-engine-demo demo.c)
target_include_directories(ibus-engine-demo PRIVATE ${IBUS_INCLUDE_DIRS})
target_link_libraries(ibus-engine-demo PRIVATE ${IBUS_LIBRARIES})
target_compile_options(ibus-engine-demo PRIVATE ${IBUS_CFLAGS_OTHER})

install(TARGETS ibus-engine-demo RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/ibus-demo)
install(FILES demo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/ibus/component)
```

### 12.3 Python 版本思路

IBus 提供 PyGObject 绑定，`ibus.engine.Engine` 是基类，覆写 `do_process_key_event` 等即可：

```python
import ibus
from ibus import keysyms

class DemoEngine(ibus.EngineBase):
    def do_process_key_event(self, keyval, keycode, state):
        if state & ibus.ModifierType.RELEASE_MASK:
            return False
        if keyval == keysyms.Return:
            self.commit_text(ibus.Text("你好"))
            return True
        return False

class DemoFactory(ibus.EngineFactoryBase):
    def create_engine(self, engine_name):
        return DemoEngine(self._bus, self._object_path, engine_name)
```

`ibus-typing-booster` 是完整的 Python 引擎参考（含工厂、配置、打包）。

### 12.4 Rust 版本思路

没有官方 Rust 绑定，两条路：
1. 手写 FFI + GObject 虚表（鲸鱼 `whale-engine` 的做法）：定义 `IBusEngineClass` 结构体布局并用 `_Static_assert` 校验 `sizeof`，在 `class_init` 里赋函数指针。优点是零依赖、可控；缺点是要跟 GObject ABI 版本走。
2. 用现成的 `ibus` crate（如果版本可用），封装程度高但更新滞后。

### 12.5 调试清单

```bash
# 1. 引擎是否被发现
ibus list-engine | grep demo

# 2. 组件 XML 是否解析
G_MESSAGES_DEBUG=all ibus-daemon -drx --verbose

# 3. 引擎进程是否起来、总线名是否抢到
busctl --user list | grep -i demo
busctl --user introspect org.freedesktop.IBus /org/freedesktop/IBus

# 4. 看 D-Bus 往返
dbus-monitor --session "interface='org.freedesktop.IBus.Engine'"

# 5. 直接指定引擎启动（绕过面板）
IBUS_ENGINE_NAME=demo ibus-daemon -drx

# 6. 日志
G_MESSAGES_DEBUG=all ibus-engine-demo --ibus
```

---

## 13. 常见坑（按踩坑频率排序）

1. **三个名字不一致**：component name、engine name、总线名必须自洽；否则引擎起得来但列表里没有，或切换时报 "no such engine"。
2. **忘了 `--ibus`**：`<exec>` 里不带 `--ibus`，引擎会以为自己是独立模式。
3. **改 XML 不重启/不刷缓存**：daemon 读的是缓存，`ibus write-cache --force` + `ibus restart` 是标准动作。
4. **release 事件处理错**：只判断 keyval 不看 `IBUS_RELEASE_MASK`，会导致 Shift 卡住、按键重复触发。
5. **`cursor_pos` 用字节数**：IBus 要的是字符位置；中文预编辑按字节会错位。
6. **浮引用处理错**：`commit_text` 后再 `g_object_unref` 会 double free；自己留的 text 要 `ref_sink`。
7. **在 `init` 里加载大词典**：引擎进程启动会卡住整个输入法切换；放到后台线程或首次按键时懒加载。
8. **在 `process_key_event` 里阻塞**：主循环是 GLib main context，阻塞会让候选窗和上屏都延迟。
9. **假设面板一定存在**：GNOME Shell 下没有 `ibus-ui-gtk3`，属性菜单/候选样式可能完全不显示。
10. **假设有 surrounding text**：终端、密码框、部分 Electron 应用不给；必须有降级路径。
11. **用 keycode 判断字符**：不同键盘布局下 keycode 不同，必须用 keyval。
12. **在密码框里联想/云请求**：`set_content_type` 的 purpose 是 `PASSWORD` 时必须完全禁用记录与外发。
13. **引擎崩了但用户以为输入法坏了**：daemon 会重启引擎，但组合状态丢失；要尽量在异常路径做清理。
14. **GPL 污染**：`ibus-rime` 是 GPL-3.0，闭源项目不要复制其代码；框架本身是 LGPL，动态链接通常没问题，但要按 LGPL 要求允许替换库。
15. **Wayland 下自绘候选窗**：焦点、坐标、层级都不可靠；优先用面板。

---

## 14. 什么时候选 IBus

**适合：**
- 目标桌面是 GNOME/Fedora/Ubuntu/Debian；
- 想要进程隔离，引擎崩溃不拖垮桌面；
- 想用 C/Python/Vala 快速写引擎；
- 需要成熟的 Flatpak/portal 集成。

**不适合：**
- 目标是 KDE/Sway/Hyprland 等 Wayland 合成器（Fcitx5 更合适）；
- 需要高度定制的候选窗/皮肤/悬浮工具栏；
- 需要低延迟的进程内引擎（Fcitx5 更快）；
- 需要同时管理大量输入法/复杂状态（Fcitx5 的 addon 模型更灵活）。

---

## 15. 速查表

| 项 | 值 |
|---|---|
| 组件目录 | `/usr/share/ibus/component/`、`~/.local/share/ibus/component/` |
| 注册表缓存 | `~/.cache/ibus/bus/registry` |
| 用户配置 | GSettings/dconf（`org.freedesktop.ibus.*`） |
| Rime 用户目录（ibus-rime） | `~/.config/ibus/rime` |
| 引擎基类 | `IBUS_TYPE_ENGINE` / `IBusEngineClass` |
| 主入口 | `process_key_event` |
| 上屏 | `ibus_engine_commit_text` |
| 预编辑 | `ibus_engine_update_preedit_text[_with_mode]` |
| 候选 | `ibus_engine_update_lookup_table` |
| 状态属性 | `ibus_engine_register_properties` |
| 最低现代版本 | IBus 1.5.20+ |
| 典型启动 | `ibus-daemon -drx` |
| 调试 | `ibus list-engine`、`ibus engine <name>`、`dbus-monitor`、`G_MESSAGES_DEBUG=all` |

---

## 附：和 Fcitx5 的核心差异（一句话版）

| 维度 | IBus | Fcitx5 |
|---|---|---|
| 引擎形态 | 独立进程 | 进程内 addon |
| 通信 | D-Bus | 函数调用 |
| 崩溃影响 | 只挂引擎 | 拖垮整个输入法 |
| 延迟 | 多一次 D-Bus 往返 | 最低 |
| UI 定制 | 受面板限制 | addon 可完全接管 |
| Wayland | 依赖桌面桥接 | 自带 input-method 协议 |
| 生态重心 | GNOME/Fedora/Ubuntu | KDE/Arch/中文社区 |

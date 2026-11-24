# Fcitx 4 / Fcitx 5 框架与生态全解

> 目标：理解 Fcitx 两代框架的架构、插件模型、输入法接口、事件与 UI 机制、Wayland 现状和生态，并能判断"该用哪一代、怎么写、怎么迁"。
> 环境说明：本机装有 fcitx 4.2.9.6（含 `/usr/include/fcitx/` 开发头）和 fcitx5 4.99.0（早期快照）。Fcitx4 的接口描述直接对照本机头文件；Fcitx5 的接口描述以本目录中 WindInput、水杉 msime、Sime、鲸鱼四个现代 addon 的真实代码为准（均面向 Fcitx5 5.0.20+ / 5.1.x）。

---

## 0. 一分钟版本

| 问题 | 答案 |
|---|---|
| Fcitx 是什么 | 中文社区最主流的 Linux 输入法框架，单进程 + 插件化 |
| 现在该用哪代 | **Fcitx5**。Fcitx4 已 EOL，只用于老系统兼容 |
| 和 IBus 最大区别 | 引擎/输入法是**进程内 addon**，不是独立进程；走函数调用，不走 D-Bus |
| 写输入法难吗 | 最小 addon 约 100 行：继承 `InputMethodEngine`，实现 `keyEvent`，调 `commitString` |
| 最大坑 | addon 崩了会拖垮整个 fcitx5；Wayland 下 GNOME 不支持它 |

---

## 1. 版本谱系与定位

### 1.1 名字与作者

Fcitx = "Free Chinese Input Toy for X"，最初是 YUKING 写的中文输入法，后来由 Weng Xuetian（CSSlayer）重构为插件化框架。Fcitx4 与 Fcitx5 都是他主导设计。

### 1.2 三代架构

| 代际 | 时间 | 架构特点 | 状态 |
|---|---|---|---|
| Fcitx 3 | 2007–2010 | 早期插件化，X11 中心 | 历史 |
| **Fcitx 4** | 2010–2020 | `fcitx` 单进程 + 动态 addon + frontend/ui/module 分类；全局 IM 状态 | **EOL**，仅维护性修复 |
| **Fcitx 5** | 2020– | 重写事件循环与对象模型；addon + InputContext 属性；原生 Wayland；现代 C++ | **活跃主线** |

### 1.3 两代的设计哲学差异

| 维度 | Fcitx4 | Fcitx5 |
|---|---|---|
| 语言 | C | C++17 |
| 事件循环 | 自建 + X11 事件 | 抽象 EventLoop / EventSource |
| 状态管理 | 全局单例 `FcitxInstance` | `Instance` + `InputContext` + `InputContextProperty` |
| 输入法接口 | `FcitxIM` 一堆函数指针 + 返回码位标志 | `InputMethodEngine` 虚类 + `KeyEvent` 消费语义 |
| UI | 独立 UI addon 画所有输入法 | UI addon + `InputPanel` 数据模型 |
| Wayland | 不支持（X11 only） | 原生支持 input-method 协议 |
| 配置 | `.desc` + `FcitxConfigData` | `fcitx-config` 模板 + INI |
| 生态 | 老系统、UOS/麒麟 | KDE/Arch/Debian/Fedora 主流 |

---

## 2. Fcitx 的通用架构

### 2.1 单进程 + 插件

```
┌──────────────────────────────────────────────────────────┐
│ fcitx / fcitx5 进程                                       │
│                                                          │
│  ┌────────────┐   ┌─────────────┐   ┌─────────────────┐  │
│  │ AddonManager│──►│ InputMethod │   │ Frontend addons │  │
│  │  加载/依赖  │   │  Engine(s)  │◄──│ xcb/wayland/xim │  │
│  └────────────┘   └──────┬──────┘   │ ibus/dbus       │  │
│                          │          └─────────────────┘  │
│                   ┌──────▼──────┐   ┌─────────────────┐  │
│                   │ InputContext│──►│ UI addons       │  │
│                   │  每输入框一份│   │ classicui/kimpanel│ │
│                   └─────────────┘   └─────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

- **frontend**：负责和客户端应用/显示服务器打交道（X11、Wayland、XIM、DBus、IBus 兼容层）。
- **input method engine**：负责"按键 → 候选 → 上屏"。
- **UI addon**：负责画候选窗/状态栏/菜单。
- **module**：其他能力（剪贴板、通知、快速短语、拼写检查、DBus 控制）。

**关键点**：引擎不直接和客户端说话，也不直接画窗口。它只操作 `InputContext` 和 `InputPanel` 数据模型，由 frontend 把结果送出去、由 UI addon 画出来。

### 2.2 与 IBus 的结构性对比

| 维度 | Fcitx5 | IBus |
|---|---|---|
| 引擎位置 | 同进程动态库 | 独立进程 |
| 跨进程 | frontend 才跨（XIM/Wayland/DBus） | 全程 D-Bus |
| 延迟 | 函数调用，最低 | 多一次总线往返 |
| 崩溃影响 | 整个输入法挂掉 | 只挂当前引擎 |
| 状态隔离 | 每个 InputContext 一份 property | 每个 IC 一个 engine 实例 |
| 调试 | gdb 直接进 addon | 要 attach 到对应引擎进程 |

---

## 3. Fcitx4 详解

### 3.1 组件模型

Fcitx4 的一切都是 addon，addon 分四类（`FcitxAddonCategory`）：

| 类别 | 作用 | 例子 |
|---|---|---|
| `Frontend` | 客户端接入 | `fcitx-x11`、`fcitx-gtk2/3`、`fcitx-qt4/5`、`fcitx-xim` |
| `InputMethod` | 输入法 | `fcitx-pinyin`、`fcitx-rime`、`fcitx-table`、`fcitx-libpinyin` |
| `UI` | 面板/候选窗 | `fcitx-ui-classic`、`fcitx-ui-gtk`、`fcitx-ui-qt`、`fcitx-kimpanel-ui` |
| `Module` | 其他能力 | `fcitx-clipboard`、`fcitx-notify`、`fcitx-spell`、`fcitx-dbus` |

### 3.2 addon 元数据

安装位置：`/usr/share/fcitx/addon/*.conf`、`~/.config/fcitx/addon/`。

```ini
[Addon]
Name=fcitx-demo
GeneralName=Demo IM
Comment=Demo input method
Category=InputMethod
Enabled=True
Library=fcitx-demo.so
Type=SharedLibrary
Priority=5
Advance=False
```

| 字段 | 含义 |
|---|---|
| `Name` | addon 标识 |
| `GeneralName` / `Comment` | 用户可见名与说明（可带 `[zh_CN]` 翻译） |
| `Category` | Frontend/InputMethod/UI/Module |
| `Library` | `.so` 文件名 |
| `Type` | `SharedLibrary` / `StaticLibrary` / `Exec` 等 |
| `Priority` | 加载/显示优先级 |
| `Depend` | 依赖的 addon |
| `Advance` | 是否"高级"（默认隐藏） |

### 3.3 输入法插件接口

Fcitx4 的输入法是一个 `FcitxIMClass` / `FcitxIMClass2`：

```c
typedef struct _FcitxIMClass {
    void* (*Create)(FcitxInstance *instance);
    void  (*Destroy)(void *arg);
} FcitxIMClass;

typedef struct _FcitxIMClass2 {
    void* (*Create)(FcitxInstance *instance);
    void  (*Destroy)(void *arg);
    void  (*ReloadConfig)(void *arg);
    /* 5 个 padding */
} FcitxIMClass2;
```

`Create` 里调用 `FcitxInstanceRegisterIM()` 或 `FcitxInstanceRegisterIMv2()`，把一组回调注册进去：

```c
void FcitxInstanceRegisterIMv2(
    FcitxInstance *instance,
    void *imclass,            /* 你的私有状态 */
    const char *uniqueName,   /* 唯一名，如 "demo" */
    const char *name,         /* 显示名 */
    const char *iconName,
    FcitxIMIFace iface,       /* 回调表 */
    int priority,
    const char *langCode);    /* "zh_CN" */
```

`FcitxIMIFace` 的核心成员：

| 成员 | 作用 |
|---|---|
| `ResetIM` | 清空组合状态 |
| `DoInput` | 处理按键，返回 `INPUT_RETURN_VALUE` |
| `GetCandWords` | 生成候选列表 |
| `Save` | 保存用户数据 |
| `Init` | 切换到该输入法时调用 |
| `ReloadConfig` | 重载配置 |
| `KeyBlocker` | 判断某键是否应被拦截（不转发给应用） |
| `UpdateSurroundingText` | 应用周围文本变化 |
| `DoReleaseInput` | 处理按键抬起 |
| `OnClose` | 切走/失焦时的收尾 |
| `GetSubModeName` | 子模式名（中英/简繁） |

### 3.4 `DoInput` 的签名与返回码

```c
typedef INPUT_RETURN_VALUE (*FcitxIMDoInput)(void *arg, FcitxKeySym sym, unsigned int state);
```

`INPUT_RETURN_VALUE` 不是普通枚举，而是**位标志组合**，这是 Fcitx4 最独特的设计：

| 标志 | 含义 |
|---|---|
| `IRV_TO_PROCESS` (0) | 继续走后续阶段 |
| `IRV_FLAG_BLOCK_FOLLOWING_PROCESS` | 阻止后续处理 |
| `IRV_FLAG_FORWARD_KEY` | 把键转发给应用 |
| `IRV_FLAG_RESET_INPUT` | 重置输入状态 |
| `IRV_FLAG_PENDING_COMMIT_STRING` | 提交 output buffer 里的内容 |
| `IRV_FLAG_UPDATE_INPUT_WINDOW` | 刷新预编辑窗口 |
| `IRV_FLAG_UPDATE_CANDIDATE_WORDS` | 重新生成候选 |
| `IRV_FLAG_ENG` / `IRV_FLAG_PUNC` / `IRV_FLAG_DISPLAY_LAST` | 特殊语义 |

常用组合别名：

```c
IRV_DO_NOTHING      = BLOCK
IRV_DONOT_PROCESS   = FORWARD_KEY
IRV_COMMIT_STRING   = PENDING_COMMIT_STRING | DO_PHRASE_TIPS
IRV_DISPLAY_CANDWORDS = UPDATE_INPUT_WINDOW | UPDATE_CANDIDATE_WORDS
IRV_CLEAN           = RESET_INPUT
```

**理解方式**：返回值同时表达"键怎么处理 + UI 要不要刷新 + 候选要不要重算 + 要不要提交"。写 Fcitx4 插件时，返回错组合就会出现"候选不刷新""提交了两次""键被吃掉"。

### 3.5 按键的七阶段流水线

Fcitx4 头文件开头明确写了按键处理分七个阶段：

```
PreInput → DoInput → Update Candidates → Prev/Next Page → PostInput → Hotkey → Key Blocker
```

- 每个阶段都可能返回非 `IRV_TO_PROCESS` 的值，从而截断后续阶段；
- `DoInput` 是输入法引擎的主要入口；
- `Key Blocker` 决定"缓冲非空时哪些键不要转发给应用"；
- 如果走完全部阶段仍是 `IRV_TO_PROCESS`，按键被转发。

### 3.6 输入状态与候选

`FcitxInputState` 是全局输入状态（每个 `FcitxInstance` 一份，注意这是 Fcitx4 的结构性缺陷）：

| API | 作用 |
|---|---|
| `FcitxInstanceGetInputState(instance)` | 取输入状态 |
| `FcitxInputStateGetRawInputBuffer(input)` | 取原始输入缓冲（`char*`，容量 `MAX_USER_INPUT`） |
| `FcitxInputStateSetRawInputBufferSize(input, n)` | 设置有效长度 |
| `FcitxInputStateSetCursorPos(input, pos)` | 预编辑光标 |
| `FcitxInputStateSetShowCursor(input, bool)` | 是否显示光标 |
| `FcitxInputStateGetOutputString(input)` | 待提交字符串缓冲 |
| `FcitxInputStateGetCandidateList(input)` | 候选列表 |

候选列表 `FcitxCandidateWordList`：

| API/结构 | 作用 |
|---|---|
| `FcitxCandidateWordNewList()` / `FreeList()` | 创建/释放 |
| `FcitxCandidateWordAppend(list, word)` | 追加 |
| `FcitxCandidateWordInsert(list, word, pos)` | 插入 |
| `FcitxCandidateWordSetPage(list, page)` | 设当前页 |
| `FcitxCandidateWordSetFocus(list, index)` | 设高亮 |
| `FcitxCandidateWordChooseByIndex/TotalIndex()` | 选择并触发提交回调 |
| `FcitxCandidateWordSetChoose(list, keys)` | 自定义选择键 |
| `FcitxCandidateWordGoNextPage/PrevPage()` | 翻页 |

`FcitxCandidateWord` 的字段很直白：

```c
typedef struct _FcitxCandidateWord {
    char *strWord;                          /* 显示文本 */
    char *strExtra;                         /* 附加提示 */
    FcitxCandidateWordCommitCallback callback; /* 选中时回调 */
    FcitxMessageType wordType, extraType;
    void *owner;                            /* 来源标识 */
    void *priv;                             /* 私有数据/索引 */
} FcitxCandidateWord;
```

提交有两种方式：
1. 直接 `FcitxInstanceCommitString(instance, ic, text)`；
2. 把文本写进 `OutputString`，返回 `IRV_COMMIT_STRING`，由框架统一提交。

### 3.7 前端接口

Fcitx4 的 frontend 是一个 `FcitxFrontend` 回调表：

| 回调 | 作用 |
|---|---|
| `Create` | 初始化前端 |
| `CreateIC` / `DestroyIC` | 创建/销毁输入上下文 |
| `CheckIC` | 判断 IC 是否属于本前端 |
| `EnableIM` / `CloseIM` | 启用/关闭输入法 |
| `CommitString` | 把文本送进应用 |
| `ForwardKey` | 转发按键 |
| `SetWindowOffset` / `GetWindowRect` | 候选窗定位 |
| `UpdatePreedit` | 刷新预编辑 |
| `UpdateClientSideUI` | 客户端侧 UI |
| `GetSurroundingText` / `DeleteSurroundingText` | 周围文本 |

`FcitxInputContext` 是应用输入框的抽象，包含前端 ID、`CommitString`/`ForwardKey` 等函数指针、窗口矩形、app name、pid 等。

### 3.8 配置

Fcitx4 用 `.desc` 文件描述配置项，运行时由 `FcitxConfigData` 读写：

- `/usr/share/fcitx/configdesc/*.desc`
- `~/.config/fcitx/conf/*.config`
- 配置 UI 由 `fcitx-configtool` 提供

这套系统比较笨重：每个配置项要在 `.desc` 里声明类型、默认值、范围，再由 addon 读取。Fcitx5 换成了 C++ 模板 + INI，体验好很多。

### 3.9 UI addon

| UI | 说明 |
|---|---|
| `fcitx-ui-classic` | 默认，轻量，X11 自绘 |
| `fcitx-ui-gtk` | GTK 风格 |
| `fcitx-ui-qt` | Qt 风格 |
| `fcitx-kimpanel-ui` | KDE kimpanel 协议 |
| `fcitx-ui-light` | 极简 |

### 3.10 为什么 Fcitx4 被淘汰

1. **全局单例状态**：所有输入框共享一个 `FcitxInputState`，多窗口切换容易串状态。
2. **接口是函数指针 + 位标志**：难以扩展，错误组合难排查。
3. **X11 中心**：没有 Wayland 方案，而 Wayland 已成为默认。
4. **配置系统笨重**：`.desc` 维护成本高。
5. **C 语言 + 手动内存管理**：候选/字符串生命周期容易出错。
6. **维护者精力转向 Fcitx5**：4.2.9.x 之后基本只做兼容修复。

### 3.11 今天还会遇到 Fcitx4 的场景

- UOS 20、麒麟 V10 等政企桌面（系统自带 fcitx4）；
- Debian oldstable / 老 Ubuntu；
- 依赖 `fcitx-libpinyin`、`fcitx-rime`、`fcitx-sunpinyin`、`fcitx-cloudpinyin`、`fcitx-table` 的老方案；
- 某些老应用只兼容 XIM + fcitx4。

**策略**：新项目不要以 fcitx4 为主目标；如果必须支持，写一个**最小 addon 复用同一套引擎 C ABI**，不要为它单独做 UI。

### 3.12 最小 Fcitx4 addon 骨架

```c
#include <fcitx/fcitx.h>
#include <fcitx/ime.h>
#include <fcitx/instance.h>
#include <fcitx/candidate.h>

typedef struct _DemoIM {
    FcitxInstance *owner;
    char buf[MAX_USER_INPUT];
} DemoIM;

static void demo_reset(void *arg)
{
    DemoIM *im = arg;
    im->buf[0] = '\0';
    FcitxInputState *input = FcitxInstanceGetInputState(im->owner);
    FcitxInputStateSetRawInputBufferSize(input, 0);
    FcitxInputStateSetShowCursor(input, false);
}

static INPUT_RETURN_VALUE demo_do_input(void *arg, FcitxKeySym sym, unsigned int state)
{
    DemoIM *im = arg;
    FcitxInputState *input = FcitxInstanceGetInputState(im->owner);

    if (sym == FcitxKey_Return) {
        char *out = FcitxInputStateGetOutputString(input);
        strcpy(out, "你好");
        return IRV_COMMIT_STRING;
    }

    if (sym >= FcitxKey_a && sym <= FcitxKey_z) {
        size_t n = strlen(im->buf);
        if (n + 1 < sizeof(im->buf)) { im->buf[n] = (char)sym; im->buf[n+1] = '\0'; }
    } else {
        return IRV_TO_PROCESS;
    }

    char *raw = FcitxInputStateGetRawInputBuffer(input);
    snprintf(raw, MAX_USER_INPUT, "%s", im->buf);
    FcitxInputStateSetRawInputBufferSize(input, strlen(im->buf));
    FcitxInputStateSetCursorPos(input, strlen(im->buf));
    FcitxInputStateSetShowCursor(input, true);
    return IRV_DISPLAY_CANDWORDS;
}

static INPUT_RETURN_VALUE demo_get_cand(void *arg)
{
    DemoIM *im = arg;
    FcitxInputState *input = FcitxInstanceGetInputState(im->owner);
    FcitxCandidateWordList *list = FcitxInputStateGetCandidateList(input);

    FcitxCandidateWordReset(list);
    FcitxCandidateWordSetPageSize(list, 5);
    /* 构造 FcitxCandidateWord（strWord / callback / priv），再
       FcitxCandidateWordAppend(list, &word)；
       字符串所有权按目标版本的 API 约定处理，建议以 fcitx-pinyin 源码为模板。 */
    return IRV_DISPLAY_CANDWORDS;
}

static void *demo_create(FcitxInstance *instance)
{
    DemoIM *im = calloc(1, sizeof(DemoIM));
    im->owner = instance;

    FcitxIMIFace iface = {0};
    iface.ResetIM    = demo_reset;
    iface.DoInput    = demo_do_input;
    iface.GetCandWords = demo_get_cand;

    FcitxInstanceRegisterIMv2(instance, im, "demo", "Demo", "demo",
                              iface, 5, "zh_CN");
    return im;
}

FCITX_DEFINE_PLUGIN(demo, ime, FcitxIMClass2);
```

> 说明：末尾的导出宏按本机 `fcitx.h` 的 `FCITX_DEFINE_PLUGIN(name, category, type)` 写法；不同 fcitx4 小版本的导出符号约定略有差异，移植时以目标版本的 `addon.c` 加载逻辑为准。候选构造的具体 API 随版本变化，建议以 `fcitx-pinyin` 源码为模板。

---

## 4. Fcitx5 详解

### 4.1 核心对象

| 对象 | 头文件 | 职责 |
|---|---|---|
| `Instance` | `fcitx/instance.h` | 全局实例：事件循环、addon 管理器、输入法管理器、UI 管理器 |
| `AddonManager` | `fcitx/addonmanager.h` | 加载/卸载 addon，解析依赖 |
| `AddonInstance` | `fcitx/addoninstance.h` | addon 基类 |
| `AddonFactory` | `fcitx/addonfactory.h` | 创建 addon 实例 |
| `InputContext` | `fcitx/inputcontext.h` | 一个输入框的抽象（相当于 Fcitx4 的 IC） |
| `InputContextManager` | `fcitx/inputcontextmanager.h` | 管理所有 IC、焦点、属性 |
| `InputContextProperty` | `fcitx/inputcontextproperty.h` | 每个 IC 一份的自定义状态 |
| `InputMethodEngine` | `fcitx/inputmethodengine.h` | 输入法引擎基类 |
| `InputMethodEntry` | `fcitx/inputmethodentry.h` | 一个输入法条目（名字/语言/图标） |
| `EventLoop` / `EventSource` | `fcitx-utils/event.h` | 异步事件 |
| `InputPanel` | `fcitx/inputpanel.h` | 预编辑/候选/辅助文本的数据模型 |

### 4.2 运行时结构

```
Instance（fcitx5 进程）
├── EventLoop
├── AddonManager
│   ├── Frontend addons: wayland / waylandim / xcb / xim / dbusfrontend / ibusfrontend
│   ├── InputMethod addons: pinyin / rime / table / ...
│   ├── UI addons: classicui / kimpanel
│   └── Module addons: clipboard / notifications / quickphrase / spell
├── InputContextManager
│   └── InputContext（每个客户端输入框）
│       ├── InputContextProperty（每个引擎挂自己的状态）
│       └── InputPanel（预编辑 + 候选）
└── UserInterfaceManager（状态栏 Action/Menu）
```

### 4.3 addon 元数据

安装位置：

| 类型 | 系统 | 用户 |
|---|---|---|
| addon 描述 | `/usr/share/fcitx5/addon/*.conf` | `~/.local/share/fcitx5/addon/` |
| 输入法条目 | `/usr/share/fcitx5/inputmethod/*.conf` | `~/.local/share/fcitx5/inputmethod/` |
| addon 库 | `/<libdir>/fcitx5/lib<name>.so` | 同上 |
| 配置 | `~/.config/fcitx5/conf/<addon>.conf` | — |

一个输入法 addon 需要**两个** conf：

```ini
# demo.conf —— 声明 addon 本身
[Addon]
Name=Demo
Name[zh_CN]=示例输入法
Category=InputMethod
Type=SharedLibrary
Library=libdemo
OnDemand=True
Configurable=False

[Addon/Dependencies]
0=core:5.0.20
```

```ini
# demo-inputmethod.conf —— 声明输入法条目
[InputMethod]
Name=Demo
Name[zh_CN]=示例拼音
Icon=demo
Label=示
LangCode=zh_CN
Addon=demo
Configurable=False
```

| 字段 | 关键点 |
|---|---|
| `Category` | `InputMethod` / `Frontend` / `UI` / `Module` |
| `Type` | 通常是 `SharedLibrary`；也有 `StaticLibrary`、`Exec` |
| `Library` | 可写 `demo` 或 `libdemo`，fcitx5 两种都能解析（本地 wind/msime 都写 `libxxx`） |
| `OnDemand` | `True` 表示首次使用时才加载，省内存 |
| `Dependencies` | `core:5.0.20` 表示最低核心版本 |
| `Configurable` | 是否在配置工具里显示"配置"按钮 |
| `Addon`（IM conf） | **必须与 addon 文件名/Name 一致**，否则切不过去 |
| `UniqueName`（IM conf） | 可选；不写时用 addon 名 |

### 4.4 引擎接口的版本

| 基类 | 何时用 |
|---|---|
| `InputMethodEngine` | 最少接口：keyEvent / activate / deactivate / reset |
| `InputMethodEngineV2` | 需要 `listInputMethods()`、子模式（subMode）时 |
| `InputMethodEngineV3` | 更新的扩展接口（具体新增项随版本演进，Sime 等现代 addon 在用）；编译前确认目标版本是否提供 |

**最小实现只需要四个虚函数**：

```cpp
void keyEvent(const InputMethodEntry &entry, KeyEvent &event) override;
void activate(const InputMethodEntry &entry, InputContextEvent &event) override;
void deactivate(const InputMethodEntry &entry, InputContextEvent &event) override;
void reset(const InputMethodEntry &entry, InputContextEvent &event) override;
```

如果要让输入法出现在列表中，**必须实现**：

```cpp
std::vector<InputMethodEntry> listInputMethods() override {
    return { InputMethodEntry("demo", "Demo", "zh_CN", "demo") };
}
```

`InputMethodEntry` 的第 4 个参数是 addon 名，必须与 `demo.conf` 一致。

### 4.5 按键事件

```cpp
void keyEvent(const InputMethodEntry &entry, KeyEvent &event) override {
    if (event.isRelease()) { /* 处理抬起，或直接 return */ }

    const Key &key = event.rawKey();     // 原始键（未做任何转换）
    // event.key() 是经过状态/布局处理的键
    auto *ic = event.inputContext();

    if (key.sym() == FcitxKey_space &&
        !key.states().testAny(fcitx::KeyStates{
            fcitx::KeyState::Ctrl, fcitx::KeyState::Alt,
            fcitx::KeyState::Super})) {
        ic->commitString("你好");
        event.filterAndAccept();         // 吃掉这个键
        return;
    }
    event.forward();                     // 不处理，交给应用
}
```

消费语义：

| 调用 | 含义 |
|---|---|
| `event.accept()` | 我处理了，但允许其他处理器再看到 |
| `event.filter()` | 我处理了，后续过滤器不再看到 |
| `event.filterAndAccept()` | 我处理了，最终消费（最常用） |
| `event.forward()` | 不处理，向下/向应用转发 |
| `event.ignored()` | 明确忽略 |

**注意**：
- `keyEvent` 在 fcitx5 主线程执行，**绝不能阻塞**；耗时工作丢到线程/事件源，结果再回主线程。
- `rawKey()` 是硬件键，`key()` 会考虑布局/状态；拼音引擎通常用 `rawKey().sym()` 拿 X11 keysym。
- 修饰键判断用 `Key::states()` / `KeyState::Ctrl` 等，不要自己拼位掩码。

### 4.6 输出与 InputPanel

```cpp
auto *ic = event.inputContext();

// 提交
ic->commitString("你好");

// 预编辑
panel.setPreedit(Text("ni hao"));
panel.setClientPreedit(Text("ni hao"));   // 让客户端自己画（行内）
panel.setAuxiliaryText(Text("提示"));

// 候选
auto list = std::make_unique<CommonCandidateList>();
list->setPageSize(9);
list->append(CandidateWord(Text("你好"), ...));
panel.setCandidateList(std::move(list));

// 通知框架重绘
ic->updateUserInterface(UserInterfaceComponent::InputPanel);
```

| API | 说明 |
|---|---|
| `InputContext::commitString(const std::string&)` | 上屏 |
| `InputContext::inputPanel()` | 取面板模型 |
| `InputPanel::setPreedit(Text)` | 引擎侧预编辑（候选窗上方） |
| `InputPanel::setClientPreedit(Text)` | 客户端侧预编辑（应用内行内） |
| `InputPanel::setAuxiliaryText(Text)` | 辅助文本 |
| `InputPanel::setCandidateList(unique_ptr<CandidateList>)` | 候选 |
| `InputPanel::reset()` | 清空所有 |
| `InputContext::updateUserInterface(component)` | 触发重绘 |
| `InputContext::cursorRect()` | 光标矩形（用于候选窗定位） |
| `InputContext::surroundingText()` | 周围文本 |
| `InputContext::deleteSurroundingText(offset, size)` | 删除周围文本 |

**候选列表类型**：

| 类型 | 用途 |
|---|---|
| `CommonCandidateList` | 标准候选，支持分页/高亮/选择回调 |
| `DisplayOnlyCandidateList` | 只显示，点击由自己处理（鲸鱼用它做外部面板） |
| 自定义 `CandidateList`/`CandidateWord` | 完全控制渲染与交互 |

`Text` 支持属性（下划线、颜色、高亮），属性位置用**字节偏移**（`TextAttribute`），这点与 IBus 的字符偏移不同，跨框架移植时最容易错。

### 4.7 InputContext 与状态

每个输入框一个 `InputContext`，通过 `InputContextManager` 管理。引擎的私有状态放在 `InputContextProperty`：

```cpp
class DemoState : public InputContextProperty {
public:
    std::string buffer;
};

// 在 addon 初始化时注册
instance->inputContextManager().registerProperty("demo-state",
    [](InputContext &) { return new DemoState; });

// 取用
auto *state = ic->propertyFor(&factory_);
```

生命周期语义：

| 回调 | 何时 |
|---|---|
| 构造 | 该 IC 第一次取这个 property |
| `reset` | 输入法 reset/切换/失焦时由框架调用 |
| `copy` | IC 状态复制（少见） |
| 析构 | IC 销毁 |

**常见错误**：把状态放在引擎对象的成员里，而不是 property 里 → 多窗口共享状态、切换窗口串词。鲸鱼早期的 `unordered_map<InputContext*, Handle>` 是另一种可行做法（手动管理生命周期），但要注意 IC 销毁时清理。

### 4.8 能力位与隐私

`InputContext::capabilityFlags()` 返回客户端能力：

| 能力 | 含义 | 对引擎的影响 |
|---|---|---|
| `Preedit` | 支持预编辑 | 可以 `setClientPreedit` |
| `ClientSideInputPanel` | 客户端自己画候选 | 可能要抑制原生候选窗 |
| `SurroundingText` | 提供周围文本 | 可用于上下文预测 |
| `Password` / `Sensitive` | 密码/敏感输入 | **禁止记录、联想、云请求** |
| `RelativeRect` | 支持相对坐标 | 候选窗定位 |
| `Disable` | 禁用输入法 | 直接透传 |
| `NoSpellCheck` | 不要拼写检查 | 英文模式用 |
| `Digit` / `Number` / `Dialable` | 数字/拨号场景 | 调整候选策略 |

在 `activate` 时检查一次，并在 `keyEvent` 里按需复查（能力可能变化）。

### 4.9 配置系统

Fcitx5 用 `fcitx-config`：

```cpp
class DemoConfig : public Configuration {
public:
    Option<std::string> greeting{this, "Greeting", "问候语", "你好"};
    Option<int, IntConstrain> pageSize{this, "PageSize", "每页候选数", 9,
                                       IntConstrain(1, 20)};
    Option<bool> enableAi{this, "EnableAI", "启用 AI 候选", false};
};
```

- 声明式：`Option<T>` 自带默认值、约束、i18n 名称；
- 读取：`config_.load(rawConfig)`；
- 保存：`safeSaveAsIni(config_, "conf/demo.conf")`；
- 配置工具：实现 `Configurable`（`getConfig`/`setConfig`）后，`fcitx5-configtool` 自动生成页面；
- 配置文件：`~/.config/fcitx5/conf/demo.conf`（INI）。

枚举用 `FCITX_CONFIG_ENUM_NAME_WITH_I18N` 声明显示名（Sime 的 `PreeditMode` 就是例子）。

### 4.10 事件循环与异步

| 工具 | 用途 |
|---|---|
| `instance->eventLoop()` | 主事件循环 |
| `EventSource` | 可读/可写/定时事件的基类 |
| `EventSourceTime` | 定时器（`setTime`/`setInterval`） |
| `EventDispatcher` | 把任务投递回主线程 |
| `instance->watchEvent(...)` | 监听 fd/信号 |

**铁律**：
- `keyEvent` 里不做 IO、不做模型推理、不等待锁；
- 后台线程算完，用 `EventDispatcher::schedule` 回主线程改 UI；
- 定时器（如鲸鱼的 50ms 轮询）用于消费异步结果，不要在定时器里做重活。

### 4.11 Wayland 与 X11

#### X11

- `xcb` frontend：实现 XIM 协议，服务老应用；
- `dbusfrontend`：给 fcitx5-qt/gtk immodule 用，应用通过 DBus 调输入法；
- XWayland 应用走 XIM 或 immodule，取决于应用类型。

#### Wayland

- `wayland` frontend：以输入法身份接入合成器，使用 `input-method-unstable-v1/v2`，通过 `virtual-keyboard` 提交按键/文本；
- `waylandim` frontend：面向支持输入法协议的应用/合成器，处理候选与预编辑；
- 候选窗：通常由 UI addon 用 layer-shell 画，或由合成器提供的 popup；
- 光标定位：`cursorRect()` 在 Wayland 下可能是相对坐标，需要 `RelativeRect` 能力或合成器支持。

**现实约束**：

| 桌面 | Fcitx5 可用性 |
|---|---|
| KDE Plasma | 原生支持，体验最好（input-method-v2 + 自家 applet） |
| Sway / Hyprland / wlroots | 支持，需配置环境变量与启动 |
| GNOME Wayland | **不支持第三方输入法协议**，实践中用 IBus 或 X11 会话 |
| Xfce / LXQt | X11 下可用；Wayland 支持取决于合成器 |
| XWayland 应用 | 可用（走 XIM/immodule） |

#### 环境变量（X11/兼容场景）

```bash
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx
export XMODIFIERS=@im=fcitx
export SDL_IM_MODULE=fcitx
export GLFW_IM_MODULE=ibus   # 某些 GLFW 应用只认 ibus
```

Wayland 下不要设 `GTK_IM_MODULE` 等（会让 GTK 走 DBus 而不是 text-input），具体按桌面文档来。

---

## 5. 生态全景

### 5.1 引擎

| 引擎 | 说明 |
|---|---|
| `fcitx5-chinese-addons` | 官方中文套件：拼音（基于 libpinyin）、双拼、五笔、仓颉、表码 |
| `fcitx5-rime` | Rime 前端，方案/词库最丰富（雾凇、白霜、万象都跑在它上面） |
| `fcitx5-table-extra` | 额外表码（五笔98、郑码、笔画等） |
| `fcitx5-mozc` | 日语（Mozc） |
| `fcitx5-anthy` | 日语（Anthy） |
| `fcitx5-kkc` | 日语假名 |
| `fcitx5-skk` | 日语 SKK |
| `fcitx5-hangul` | 韩语 |
| `fcitx5-unikey` | 越南语 |
| `fcitx5-chewing` | 注音 |
| `fcitx5-keyman` | Keyman 布局 |
| `fcitx5-m17n` | m17n 多语言 |
| 第三方 | 鲸鱼（libpinyin 封装）、Sime、WindInput、水杉（均为本地项目） |

### 5.2 前端/UI/工具

| 包 | 作用 |
|---|---|
| `fcitx5-qt` | Qt immodule + 配置模块 |
| `fcitx5-gtk` | GTK immodule |
| `fcitx5-configtool` | 配置工具 |
| `kcm-fcitx5` | KDE 系统设置模块 |
| `fcitx5-classicui` | 经典候选窗（内置在 core 包） |
| `fcitx5-kimpanel` | KDE kimpanel 协议 |
| `fcitx5-material-color` / `fcitx5-nord` | 主题 |
| `fcitx5-remote` | 命令行控制（切换/启用/退出） |
| `fcitx5-diagnose` | 诊断脚本 |

### 5.3 主题与外观

- 候选窗由 UI addon 画，主题是 `~/.local/share/fcitx5/themes/<name>/theme.conf` + 图片；
- `classicui` 支持字体、圆角、阴影、背景图、横竖排；
- KDE 下可用 kimpanel 让 Plasma 画候选，外观跟随系统。

### 5.4 发行版现状

| 发行版/桌面 | 默认/推荐 |
|---|---|
| Arch / Manjaro | 用户自选，中文社区多 Fcitx5 |
| Debian / Ubuntu | 可选，`im-config` 切换 |
| Fedora | 默认 IBus，可装 Fcitx5 |
| KDE Plasma | Fcitx5 体验最好 |
| UOS / 麒麟 | 老版本 fcitx4，新版本逐步迁 Fcitx5 |
| GNOME | IBus |

---

## 6. Fcitx4 → Fcitx5 迁移

### 6.1 概念对照

| Fcitx4 | Fcitx5 |
|---|---|
| `FcitxInstance` | `Instance` |
| `FcitxIM` / `FcitxIMIFace` | `InputMethodEngine` 虚类 |
| `FcitxInputState`（全局） | `InputContext` + `InputContextProperty`（每 IC） |
| `FcitxCandidateWordList` | `CandidateList` / `CommonCandidateList` |
| `FcitxCandidateWord` | `CandidateWord` |
| `FcitxInputContext` | `InputContext` |
| `INPUT_RETURN_VALUE` 位标志 | `KeyEvent` 的 accept/filter/forward |
| `FcitxConfigData` + `.desc` | `Configuration` + `Option<T>` |
| `FcitxAddon` 四类 | `AddonInstance` + `Category` |
| `.config` / `.desc` | `.conf`（INI） |
| X11 only | Wayland + X11 |

### 6.2 API 迁移要点

1. **状态从全局搬到 property**：这是最大的结构性变化，不能直接翻译。
2. **返回值语义换成事件消费**：`IRV_COMMIT_STRING` → `commitString()` + `filterAndAccept()`；`IRV_DISPLAY_CANDWORDS` → 设置 `InputPanel` 后 `updateUserInterface()`。
3. **候选对象换成 `CandidateList`**：分页、选择回调、高亮都由列表对象管理。
4. **配置换成 `Option<T>`**：不再写 `.desc`，但要处理 INI 读写与默认值。
5. **前端不再由你实现**：Fcitx5 自带 xcb/wayland/dbus/xim，你只写引擎。
6. **UI 不再由你实现**：设置 `InputPanel` 即可；要自定义皮肤用 UI addon。

### 6.3 用户数据迁移

- 词库/用户词：`~/.config/fcitx/pinyin/` → `~/.local/share/fcitx5/pinyin/`（不同引擎路径不同）；
- 配置：`~/.config/fcitx/conf/*.config` → `~/.config/fcitx5/conf/*.conf`（格式不同，不能直接复制）；
- 输入法列表：`~/.config/fcitx/profile` → `~/.config/fcitx5/profile`（格式不同）；
- **建议**：提供一个"导入旧词库"的一次性工具，而不是让用户手工搬。

---

## 7. 开发实战：最小 Fcitx5 addon

### 7.1 代码

```cpp
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

#include <memory>
#include <string>
#include <vector>

namespace demo {

class DemoState : public fcitx::InputContextProperty {
public:
    std::string buffer;
};

class DemoEngine : public fcitx::InputMethodEngineV2 {
public:
    explicit DemoEngine(fcitx::Instance *instance)
        : instance_(instance),
          factory_([](fcitx::InputContext &) { return new DemoState; }) {
        instance_->inputContextManager().registerProperty("demo-state", &factory_);
    }

    void keyEvent(const fcitx::InputMethodEntry &,
                  fcitx::KeyEvent &event) override {
        auto *ic = event.inputContext();
        auto *state = ic->propertyFor(&factory_);

        if (event.isRelease()) { event.forward(); return; }

        const auto sym = event.rawKey().sym();
        if (sym == FcitxKey_BackSpace && !state->buffer.empty()) {
            state->buffer.pop_back();
            render(ic, state);
            event.filterAndAccept();
            return;
        }
        if (sym == FcitxKey_Return && !state->buffer.empty()) {
            ic->commitString("你好");     // 示例：真实实现应查词
            state->buffer.clear();
            render(ic, state);
            event.filterAndAccept();
            return;
        }
        if (sym >= FcitxKey_a && sym <= FcitxKey_z) {
            state->buffer.push_back(static_cast<char>(sym));
            render(ic, state);
            event.filterAndAccept();
            return;
        }
        event.forward();
    }

    void activate(const fcitx::InputMethodEntry &,
                  fcitx::InputContextEvent &) override {}

    void deactivate(const fcitx::InputMethodEntry &,
                    fcitx::InputContextEvent &) override {}

    void reset(const fcitx::InputMethodEntry &,
               fcitx::InputContextEvent &event) override {
        auto *ic = event.inputContext();
        ic->propertyFor(&factory_)->buffer.clear();
        ic->inputPanel().reset();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

    std::vector<fcitx::InputMethodEntry> listInputMethods() override {
        return {fcitx::InputMethodEntry("demo", "Demo", "zh_CN", "demo")};
    }

private:
    void render(fcitx::InputContext *ic, DemoState *state) {
        auto &panel = ic->inputPanel();
        if (state->buffer.empty()) {
            panel.reset();
        } else {
            panel.setPreedit(fcitx::Text(state->buffer));
            auto list = std::make_unique<fcitx::CommonCandidateList>();
            list->setPageSize(9);
            list->append<fcitx::CandidateWord>(fcitx::Text("你好"));
            list->append<fcitx::CandidateWord>(fcitx::Text("你号"));
            panel.setCandidateList(std::move(list));
        }
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

    fcitx::Instance *instance_;
    fcitx::FactoryFor<DemoState> factory_;   // 见下方说明：工厂需按版本适配
};

class DemoFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new DemoEngine(manager->instance());
    }
};

} // namespace demo

FCITX_ADDON_FACTORY(demo::DemoFactory);
```

> 关于 `FactoryFor<T>`：这是 Fcitx5 的标准写法——构造函数里用 lambda 初始化工厂，再 `registerProperty(name, &factory_)`，之后用 `ic->propertyFor(&factory_)` 取状态（本地 Sime 的实现就是如此）。部分较新版本也支持 `ic->propertyFor<T>()`，以目标版本的 `inputcontextproperty.h` 为准。

### 7.2 CMake

```cmake
cmake_minimum_required(VERSION 3.16)
project(fcitx5-demo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Fcitx5Core 5.0.20 REQUIRED)
find_package(Fcitx5Utils REQUIRED)

add_library(demo MODULE demo.cpp)
target_link_libraries(demo PRIVATE Fcitx5::Core Fcitx5::Utils)

install(TARGETS demo LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/fcitx5)
install(FILES demo.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/addon)
install(FILES demo-inputmethod.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx5/inputmethod)
```

产物是 `libdemo.so`，与 `Library=libdemo` 对应。

### 7.3 调试

```bash
# 前台启动，看日志
fcitx5 -d --verbose

# 只加载指定 addon（排障）
fcitx5 -d --enable demo

# 状态/切换
fcitx5-remote -o          # 打开
fcitx5-remote -c          # 关闭
fcitx5-remote -t          # 切换
fcitx5-remote -e          # 退出
fcitx5-remote -n          # 当前 IM 名

# 全面诊断
fcitx5-diagnose

# 看 addon 是否加载
fcitx5-remote -m 2>/dev/null; ls ~/.local/share/fcitx5/addon/
busctl --user list | grep fcitx

# gdb 进 addon（因为它就在 fcitx5 进程里）
gdb --args fcitx5 -d
```

日志宏：`FCITX_INFO()`、`FCITX_DEBUG()`、`FCITX_WARN()`；日志级别用 `--verbose` 或环境变量控制。

---

## 8. 常见坑

### Fcitx5

1. **addon 名与 IM conf 的 `Addon=` 不一致** → 输入法列表里有名字但切不过去。
2. **忘了 `listInputMethods()`** → addon 加载了但没有任何输入法条目。
3. **把状态放引擎成员而不是 property** → 多个输入框串词。
4. **在 `keyEvent` 里做重活** → 整个桌面输入卡顿（主线程被占）。
5. **候选对象生命周期** → `setCandidateList(std::move(list))` 后不要再碰。
6. **`TextAttribute` 用字符偏移** → Fcitx5 用字节偏移，中文会错位。
7. **Wayland 下自绘窗口** → 坐标/层级/焦点都可能不对，优先用 `InputPanel` + UI addon。
8. **GNOME 上装 Fcitx5** → Wayland 会话基本不可用，需换 IBus 或 X11。
9. **密码框联想** → 必须检查 `CapabilityFlag::Password`/`Sensitive`。
10. **addon 崩了** → 整个 fcitx5 挂掉，所有应用失去输入法；关键路径要加 try/catch 与兜底。
11. **多版本插件冲突** → 同名 addon 的 `STB_GNU_UNIQUE` 符号可能被合并，msime 用隐藏符号 + 版本化库名解决；普通项目不用太担心，但要注意。
12. **`OnDemand=True` 下初始化过重** → 首次切换输入法卡顿；大词库要异步加载。
13. **XWayland 应用没输入法** → 检查 `xcb`/`xim` addon 是否启用、`XMODIFIERS` 是否设置。
14. **配置改了不生效** → `setConfig` 后要 `applyConfig()`，并 `safeSaveAsIni`；配置工具用 `fcitx5-configtool` 或 `kcm-fcitx5`。

### Fcitx4

1. `INPUT_RETURN_VALUE` 组合错 → 候选不刷新/重复提交。
2. 全局 `FcitxInputState` → 多窗口串状态（这是框架缺陷，只能尽量规避）。
3. 候选对象内存泄漏 → `FcitxCandidateWord` 的 `strWord` 所有权要按 API 约定处理。
4. `.desc` 配置项与代码不一致 → 配置工具显示错误默认值。
5. 没有 Wayland 支持 → 不要在新 Wayland 桌面上推 fcitx4。
6. addon 依赖加载顺序 → 用 `Depend` 声明，否则 `Create` 时拿不到依赖。

---

## 9. 选型建议

### 9.1 Fcitx5 vs IBus

| 你的情况 | 建议 |
|---|---|
| GNOME / Fedora / Ubuntu 默认 | IBus |
| KDE Plasma | Fcitx5 |
| Sway / Hyprland / wlroots | Fcitx5 |
| 需要自绘候选窗/皮肤 | Fcitx5 |
| 需要引擎崩溃不影响桌面 | IBus |
| 引擎要用 C++ 且追求低延迟 | Fcitx5 |
| 引擎要用 Python 快速原型 | IBus（PyGObject 绑定成熟） |
| 要同时支持三框架 | 共享 C ABI 内核 + 三个薄适配器（见《输入法项目汇总与开发参考》） |

### 9.2 Fcitx4 的定位

- **只作为 legacy 兼容目标**：UOS/麒麟老系统、Debian oldstable；
- 新功能不要只在 fcitx4 上做；
- 如果必须支持，做一个最小 addon 复用同一引擎，不要维护两套 UI/状态机；
- 打包时用 `Conflicts/Replaces/Provides` 处理 fcitx4 与 fcitx5 的共存关系。

---

## 10. 速查表

### 路径

| 项 | Fcitx5 | Fcitx4 |
|---|---|---|
| addon 描述 | `/usr/share/fcitx5/addon/` | `/usr/share/fcitx/addon/` |
| 输入法条目 | `/usr/share/fcitx5/inputmethod/` | `/usr/share/fcitx/inputmethod/` |
| addon 库 | `/usr/lib/<arch>/fcitx5/` | `/usr/lib/<arch>/fcitx/` |
| 全局配置 | `~/.config/fcitx5/config` | `~/.config/fcitx/config` |
| addon 配置 | `~/.config/fcitx5/conf/<name>.conf` | `~/.config/fcitx/conf/<name>.config` |
| 用户数据 | `~/.local/share/fcitx5/` | `~/.config/fcitx/`、`~/.local/share/fcitx/` |
| Rime 用户目录 | `~/.local/share/fcitx5/rime` | `~/.config/fcitx/rime` |

### 命令

| 命令 | 作用 |
|---|---|
| `fcitx5 -d` | 后台启动 |
| `fcitx5 -r` | 替换已有实例 |
| `fcitx5 -d --verbose` | 前台 + 详细日志 |
| `fcitx5-remote -o/-c/-t/-e` | 开/关/切换/退出 |
| `fcitx5-remote -n` | 当前输入法名 |
| `fcitx5-configtool` | 图形配置 |
| `fcitx5-diagnose` | 诊断 |
| `busctl --user list`（配合 grep fcitx） | 看 D-Bus 服务 |
| `fcitx-configtool` | Fcitx4 配置 |

### 关键头文件（Fcitx5）

| 头文件 | 内容 |
|---|---|
| `fcitx/inputmethodengine.h` | `InputMethodEngine` / V2 / V3 |
| `fcitx/inputcontext.h` | `InputContext`、commit、cursor、capability |
| `fcitx/inputcontextproperty.h` | 每 IC 状态 |
| `fcitx/inputpanel.h` | 预编辑/候选模型 |
| `fcitx/candidatelist.h` | `CandidateList` / `CandidateWord` |
| `fcitx/addonfactory.h` | `AddonFactory`、`FCITX_ADDON_FACTORY` |
| `fcitx/addonmanager.h` | 加载/依赖 |
| `fcitx/instance.h` | 全局实例、事件循环 |
| `fcitx-config/configuration.h` | `Configuration` / `Option<T>` |
| `fcitx-utils/key.h` | `Key` / `KeyState` |
| `fcitx-utils/event.h` | `EventSource` / `EventSourceTime` |

---

## 11. 一句话总结

**Fcitx4 是"位标志 + 全局状态"的 C 时代框架，Fcitx5 是"虚类 + 每输入框状态 + 事件循环"的现代 C++ 框架。写新输入法只考虑 Fcitx5；fcitx4 只做最小兼容。跨框架复用的正确边界是"自有引擎 + C ABI"，而不是把 Fcitx5 的代码硬搬到 IBus。**

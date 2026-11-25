// Fcitx4 addon 的加载入口。
//
// fcitx4 按 addon 配置里的 IMRegisterMethod=Self 推导导出符号：addon 名 `fcitx-windinput`
// → `fcitx_windinput_ime2`（对应内置 fcitx-keyboard 的 fcitx_keyboard_ime2）。
// 结构由 fcitx4 dlopen/dlsym 后取用，必须保持 C 链接、不能被 C++ 改名。
#include "WindEngine4.h"

extern "C" {

// fcitx4 的 FcitxCheckABIVersion 先 dlsym `<addon 名>_ABI_VERSION`，取不到就拒绝加载。
// 名字与 addon 配置 Name=fcitx-windinput 对应（'-' → '_'）。
FCITX_EXPORT_API int fcitx_windinput_ABI_VERSION = FCITX_ABI_VERSION;

FcitxIMClass2 fcitx_windinput_ime2 = {
    &windlinux::WindEngine4::create,
    &windlinux::WindEngine4::destroy,
    nullptr, // ReloadConfig
    nullptr, // padding1
    nullptr, // padding2
    nullptr, // padding3
    nullptr, // padding4
    nullptr, // padding5
};

} // extern "C"

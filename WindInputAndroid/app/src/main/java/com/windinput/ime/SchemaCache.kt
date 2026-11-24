package com.windinput.ime

import uniffi.wind_android.FfiSchemaEntry

/**
 * 已装方案的内存缓存（由 RustBridge 在启动时填充）。
 * 设置页读取它展示/切换；没有打开过键盘时为空。
 */
object SchemaCache {
    @Volatile
    var entries: List<FfiSchemaEntry> = emptyList()
}

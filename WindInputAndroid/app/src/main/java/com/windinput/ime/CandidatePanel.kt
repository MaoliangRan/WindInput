package com.windinput.ime

import android.app.AlertDialog

/**
 * 候选展开面板（「更多」）：一次性拉最多 200 条，点选走上屏链路。
 * 候选池扩容（`rebuilt`）时以本次返回的窗口为准（offset=0）。
 */
object CandidatePanel {

    fun show(service: WindImeService, bridge: RustBridge) {
        bridge.candidates(0, 200) { win ->
            if (win.items.isEmpty()) {
                android.widget.Toast.makeText(
                    service,
                    "暂无候选",
                    android.widget.Toast.LENGTH_SHORT,
                ).show()
                return@candidates
            }
            val labels = win.items.mapIndexed { i, item ->
                val comment = win.comments.getOrNull(i)?.takeIf { it.isNotBlank() }
                if (comment != null) "$item  $comment" else item
            }.toTypedArray()

            AlertDialog.Builder(service)
                .setTitle("候选")
                .setItems(labels) { _, which ->
                    bridge.selectCandidate(win.offset.toInt() + which)
                }
                .setNegativeButton("关闭", null)
                .show()
        }
    }
}

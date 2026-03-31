package com.quest.bridge

import android.app.Activity
import android.os.Bundle
import android.view.WindowManager

/**
 * Minimal Kotlin glue — the real work happens in native C++ via JNI.
 */
class MainActivity : Activity() {

    companion object {
        init {
            System.loadLibrary("quest_ros2_bridge")
        }
    }

    private external fun nativeOnCreate()
    private external fun nativeOnDestroy()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        nativeOnCreate()
    }

    override fun onDestroy() {
        nativeOnDestroy()
        super.onDestroy()
    }
}

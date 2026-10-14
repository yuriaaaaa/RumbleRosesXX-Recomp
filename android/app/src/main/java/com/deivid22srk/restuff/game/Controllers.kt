package com.deivid22srk.restuff.game

import android.content.Context
import android.hardware.input.InputManager
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext

/** A physical controller as the engine will number it ("Controller 1" = pad1). */
data class ConnectedController(val deviceId: Int, val name: String)

/**
 * Lists controllers the same way the engine sees them, so "Controller N" in
 * Settings is the engine's pad N:
 *  - SDLControllerManager.isDeviceSDLJoystick picks the devices,
 *  - the SDL input driver drops "... Consumer Control" media-key devices,
 *  - the engine numbers pads in connection order, and Android hands out
 *    increasing device ids, so sorting by id matches.
 */
object Controllers {

    fun list(): List<ConnectedController> =
        InputDevice.getDeviceIds()
            .filter { it >= 0 }
            .mapNotNull { id -> InputDevice.getDevice(id)?.takeIf { isController(it) } }
            .sortedBy { it.id }
            .map { ConnectedController(it.id, it.name ?: "Controller") }

    fun isController(device: InputDevice): Boolean {
        if (device.isVirtual) return false
        if (device.name?.contains("Consumer Control") == true) return false
        val sources = device.sources
        return (sources and InputDevice.SOURCE_CLASS_JOYSTICK) != 0 ||
            (sources and InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD ||
            (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
    }
}

/** Connected controllers, refreshed as they connect and disconnect. */
@Composable
fun rememberConnectedControllers(): List<ConnectedController> {
    val context = LocalContext.current
    var controllers by remember { mutableStateOf(Controllers.list()) }
    DisposableEffect(context) {
        val manager = context.getSystemService(Context.INPUT_SERVICE) as InputManager
        val listener = object : InputManager.InputDeviceListener {
            override fun onInputDeviceAdded(deviceId: Int) { controllers = Controllers.list() }
            override fun onInputDeviceRemoved(deviceId: Int) { controllers = Controllers.list() }
            override fun onInputDeviceChanged(deviceId: Int) { controllers = Controllers.list() }
        }
        manager.registerInputDeviceListener(listener, Handler(Looper.getMainLooper()))
        onDispose { manager.unregisterInputDeviceListener(listener) }
    }
    return controllers
}

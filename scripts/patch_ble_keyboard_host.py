"""Compile a build-local SDK copy compatible with pinned NimBLE-Arduino 2.3.8."""

from pathlib import Path

Import("env")  # noqa: F821

METHOD = """  uint32_t onPassKeyDisplay(NimBLEConnInfo&) override {
    const uint32_t passkey = NimBLEDevice::getSecurityPasskey();
    self().onPairingPasskey(passkey);
#if FREEINK_BLE_HID_SCAN_DEBUG
    Serial.printf("[BleHid] pairing passkey: %06lu\\n", static_cast<unsigned long>(passkey));
#endif
    return passkey;
  }
"""


def write_compatible_source(target, source, env):
    original = Path(source[0].get_abspath()).read_text(encoding="utf-8")
    already_compatible = original.count(METHOD) == 0 and "onPassKeyDisplay" not in original
    if original.count(METHOD) != 1 and not already_compatible:
        raise RuntimeError("Unsupported BleKeyboardHost passkey callback; check the SDK/NimBLE pins")
    output = Path(target[0].get_abspath())
    output.parent.mkdir(parents=True, exist_ok=True)
    # A source that already lacks the callback (e.g. a pre-patched build copy) passes through unchanged.
    output.write_text(original.replace(METHOD, "", 1), encoding="utf-8")


def _node_path(node):
    """Source path of a SCons node, or None when it cannot be determined."""
    srcnode = getattr(node, "srcnode", None)
    if srcnode is not None:
        return srcnode().get_path().replace("\\", "/")
    get_abspath = getattr(node, "get_abspath", None)
    return get_abspath().replace("\\", "/") if get_abspath is not None else None


def compile_compatible_host(build_env, node):
    # See configure_nimble_psram.py: pioarduino's Windows middleware wrapper
    # ignores the pattern and defers Object(), so filter and tolerate None.
    # The deferred compile uses the original source with these flags; the shim
    # (the part the link needs) still travels via -include.
    path = _node_path(node)
    if path is not None and not path.endswith("BleKeyboardHost/src/BleKeyboardHost.cpp"):
        return node
    compatible = build_env.Command(
        "$BUILD_DIR/ble-compat/BleKeyboardHost.cpp", node, write_compatible_source
    )
    build_env.Depends(compatible, build_env.subst("$PROJECT_DIR/scripts/patch_ble_keyboard_host.py"))
    shim = build_env.subst("$PROJECT_DIR/src/platform/BtLibraryInUseShim.h")
    config = build_env.GetProjectOption("custom_nimble_config", "")
    config_flags = []
    if config:
        config = str(Path(build_env.subst("$PROJECT_DIR")) / config)
        config_flags = ["-include", config]
    compiled = build_env.Object(
        compatible,
        CPPPATH=build_env.get("CPPPATH", []) + [str(Path(node.get_abspath()).parent)],
        # Custom-core bootstrap omits application sources, so the existing weak
        # flag must travel with this library in both bootstrap and final links.
        CCFLAGS=build_env.get("CCFLAGS", [])
        + ["-include", shim] + config_flags,
    )
    if not compiled:
        return None
    compiled = compiled[0]
    build_env.Depends(compiled, shim)
    if config:
        build_env.Depends(compiled, config)
    build_env.Depends(compiled, build_env.subst("$PROJECT_DIR/src/platform/BleIpcStack.h"))
    return compiled


env.AddBuildMiddleware(compile_compatible_host, "*/BleKeyboardHost/src/BleKeyboardHost.cpp")

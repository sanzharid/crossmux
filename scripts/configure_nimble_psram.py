"""Override NimBLE host configuration without changing installed sources."""

import os

Import("env")  # noqa: F821

config = os.path.join(
    env.subst("$PROJECT_DIR"),
    env.GetProjectOption("custom_nimble_config", "src/platform/NimblePsramConfig.h"),
)


def _node_path(node):
    """Source path of a SCons node, or None when it cannot be determined."""
    srcnode = getattr(node, "srcnode", None)
    if srcnode is not None:
        return srcnode().get_path().replace("\\", "/")
    get_abspath = getattr(node, "get_abspath", None)
    return get_abspath().replace("\\", "/") if get_abspath is not None else None


def configure_host(build_env, node):
    # On Windows, pioarduino wraps every middleware: it calls them for all
    # sources regardless of the pattern and defers Object() (returns None),
    # compiling the node itself with the captured kwargs. Filter here too.
    path = _node_path(node)
    if path is not None and "/NimBLE-Arduino/src/" not in path:
        return node
    compiled = build_env.Object(node, CCFLAGS=build_env.get("CCFLAGS", []) + ["-include", config])
    if not compiled:
        return None
    build_env.Depends(compiled[0], config)
    return compiled[0]


env.AddBuildMiddleware(configure_host, "*/NimBLE-Arduino/src/*")

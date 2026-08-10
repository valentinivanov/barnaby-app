CEF_WINDOWS_URL = "https://cef-builds.spotifycdn.com/cef_binary_149.0.5%2Bg6770623%2Bchromium-149.0.7827.197_windows64_minimal.tar.bz2"
CEF_WINDOWS_STRIP_PREFIX = "cef_binary_149.0.5+g6770623+chromium-149.0.7827.197_windows64_minimal"
CEF_LINUX_URL = "https://cef-builds.spotifycdn.com/cef_binary_149.0.6%2Bg0d0eeb6%2Bchromium-149.0.7827.201_linux64_minimal.tar.bz2"
CEF_LINUX_STRIP_PREFIX = "cef_binary_149.0.6+g0d0eeb6+chromium-149.0.7827.201_linux64_minimal"

def _is_windows(ctx):
    return ctx.os.name.lower().find("windows") != -1

def _cef_platform(ctx):
    if _is_windows(ctx):
        return struct(
            url = CEF_WINDOWS_URL,
            strip_prefix = CEF_WINDOWS_STRIP_PREFIX,
            shared_library = "Release/libcef.dll",
            interface_library = "Release/libcef.lib",
            layout_description = (
                "include/cef_app.h, Release/libcef.dll, Release/libcef.lib, " +
                "and Resources/resources.pak"
            ),
        )
    return struct(
        url = CEF_LINUX_URL,
        strip_prefix = CEF_LINUX_STRIP_PREFIX,
        shared_library = "Release/libcef.so",
        interface_library = None,
        layout_description = (
            "include/cef_app.h, Release/libcef.so, and Resources/resources.pak"
        ),
    )

def _workspace_root(ctx):
    return ctx.path(Label("//:MODULE.bazel")).dirname

def _workspace_cef_root(ctx):
    return _workspace_root(ctx).get_child("dependencies").get_child("cef")

def _has_cef_layout(ctx, root, platform):
    return (
        ctx.path("{}/include/cef_app.h".format(root)).exists and
        ctx.path("{}/{}".format(root, platform.shared_library)).exists and
        (not platform.interface_library or
         ctx.path("{}/{}".format(root, platform.interface_library)).exists) and
        ctx.path("{}/Resources/resources.pak".format(root)).exists
    )

def _symlink_cef_layout(ctx, root):
    ctx.symlink("{}/include".format(root), "include")
    ctx.symlink("{}/libcef_dll".format(root), "libcef_dll")
    ctx.symlink("{}/Release".format(root), "Release")
    ctx.symlink("{}/Resources".format(root), "Resources")

def _materialize_cef(ctx):
    platform = _cef_platform(ctx)
    cef_root = ctx.getenv("CEF_ROOT")
    if cef_root:
        root = cef_root.replace("\\", "/")
        if not _has_cef_layout(ctx, root, platform):
            fail(
                "CEF_ROOT='{}' does not contain the expected CEF minimal SDK " +
                "layout: {}."
            .format(cef_root, platform.layout_description))
        _symlink_cef_layout(ctx, root)
        return platform

    workspace_root = _workspace_cef_root(ctx)
    if _has_cef_layout(ctx, workspace_root, platform):
        _symlink_cef_layout(ctx, workspace_root)
        return platform

    ctx.download_and_extract(
        url = platform.url,
        stripPrefix = platform.strip_prefix,
    )

    if not _has_cef_layout(ctx, ".", platform):
        fail(
            "downloaded CEF archive did not contain the expected minimal SDK layout",
        )
    return platform

def _local_cef_impl(ctx):
    platform = _materialize_cef(ctx)
    interface_library_line = ""
    if platform.interface_library:
        interface_library_line = '    interface_library = "{}",\n'.format(platform.interface_library)

    build_file = """load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_import(
    name = "libcef",
%{interface_library_line}    shared_library = "%{shared_library}",
)

cc_library(
    name = "cef_wrapper",
    srcs = glob([
        "libcef_dll/**/*.cc",
        "libcef_dll/**/*.h",
        "include/**/*.inc",
    ]),
    hdrs = glob(["include/**"]),
    defines = [
        "NOMINMAX",
        "UNICODE",
        "WRAPPING_CEF_SHARED",
        "WINVER=0x0A00",
        "_UNICODE",
        "_WIN32_WINNT=0x0A00",
    ],
    copts = select({
        "@platforms//os:windows": ["/std:c++20"],
        "//conditions:default": [
            "-std=c++20",
            "-Wno-deprecated-declarations",
        ],
    }),
    includes = ["."],
    deps = [":libcef"],
)

filegroup(
    name = "runtime_files",
    srcs = glob([
        "Release/*.dll",
        "Release/*.bin",
        "Release/*.dat",
        "Release/*.pak",
        "Release/*.json",
        "Release/*.so",
        "Release/*.so.*",
        "Resources/*.bin",
        "Resources/*.dat",
        "Resources/*.pak",
        "Resources/locales/*.pak",
        "chrome-sandbox",
    ], allow_empty = True),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "cef",
    hdrs = glob(["include/**"]),
    includes = ["."],
    deps = [
        ":cef_wrapper",
        ":libcef",
    ],
    visibility = ["//visibility:public"],
)
"""
    build_file = build_file.replace("%{interface_library_line}", interface_library_line)
    build_file = build_file.replace("%{shared_library}", platform.shared_library)
    ctx.file("BUILD.bazel", build_file)

local_cef_repository = repository_rule(
    implementation = _local_cef_impl,
    environ = ["CEF_ROOT"],
    local = True,
)

def _cef_config_impl(module_ctx):
    local_cef_repository(name = "local_cef")

cef_config = module_extension(implementation = _cef_config_impl)

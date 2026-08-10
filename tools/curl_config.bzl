def _split_flags(value):
    return [flag for flag in value.strip().split(" ") if flag]

def _is_windows(ctx):
    return ctx.os.name.lower().find("windows") != -1

def _vcpkg_curl_roots(vcpkg_root):
    return [
        "{}/installed/x64-windows".format(vcpkg_root),
        "{}/packages/curl_x64-windows".format(vcpkg_root),
    ]

def _manifest_curl_roots():
    return [
        "vcpkg_installed/x64-windows",
        "vcpkg_installed/x64-windows-static",
    ]

def _vcpkg_executable(vcpkg_root):
    if _is_windows_path(vcpkg_root):
        return "{}/vcpkg.exe".format(vcpkg_root)
    return "{}/vcpkg".format(vcpkg_root)

def _is_windows_path(path):
    return path.find(":") == 1 or path.find("\\") != -1

def _find_curl_root(ctx, vcpkg_root):
    for candidate in _vcpkg_curl_roots(vcpkg_root):
        if (ctx.path("{}/include/curl/curl.h".format(candidate)).exists and
            ctx.path("{}/lib/libcurl.lib".format(candidate)).exists and
            ctx.path("{}/bin/libcurl.dll".format(candidate)).exists):
            return candidate
    return None

def _find_manifest_curl_root(ctx):
    for candidate in _manifest_curl_roots():
        if (ctx.path("{}/include/curl/curl.h".format(candidate)).exists and
            ctx.path("{}/lib/libcurl.lib".format(candidate)).exists and
            ctx.path("{}/bin/libcurl.dll".format(candidate)).exists):
            return candidate
    return None

def _write_vcpkg_manifest(ctx):
    ctx.file(
        "vcpkg.json",
        """{
  "name": "gitboard-cpp-bazel-deps",
  "version-string": "0.4.2",
  "dependencies": [
    "curl"
  ]
}
""",
    )

def _install_vcpkg_curl(ctx, vcpkg_root):
    vcpkg = _vcpkg_executable(vcpkg_root)
    if not ctx.path(vcpkg).exists:
        return None

    result = ctx.execute(
        [vcpkg, "install", "curl:x64-windows"],
        timeout = 1800,
    )
    if result.return_code != 0:
        _write_vcpkg_manifest(ctx)
        result = ctx.execute(
            [vcpkg, "install", "--triplet", "x64-windows"],
            timeout = 1800,
        )
        if result.return_code != 0 and result.stdout.find("specified baseline") != -1:
            baseline_result = ctx.execute(
                [vcpkg, "x-update-baseline", "--add-initial-baseline"],
                timeout = 1800,
            )
            if baseline_result.return_code != 0:
                fail(
                    "vcpkg is present at '{}', but generating a baseline for the curl manifest failed.\n\nstdout:\n{}\n\nstderr:\n{}"
                    .format(vcpkg, baseline_result.stdout, baseline_result.stderr),
                )
            result = ctx.execute(
                [vcpkg, "install", "--triplet", "x64-windows"],
                timeout = 1800,
            )
        if result.return_code != 0:
            fail(
                "vcpkg is present at '{}', but installing curl:x64-windows failed in classic and manifest modes.\n\nstdout:\n{}\n\nstderr:\n{}"
                .format(vcpkg, result.stdout, result.stderr),
            )

        return _find_manifest_curl_root(ctx)

    return _find_curl_root(ctx, vcpkg_root)

def _local_libcurl_windows_impl(ctx):
    vcpkg_root = ctx.getenv("VCPKG_ROOT")
    if not vcpkg_root:
        fail(
            "could not find libcurl on Windows because VCPKG_ROOT is not set. " +
            "Install curl with vcpkg and set VCPKG_ROOT to the vcpkg checkout.",
        )

    normalized_vcpkg_root = vcpkg_root.replace("\\", "/")
    curl_root = _find_curl_root(ctx, normalized_vcpkg_root)
    if not curl_root:
        curl_root = _find_manifest_curl_root(ctx)
    if not curl_root:
        curl_root = _install_vcpkg_curl(ctx, normalized_vcpkg_root)

    if not curl_root:
        fail(
            "could not find vcpkg libcurl under VCPKG_ROOT='{}'. Expected " +
            "installed/x64-windows or packages/curl_x64-windows with " +
            "include/curl/curl.h, lib/libcurl.lib, and bin/libcurl.dll."
        .format(vcpkg_root))

    ctx.symlink("{}/include".format(curl_root), "include")
    ctx.symlink("{}/lib/libcurl.lib".format(curl_root), "lib/libcurl.lib")
    ctx.symlink("{}/bin".format(curl_root), "bin")

    ctx.file(
        "BUILD.bazel",
        """load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_import(
    name = "libcurl_import",
    interface_library = "lib/libcurl.lib",
    shared_library = "bin/libcurl.dll",
)

cc_import(
    name = "zlib_runtime",
    shared_library = "bin/z.dll",
)

filegroup(
    name = "runtime_dlls",
    srcs = glob(["bin/*.dll"], allow_empty = True),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "libcurl",
    data = [":runtime_dlls"],
    hdrs = glob(["include/**"], allow_empty = True),
    includes = ["include"],
    deps = [
        ":libcurl_import",
        ":zlib_runtime",
    ],
    visibility = ["//visibility:public"],
)
""",
    )

def _local_libcurl_impl(ctx):
    if _is_windows(ctx):
        _local_libcurl_windows_impl(ctx)
        return

    cflags = ctx.execute(["curl-config", "--cflags"])
    libs = ctx.execute(["curl-config", "--libs"])

    if cflags.return_code != 0 or libs.return_code != 0:
        cflags = ctx.execute(["pkg-config", "--cflags", "libcurl"])
        libs = ctx.execute(["pkg-config", "--libs", "libcurl"])
        if cflags.return_code != 0 or libs.return_code != 0:
            fail(
                "could not find libcurl with curl-config or pkg-config. " +
                "Install the libcurl development package, for example " +
                "libcurl-devel on Fedora.",
            )

    compile_options = []
    include_paths = []
    for option in _split_flags(cflags.stdout):
        if option.startswith("-I"):
            include_dir = "include/path_{}".format(len(include_paths))
            ctx.symlink(option[2:], include_dir)
            include_paths.append(include_dir)
        else:
            compile_options.append(option)

    ctx.file(
        "BUILD.bazel",
        """load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "libcurl",
    hdrs = glob(["include/**"], allow_empty = True),
    includes = {},
    copts = {},
    linkopts = {},
    visibility = ["//visibility:public"],
)
""".format(
            repr(include_paths),
            repr(compile_options),
            repr(_split_flags(libs.stdout)),
        ),
    )

local_libcurl_repository = repository_rule(
    implementation = _local_libcurl_impl,
    environ = ["VCPKG_ROOT"],
    local = True,
)

def _curl_config_impl(module_ctx):
    local_libcurl_repository(name = "local_libcurl")

curl_config = module_extension(implementation = _curl_config_impl)

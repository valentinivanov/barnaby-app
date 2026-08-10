def _local_gtk_webkit_impl(ctx):
    cflags = ctx.execute(["pkg-config", "--cflags", "gtk4", "webkitgtk-6.0"])
    if cflags.return_code != 0:
        fail("pkg-config could not find gtk4 and webkitgtk-6.0:\n{}".format(cflags.stderr))

    libs = ctx.execute(["pkg-config", "--libs", "gtk4", "webkitgtk-6.0"])
    if libs.return_code != 0:
        fail("pkg-config could not link gtk4 and webkitgtk-6.0:\n{}".format(libs.stderr))

    compile_options = []
    include_paths = []
    for option in cflags.stdout.strip().split(" "):
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
    name = "gtk_webkit",
    hdrs = glob(["include/**"]),
    includes = {},
    copts = {},
    linkopts = {},
    visibility = ["//visibility:public"],
)
""".format(
            repr(include_paths),
            repr(compile_options),
            repr(libs.stdout.strip().split(" ")),
        ),
    )

local_gtk_webkit_repository = repository_rule(
    implementation = _local_gtk_webkit_impl,
    local = True,
)

def _gtk_config_impl(module_ctx):
    local_gtk_webkit_repository(name = "local_gtk_webkit")

gtk_config = module_extension(implementation = _gtk_config_impl)

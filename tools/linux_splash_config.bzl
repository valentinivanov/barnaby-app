def _split_flags(value):
    return [flag for flag in value.strip().split(" ") if flag]

def _local_linux_splash_impl(ctx):
    modules = ["x11", "cairo", "gdk-pixbuf-2.0"]
    cflags = ctx.execute(["pkg-config", "--cflags"] + modules)
    libs = ctx.execute(["pkg-config", "--libs"] + modules)

    if cflags.return_code != 0 or libs.return_code != 0:
        fail(
            "could not find Linux splash dependencies with pkg-config. " +
            "Install the X11, Cairo, and gdk-pixbuf development packages.",
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
    name = "linux_splash",
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

local_linux_splash_repository = repository_rule(
    implementation = _local_linux_splash_impl,
    local = True,
)

def _linux_splash_config_impl(module_ctx):
    local_linux_splash_repository(name = "local_linux_splash")

linux_splash_config = module_extension(implementation = _linux_splash_config_impl)

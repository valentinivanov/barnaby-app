def _split_flags(value):
    return [flag for flag in value.strip().split(" ") if flag]

def _local_libsecret_impl(ctx):
    cflags = ctx.execute(["pkg-config", "--cflags", "libsecret-1"])
    libs = ctx.execute(["pkg-config", "--libs", "libsecret-1"])

    if cflags.return_code != 0 or libs.return_code != 0:
        fail(
            "could not find libsecret with pkg-config. Install the " +
            "libsecret development package, for example libsecret-devel " +
            "on Fedora.",
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
    name = "libsecret",
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

local_libsecret_repository = repository_rule(
    implementation = _local_libsecret_impl,
    local = True,
)

def _libsecret_config_impl(module_ctx):
    local_libsecret_repository(name = "local_libsecret")

libsecret_config = module_extension(implementation = _libsecret_config_impl)

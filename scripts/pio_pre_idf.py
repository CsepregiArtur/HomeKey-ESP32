"""Pre-build hook for the PlatformIO ESP-IDF environment (HomeKey-ESP32).

`main/CMakeLists.txt` packs the Svelte web UI into the littlefs partition:

    littlefs_create_partition_image(spiffs ../data/dist FLASH_IN_PROJECT [DEPENDS webui])

When the ``CI`` environment variable is *not* defined, the CMake project also
creates a ``webui`` target that runs ``bun install && bun run build`` and the
littlefs image depends on it. PlatformIO does not ship ``bun``, so this hook
reproduces the project's own CI workflow instead:

1. build/refresh ``data/dist`` with npm (only when it is missing),
2. export ``CI`` before ESP-IDF's CMake configure step so the ``webui`` target
   is not created and ``data/dist`` is consumed as-is, and
3. make PlatformIO's "generic files" component (``__pio_env``) known to ESP-IDF
   early enough to survive the ``MINIMAL_BUILD`` component filter.

The script is loaded with the ``pre:`` prefix, i.e. before PlatformIO's platform
builder runs CMake.
"""

import os
import shutil
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

PROJECT_DIR = env.subst("$PROJECT_DIR")  # noqa: F821
DATA_DIR = os.path.join(PROJECT_DIR, "data")
DIST_DIR = os.path.join(DATA_DIR, "dist")

# 1. Keep ESP-IDF's build free of the bun-based `webui` target.
os.environ["CI"] = "true"

# 2. The littlefs image is generated from data/dist - build it when missing.
if not os.path.isdir(DIST_DIR) or not os.listdir(DIST_DIR):
    npm = shutil.which("npm")
    if not npm:
        print(
            "WARNING: 'data/dist' (web UI bundle) is missing and npm was not found.\n"
            "         The littlefs image will be empty. Build the UI first:\n"
            "           cd data && bun install && bun run build"
        )
    else:
        print("Web UI bundle 'data/dist' is missing - building it with npm ...")
        subprocess.check_call(
            [npm, "install", "--no-fund", "--no-audit"], cwd=DATA_DIR
        )
        subprocess.check_call([npm, "run", "build"], cwd=DATA_DIR)


# 3. PlatformIO compiles files that belong to no component into its own
#    "__pio_env" component and therefore requires it to be part of the build.
#    PlatformIO only drops that component into $IDF_PATH/components, which
#    ESP-IDF scans *after* it composed the component list for a MINIMAL_BUILD
#    (see COMPONENTS in tools/cmake/project.cmake), so the component gets
#    filtered out and the build stops with
#    "Failed to find the default IDF component with build information for generic files".
#    Fix: pass the component directory as an extra component directory (so it is
#    registered before the filter runs) and add it to COMPONENTS. ESP-IDF
#    tolerates the duplicate directory (__component_add() keeps the first entry).
#    The root project sets MINIMAL_BUILD, so COMPONENTS must list "main" as well;
#    ESP-IDF then expands the requirements of "main" exactly like a minimal build.
def _register_pio_generic_component():
    idf_path = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
    if not idf_path:
        return
    idf_path = os.path.normpath(idf_path)
    component_dir = os.path.join(idf_path, "components", "__pio_env")
    if not os.path.isdir(component_dir):
        os.makedirs(component_dir)

    cmake_lists = os.path.join(component_dir, "CMakeLists.txt")
    if not os.path.isfile(cmake_lists):
        with open(cmake_lists, "w") as fp:
            fp.write(
                "# Warning! Do not delete this auto-generated file.\n"
                "file(GLOB component_sources *.c* *.S)\n"
                "idf_component_register(SRCS ${component_sources})\n"
            )
    for ext in (".c", ".cpp", ".S"):
        open(os.path.join(component_dir, "__dummy" + ext), "a").close()

    cmake_args = [
        env.BoardConfig().get("build.cmake_extra_args", "").strip(),  # noqa: F821
        '"-DEXTRA_COMPONENT_DIRS:PATH=%s"'
        % ";".join([env.subst("$PROJECT_SRC_DIR"), component_dir]),  # noqa: F821
        '"-DCOMPONENTS=main;__pio_env"',
    ]
    env.BoardConfig().update(  # noqa: F821
        "build.cmake_extra_args", " ".join(a for a in cmake_args if a)
    )


_register_pio_generic_component()


# 4. Since ESP-IDF 5.5.4 the toolchain keeps compiler/linker flags in response
#    files, so CMake's compile fragments look like @"<build>/toolchain/asmflags".
#    PlatformIO 6.13 strips the surrounding quotes before parsing such a fragment
#    with shlex, which leaves an unbalanced quote:
#        ValueError: No closing quotation
#    ESP-IDF has no switch to turn response files off, so make the parse lenient.
#    Dropping the quotes yields "@<build>/toolchain/asmflags", which is still a
#    valid GCC response-file reference.
def _relax_shlex_quoting():
    import shlex

    original_split = shlex.split

    def split(s, comments=False, posix=True):
        try:
            return original_split(s, comments=comments, posix=posix)
        except ValueError:
            if '"' in s:
                return original_split(
                    s.replace('"', ""), comments=comments, posix=posix
                )
            raise

    shlex.split = split


_relax_shlex_quoting()

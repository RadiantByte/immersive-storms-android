local version = "1.0.0"
set_project("ImmersiveStorms")
set_version(version)
add_rules("mode.release", "mode.debug")

option("preloader_sdk")
    set_showmenu(true)
    set_description("Path to a preloader-android SDK checkout")
option_end()

package("storms_preloader_sdk")
    set_kind("library", {headeronly = true})
    add_urls("https://codeload.github.com/LiteLDev/preloader-android/tar.gz/cc08b40a31471d3e2dedb267f5f17b28da2b0604",
             {filename = "preloader-sdk.tar.gz"})
    add_versions("1.0.0", "7cde0b815f91e1d428f8164f1bd5ea802dc96a07f1e0705140539eaf4838cb7a")
    on_install(function (pkg)
        os.cp("include/pl", pkg:installdir("include"))
    end)
package_end()

if not has_config("preloader_sdk") then add_requires("storms_preloader_sdk 1.0.0") end

target("ImmersiveStorms")
    set_kind("shared")
    set_languages("c++20")
    set_runtimes("c++_shared")
    add_files("src/*.cpp")
    add_defines('STORMS_VERSION="' .. version .. '"')
    if has_config("preloader_sdk") then
        add_includedirs(path.join(get_config("preloader_sdk"), "include"))
    else
        add_packages("storms_preloader_sdk")
    end
    if is_mode("release") then set_strip("all") end
    add_cxxflags("-fno-rtti", "-fvisibility=hidden", "-fvisibility-inlines-hidden", "-Wall", "-Wextra")
    add_shflags("-Wl,--no-undefined", "-Wl,-z,max-page-size=16384", "-Wl,--build-id=sha1",
        "-Wl,--version-script=" .. path.join(os.projectdir(), "exports.map"), {force = true})
    add_syslinks("aaudio", "android", "dl", "log")

    before_build(function (target)
        assert(target:is_plat("android") and target:arch() == "arm64-v8a",
               "Configure for Android arm64-v8a")
    end)

    after_build(function (target)
        import("lib.detect.find_tool")
        local python = find_tool("python3") or find_tool("python")
        local args = {}
        if not python then
            python = find_tool("py")
            if python then table.insert(args, "-3") end
        end
        assert(python, "Python 3 is required to package ImmersiveStorms.levipack")
        table.insert(args, path.join(os.projectdir(), "scripts", "package_levipack.py"))
        table.insert(args, target:targetfile())
        table.insert(args, path.join(target:targetdir(), "ImmersiveStorms.levipack"))
        os.vrunv(python.program, args)
    end)

--Set platform, cause some of us aren't on Windows.
set_plat("windows")
set_arch("x64")

-- f4ffmpeg uses REX::TTomlSetting / FTomlSettingStore. CommonLib-shared
-- keeps TOML support optional and disabled by default, so enable it before
-- loading CommonLibF4 so the option propagates into commonlib-shared.
set_config("commonlib_toml", true)

-- include subprojects
includes("lib/commonlibf4")

-- FFMPEG is required, and we want an explicitly vulkan build. We use a custom package for this. Feel free to go through the effort to make this some build flag.
add_repositories(
    "f4ffmpeg-repo xmake-packages",
    {rootdir = os.projectdir()}
)


-- Build FFmpeg with NVIDIA's NVDEC/CUDA decode path available. This remains
-- runtime-optional: systems without an NVIDIA driver/CUDA bridge simply skip
-- the backend and fall through to the remaining hardware APIs/software.
add_requires("ffmpeg 9.0.2", {
    configs = {
        ffmpeg = false,
        ffprobe = false,
        ffplay = false,
        nvdec = true,
        libzimg = false
    }
})

-- libplacebo is opportunistic. f4ffmpeg.dll never imports it directly.
-- In static/auto builds the vcpkg package is linked into the optional
-- f4ffmpeg_placebo.dll companion; runtime mode only enables probing for a
-- separately supplied compatible companion. libswscale remains core fallback.
option("libplacebo_mode")
    set_default("auto")
    set_showmenu(true)
    set_values(
        "auto",
        "static",
        "runtime",
        "off"
    )
    set_description(
        "libplacebo backend mode: auto/static(runtime companion built)/runtime/off"
    )
option_end()

local placebo_mode =
    get_config("libplacebo_mode") or
    "auto"

if placebo_mode == "static" then
    -- vcpkg owns libplacebo's Meson/tool/dependency environment. The repository
    -- overlay builds libplacebo itself with clang-cl while f4ffmpeg remains MSVC.
    --
    -- libplacebo is installed as a static archive. Xmake's generic vcpkg package
    -- discovery does not reliably propagate the private dependency closure from
    -- libplacebo.pc, so make the non-system static dependencies explicit for the
    -- companion target as well. The repository port uses glslang directly rather
    -- than shaderc to avoid the shaderc 2026.2 compiler-crash regression implicated by the first-render failure.
    add_requires(
        "vcpkg::libplacebo",
        {
            alias = "f4ffmpeg-libplacebo"
        }
    )
    add_requires(
        "vcpkg::glslang",
        {
            alias = "f4ffmpeg-glslang"
        }
    )
    add_requires(
        "vcpkg::spirv-cross",
        {
            alias = "f4ffmpeg-spirv-cross"
        }
    )
elseif placebo_mode == "auto" then
    add_requires(
        "vcpkg::libplacebo",
        {
            alias = "f4ffmpeg-libplacebo",
            optional = true
        }
    )
    add_requires(
        "vcpkg::glslang",
        {
            alias = "f4ffmpeg-glslang",
            optional = true
        }
    )
    add_requires(
        "vcpkg::spirv-cross",
        {
            alias = "f4ffmpeg-spirv-cross",
            optional = true
        }
    )
end

-- set project constants
set_project("f4ffmpeg")
set_version("0.4.0")
set_license("GPL-3.0")
set_languages("c11", "c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- override runtime count
add_defines("COMMONLIB_RUNTIMECOUNT=3")

-- define targets
target("f4ffmpeg")
    add_rules("commonlibf4.plugin", {
        name = "f4ffmpeg",
        author = "Continous",
        description = "This is a simple plugin intended to provide FFMPEG."
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- Core never links libplacebo. It late-loads f4ffmpeg_placebo.dll through
    -- the small C ABI in placeboBackendApi.h and always retains libswscale.
    add_packages("ffmpeg")

    if placebo_mode == "off" then
        add_defines("F4FFMPEG_ALLOW_PLACEBO_BACKEND=0")
    else
        add_defines("F4FFMPEG_ALLOW_PLACEBO_BACKEND=1")
    end

target_end()

local build_placebo_backend =
    placebo_mode == "static" or
    (
        placebo_mode == "auto" and
        has_package("f4ffmpeg-libplacebo") and
        has_package("f4ffmpeg-glslang") and
        has_package("f4ffmpeg-spirv-cross")
    )

if build_placebo_backend then
    target("f4ffmpeg_placebo")
        set_kind("shared")
        set_filename("f4ffmpeg_placebo.dll")
        set_default(true)

        add_files(
            "backends/placebo/placeboBackend.cpp",
            "backends/placebo/placeboLibav.c"
        )
        add_headerfiles("src/placeboBackendApi.h")
        add_includedirs("src")

        -- The companion owns the hard libplacebo link. libplacebo itself is
        -- static here, so f4ffmpeg_placebo.dll is the only optional runtime
        -- component that core needs to probe. Keep the static dependency
        -- closure explicit: glslang and SPIRV-Cross are private libplacebo
        -- dependencies retained by the current vcpkg overlay. The Vulkan
        -- companion itself only needs libplacebo's Vulkan+glslang backend;
        -- D3D11 is used solely for the final Fallout texture handoff.
        add_packages(
            "ffmpeg",
            "f4ffmpeg-libplacebo",
            "f4ffmpeg-glslang",
            "f4ffmpeg-spirv-cross"
        )
        add_defines("PL_STATIC")
        add_syslinks("d3d11", "dxgi", "shlwapi", "version")
    target_end()
end

target("placebo-loader-smoke")
    set_kind("binary")
    set_default(false)
    add_files("tools/placebo-loader-smoke.cpp")
    add_includedirs("src")
target_end()

target("placebo-render-smoke")
    set_kind("binary")
    set_default(false)
    add_files("tools/placebo-render-smoke.cpp")
    add_includedirs("src")
    add_packages("ffmpeg")
    add_syslinks("d3d11")
target_end()

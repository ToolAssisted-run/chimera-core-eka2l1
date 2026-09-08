# The option set both flavors configure with. Sourced, never run.
# Off: the frontend (we drive system::loop() ourselves), the tools, the Vulkan
# backend, LuaJIT scripting, the Qt camera, discord, and the Symbian-SDK patch
# build. On: upstream's own test suite, which is the first workload this port
# can run without a device dump.
EKA2L1_OPTS="
-DCMAKE_BUILD_TYPE=RelWithDebInfo
-DEKA2L1_BUILD_FRONTEND=OFF
-DEKA2L1_BUILD_TOOLS=OFF
-DEKA2L1_BUILD_TESTS=ON
-DEKA2L1_BUILD_VULKAN_BACKEND=OFF
-DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF
-DEKA2L1_ENABLE_QT_CAMERA=OFF
-DEKA2L1_ENABLE_DISCORD_RICH_PRESENCE=OFF
-DEKA2L1_BUILD_PATCH=OFF
-DEKA2L1_BUILD_FFMPEG=OFF
"
export EKA2L1_OPTS

# FFmpeg is the guest's alone. The submodule's prebuilt libraries are built
# against glibc, so the guest cannot link them and compiles the source beside
# them instead (waterbox/setup-ffmpeg.sh) - which is what gives games their
# sound. The native reference and the cpu difftest have no use for audio and
# are left as they were, off, rather than made to depend on a build step that
# is only about the guest.
EKA2L1_GUEST_FFMPEG_OPTS="
-DEKA2L1_BUILD_FFMPEG=ON
-DEKA2L1_FFMPEG_GUEST_ROOT=$root/build/ffmpeg-guest/stage
"
export EKA2L1_GUEST_FFMPEG_OPTS

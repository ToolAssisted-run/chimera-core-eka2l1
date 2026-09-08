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
-DEKA2L1_BUILD_FFMPEG=ON
-DEKA2L1_FFMPEG_GUEST_ROOT=$root/build/ffmpeg-guest/stage
"
export EKA2L1_OPTS

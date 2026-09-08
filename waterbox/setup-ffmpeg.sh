#!/bin/sh
# Builds the audio decoders this core carries: FFmpeg, from the source the
# eka2l1 submodule already ships, for the miniBox guest toolchain.
#
# Why this exists. The submodule carries FFmpeg's source AND prebuilt libraries
# for each desktop platform, and the CMake glue reaches for the prebuilt ones.
# They are built against glibc, so a guest built against musl cannot link them,
# and the core was configured -DEKA2L1_BUILD_FFMPEG=OFF to get a build at all.
# But eka2l1 has exactly one audio path: new_dsp_out_stream returns a decoder
# stream or a null pointer, and without FFmpeg it returns the null pointer. No
# stream is created, nothing ever asks the audio driver for one, and every game
# is silent. Measured before this: 735 sample pairs a frame, every one of them
# zero, and chimera::audio_sink::new_output_stream never called once in 600
# frames.
#
# So the source is compiled here instead of the prebuilts being borrowed. Only
# what the machine asks for: the four decoders eka2l1 maps a Symbian FourCC to
# (AMR-NB, MP3, PCM_S16LE, PCM_S8) and the demuxers to reach them.
#
# --disable-asm and --disable-runtime-cpudetect are not size or caution. FFmpeg
# picks SIMD paths per host at runtime, and a decoder that takes a different
# path can produce different samples - which in a core that replays movies is a
# desync between one person's machine and another's. Plain C decodes the same
# everywhere. --disable-pthreads for the same reason the rest of the guest has
# no threads of its own.
#
# The build lands in build/ffmpeg-guest/stage and is what the CMake glue links
# against; a core built without it still runs every game, and none of them make
# a sound.
#
# Usage: ./setup-ffmpeg.sh [-m <miniBox dir>] [-j N]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/chimera/extern/chimera-common-minibox}"
jobs="$(nproc)"
while getopts "m:j:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		j) jobs="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
sr="${MINIBOX_SYSROOT:-$mb/build/meson-cpp/guest-sysroot}"
[ -f "$sr/lib/musl-gcc.specs" ] || {
	echo "miniBox C++ guest toolchain missing at $sr" >&2
	exit 1
}

src="$root/extern/eka2l1/src/external/ffmpeg"
[ -f "$src/configure" ] || {
	echo "no FFmpeg source at $src - is the eka2l1 submodule checked out?" >&2
	exit 1
}

out="$root/build/ffmpeg-guest"
stage="$out/stage"
mkdir -p "$out"

# The same flags the guest toolchain compiles everything else with: the fixed
# base wants the large code model and no PIC, and the sandbox has no stack
# protector to call into.
wb="-specs=$sr/lib/musl-gcc.specs -fvisibility=hidden -mcmodel=large"
wb="$wb -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none"

if [ ! -f "$out/config.h" ]; then
	( cd "$out" && "$src/configure" \
		--prefix="$stage" \
		--cc=gcc \
		--extra-cflags="$wb" \
		--extra-ldflags="-static" \
		--disable-everything \
		--disable-programs --disable-doc --disable-shared --enable-static \
		--disable-asm --disable-runtime-cpudetect \
		--disable-pthreads --disable-network --disable-autodetect \
		--disable-avdevice --disable-avfilter --disable-postproc \
		--disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl2 \
		--enable-avformat --enable-avcodec --enable-swresample --enable-swscale \
		--enable-decoder=amrnb,mp3,pcm_s16le,pcm_s8,pcm_u8,pcm_s16be \
		--enable-demuxer=mp3,amr,wav,pcm_s16le,mov \
		--enable-parser=mpegaudio \
		--enable-protocol=file )
fi

make -C "$out" -j"$jobs" install

echo "ffmpeg: built in $stage"

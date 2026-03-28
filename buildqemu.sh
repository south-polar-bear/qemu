#!/bin/bash

# NDK configuration
export ANDROID_NDK_HOME=/home/wb/qemu2/ndk/android-ndk-r28
export NDK_HOME=$ANDROID_NDK_HOME
export TOOLCHAIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64

# Set sysroot paths
export NDK_SYSROOT=$TOOLCHAIN/sysroot
export CUSTOM_SYSROOT=/home/wb/android

# NDK Clang compiler for aarch64 (API level 30)
export CC=$TOOLCHAIN/bin/aarch64-linux-android30-clang
export CXX=$TOOLCHAIN/bin/aarch64-linux-android30-clang++

# NDK LLVM tools
export AR=$TOOLCHAIN/bin/llvm-ar
export NM=$TOOLCHAIN/bin/llvm-nm
export STRIP=$TOOLCHAIN/bin/llvm-strip
export RANLIB=$TOOLCHAIN/bin/llvm-ranlib
export OBJCOPY=$TOOLCHAIN/bin/llvm-objcopy
export OBJDUMP=$TOOLCHAIN/bin/llvm-objdump
export READELF=$TOOLCHAIN/bin/llvm-readelf

# Enter QEMU source
mkdir -p build && cd build

# Create librt.pc stub for Android (librt functions are in libc)
cat > /home/wb/android/lib/pkgconfig/librt.pc << 'EOF'
prefix=/home/wb/android
libdir=${prefix}/lib
includedir=${prefix}/include

Name: librt
Description: POSIX real-time library stub (functions in libc on Android)
Version: 1.0
Libs: 
Cflags: 
EOF

# Create pkg-config wrapper that sets correct paths
cat > pkg-config-wrapper << 'WRAPPER'
#!/bin/sh
export PKG_CONFIG_LIBDIR=/home/wb/android/lib/pkgconfig
export PKG_CONFIG_PATH=/home/wb/android/lib/pkgconfig
exec /usr/bin/pkg-config "$@"
WRAPPER
chmod +x pkg-config-wrapper

# Create cross-prefix pkg-config wrapper (meson looks for this first)
ln -sf pkg-config-wrapper aarch64-linux-android30-pkg-config

# Add build directory to PATH so meson can find our wrappers
export PATH=$PWD:$PATH
export PKG_CONFIG=$PWD/pkg-config-wrapper

# Create meson cross-compilation file
cat > cross.ini << EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
nm = '$NM'
strip = '$STRIP'
ranlib = '$RANLIB'
pkgconfig = 'aarch64-linux-android30-pkg-config'

[properties]
skip_sanity_check = true

[built-in options]
c_args = ['-I$CUSTOM_SYSROOT/usr/include/glib-2.0', '-I$CUSTOM_SYSROOT/lib/glib-2.0/include', '--sysroot=$NDK_SYSROOT']
cpp_args = ['-I$CUSTOM_SYSROOT/usr/include/glib-2.0', '-I$CUSTOM_SYSROOT/lib/glib-2.0/include', '--sysroot=$NDK_SYSROOT']
c_link_args = ['-L$CUSTOM_SYSROOT/usr/lib', '--sysroot=$NDK_SYSROOT']
cpp_link_args = ['-L$CUSTOM_SYSROOT/usr/lib', '--sysroot=$NDK_SYSROOT']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

# Create native file for build machine tools
cat > native.ini << EOF
[binaries]
c = 'cc'
cpp = 'c++'
pkgconfig = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

# Run configure and capture exit code
../configure \
    --target-list=aarch64-softmmu \
    --cross-prefix=aarch64-linux-android30- \
    --cc="$CC" \
    --cxx="$CXX" \
    --host-cc=cc \
    --disable-werror \
    --disable-vhost-user \
    --disable-virtfs \
    --extra-cflags="-I$CUSTOM_SYSROOT/usr/include/glib-2.0 -I$CUSTOM_SYSROOT/lib/glib-2.0/include --sysroot=$NDK_SYSROOT" \
    --extra-ldflags="-L$CUSTOM_SYSROOT/usr/lib --sysroot=$NDK_SYSROOT"
configure_status=$?

# If configure failed due to meson error, patch the cross file and retry
if [ $configure_status -ne 0 ]; then
    # Patch the generated cross file
    if [ -f config-meson.cross ]; then
        # Change system to 'android' and add skip_sanity_check
        sed -i "s|system = 'linux'|system = 'android'|" config-meson.cross
        sed -i '/^\[properties\]$/a skip_sanity_check = true' config-meson.cross
        
        # Clean meson state
        rm -rf meson-private meson-info meson-logs
        
        # Run meson directly with the patched cross file (from parent dir)
        cd ..
        python3 -m mesonbuild.mesonmain setup --cross-file build/config-meson.cross --native-file build/config-meson.native build
        cd build
    fi
fi

# Single-threaded build for low-resource systems
make

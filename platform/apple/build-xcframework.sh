#!/bin/sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work="$here/build"
out="$here/Mapkern.xcframework"
rm -rf "$out" "$work/headers"
mkdir -p "$work/headers"
cp "$root/include/mapkern/mapkern.h" "$here/include/module.modulemap" "$work/headers/"

slice() {
  name=$1
  shift
  dir="$work/$name"
  cmake -S "$root" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMAPKERN_TESTS=OFF -DMAPKERN_VIEWER=OFF -DMAPKERN_SHARED=OFF "$@"
  cmake --build "$dir" --target mapkern_capi
  libs=$(find "$dir" -name '*.a' ! -name '*test*' | sort)
  libtool -static -o "$dir/libmapkern.a" $libs
}

slice macos -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
slice ios -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
slice simulator -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0

xcodebuild -create-xcframework \
  -library "$work/macos/libmapkern.a" -headers "$work/headers" \
  -library "$work/ios/libmapkern.a" -headers "$work/headers" \
  -library "$work/simulator/libmapkern.a" -headers "$work/headers" \
  -output "$out"
echo "$out"

# Swift integration example

Build the C++ project first, then run from this repository’s root directory:

```sh
swiftc -I examples/swift -L build-release -lrawengine \
  -Xlinker -rpath -Xlinker "$PWD/build-release" \
  examples/swift/main.swift -o build-release/swift-example
build-release/swift-example /path/to/photo.RAW /tmp/photo.png
```

The example uses the versioned C ABI, checks errors, copies a preview buffer, exports, and releases all
owned objects. A native app should perform these synchronous calls off the main thread. `re_job_cancel`
is safe from another thread. Callbacks execute on the calling job thread; marshal UI updates to the
main actor. A render buffer contains **linear Rec.2020** pixels; assign that color space when presenting
it through Core Image or Metal. The preview is not gamma-encoded sRGB.

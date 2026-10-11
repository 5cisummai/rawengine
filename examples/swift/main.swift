import Foundation
import RawEngine

func check(_ status: re_status) throws {
    if status != RE_OK {
        throw NSError(domain: "RawEngine", code: Int(status.rawValue),
                      userInfo: [NSLocalizedDescriptionKey: String(cString: re_last_error())])
    }
}

do {
    guard CommandLine.arguments.count == 3 else {
        throw NSError(domain: "Example", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: "Usage: swift-example INPUT OUTPUT.png"])
    }
    var engine: OpaquePointer?
    try check(re_engine_create(nil, &engine))
    defer { re_engine_destroy(engine) }
    var source: OpaquePointer?
    try check(re_open(engine, CommandLine.arguments[1], nil, &source))
    defer { re_source_destroy(source) }
    var metadata: UnsafeMutablePointer<CChar>?
    try check(re_metadata_json(source, &metadata))
    if let metadata {
        print(String(cString: metadata))
        re_string_destroy(metadata)
    }
    var options = re_render_options(abi_version: UInt32(RE_ABI_VERSION), max_dimension: 1200,
                                    x: 0, y: 0, width: 0, height: 0, tile_rows: 64)
    var image: UnsafeMutablePointer<re_image>?
    try check(re_render(engine, source, "{\"adjustments\":{\"exposure\":0.5}}", &options, nil, &image))
    if let image {
        print("Preview: \(image.pointee.width) × \(image.pointee.height), linear Rec.2020 float RGB")
        // Copy to an owned Metal/CI buffer before releasing the engine buffer.
        let pixels = Array(UnsafeBufferPointer(start: image.pointee.pixels, count: image.pointee.count))
        print("First RGB pixel: \(Array(pixels.prefix(3)))")
        re_image_destroy(image)
    }
    try check(re_export(engine, source, "{}", CommandLine.arguments[2], nil, nil))
} catch {
    fputs("\(error.localizedDescription)\n", stderr)
    exit(1)
}

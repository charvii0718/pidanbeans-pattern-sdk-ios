import Foundation
import ImageIO
import CoreGraphics
#if canImport(PidanCore)
import PidanCore
#endif

// ImageIO source is local data only. EXIF orientation is deliberately not applied.
enum ImagePreparation {
  struct InvalidImage: Error {}
  struct RecognitionFailure: Error { let code: String }
  static func recognize(_ data: Data, token: OpaquePointer? = nil) throws -> String { try recognizeWatermark(data, token: token).bits }
  /// Share-id bits and the two rendering flags from the barcode.
  static func recognizeWatermark(_ data: Data, token: OpaquePointer? = nil) throws -> (bits: String, mirror: Bool, watermark: Bool) {
    let work=try prepare(data, token: token)
    var out=pidan_watermark()
    let status=work.pixels.withUnsafeBytes { b in
      pidan_recognize_ex(b.bindMemory(to:UInt8.self).baseAddress,b.count,Int32(work.width),Int32(work.height),4,1,&out,token)
    }
    guard status == 0 else { throw RecognitionFailure(code:status == 6 ? "CANCELLED" : status == 2 ? "NOT_RECOGNIZED" : "PROCESSING_ERROR") }
    let bits=withUnsafePointer(to:&out.share_id_bits) { String(cString:UnsafeRawPointer($0).assumingMemoryBound(to:CChar.self)) }
    return (bits, out.mirror == 1, out.watermark == 1)
  }
  static func prepare(_ data: Data, token: OpaquePointer? = nil) throws -> (pixels: Data, width: Int, height: Int) {
    func check() throws { if pidan_cancel_requested(token) != 0 { throw RecognitionFailure(code: "CANCELLED") } }
    try check()
    guard let source = CGImageSourceCreateWithData(data as CFData, [kCGImageSourceShouldCache: false] as CFDictionary),
      let type = CGImageSourceGetType(source) as String?, ["public.png", "public.jpeg"].contains(type),
      CGImageSourceGetCount(source) == 1,
      let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
      let w = props[kCGImagePropertyPixelWidth] as? Int, let h = props[kCGImagePropertyPixelHeight] as? Int,
      w > 0, h > 0, w <= Int(Int32.max), h <= Int(Int32.max)
    else { throw InvalidImage() }
    func render(_ p: [Int32]) throws -> Data {
      let x=Int(p[0]), y=Int(p[1]), cw=Int(p[2]), ch=Int(p[3]), ow=Int(p[4]), oh=Int(p[5])
      let image: CGImage
      if x == 0 && y == 0 && cw == w && ch == h {
        guard let thumb = CGImageSourceCreateThumbnailAtIndex(source, 0, [
          kCGImageSourceCreateThumbnailFromImageAlways: true,
          kCGImageSourceCreateThumbnailWithTransform: false,
          kCGImageSourceThumbnailMaxPixelSize: max(ow,oh),
          kCGImageSourceShouldCacheImmediately: true,
        ] as CFDictionary) else { throw InvalidImage() }
        image = thumb
      } else {
        guard let full = CGImageSourceCreateImageAtIndex(source, 0, [kCGImageSourceShouldCache: false] as CFDictionary),
          let cropped = full.cropping(to: CGRect(x:x,y:y,width:cw,height:ch)) else { throw InvalidImage() }
        image = cropped
      }
      var pixels = Data(count: ow * oh * 4)
      try pixels.withUnsafeMutableBytes { buffer in
        guard let ctx = CGContext(data:buffer.baseAddress,width:ow,height:oh,bitsPerComponent:8,bytesPerRow:ow*4,
          space:CGColorSpace(name: CGColorSpace.sRGB)!,bitmapInfo:CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue)
          else { throw InvalidImage() }
        ctx.setFillColor(CGColor(gray:1,alpha:1)); ctx.fill(CGRect(x:0,y:0,width:ow,height:oh))
        ctx.interpolationQuality = .high
        ctx.draw(image,in:CGRect(x:0,y:0,width:ow,height:oh))
      }
      return pixels
    }
    var size = [Int32](repeating:0,count:2)
    guard pidan_probe_size(Int32(w),Int32(h),&size) == 0 else { throw InvalidImage() }
    let probe = try render([0,0,Int32(w),Int32(h),size[0],size[1]])
    try check()
    var plan = [Int32](repeating:0,count:6)
    let status = probe.withUnsafeBytes { b in
      pidan_image_plan(b.bindMemory(to:UInt8.self).baseAddress,b.count,Int32(w),Int32(h),&plan)
    }
    guard status == 0 else { throw InvalidImage() }
    try check()
    return (try render(plan),Int(plan[4]),Int(plan[5]))
  }
}

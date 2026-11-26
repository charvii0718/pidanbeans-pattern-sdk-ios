import CoreFoundation
import Foundation

#if canImport(PidanCore)
  import PidanCore
#endif
#if canImport(FoundationNetworking)
  import FoundationNetworking
#endif

public struct SDKError: Error {
  public let code: String
  public let message: String
  public init(_ code: String, _ message: String) {
    self.code = code
    self.message = message
  }
}
public struct Pattern {
  public let author: String
  public let name: String
  public let M: Int
  public let N: Int
  public let createdAt: Int64
  public let cells: [[String?]]
  /// The 44 share-id bits decoded from the input barcode, including leading zeros.
  public let shareIdBits: String
  /// Mirror flag carried in the barcode (true = mirrored chart). `cells` keep the cloud orientation.
  public let mirror: Bool
  /// Whether the barcode requests rendering the author name as a watermark.
  public let watermark: Bool
}
// Each query has its own session and serial delegate queue.
final class QueryResponseDelegate: NSObject, URLSessionDataDelegate, @unchecked Sendable {
  private static let byteLimit = 1_048_576
  private var body = Data()
  private var completion: ((Result<Data, SDKError>) -> Void)?

  init(completion: @escaping (Result<Data, SDKError>) -> Void) {
    self.completion = completion
  }

  private func finish(_ result: Result<Data, SDKError>) {
    let callback = completion
    completion = nil
    callback?(result)
  }

  func urlSession(_ session: URLSession, dataTask: URLSessionDataTask,
                  didReceive response: URLResponse,
                  completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
    guard let http = response as? HTTPURLResponse else {
      completionHandler(.cancel)
      finish(.failure(SDKError("INVALID_RESPONSE", "Missing HTTP response")))
      return
    }
    if http.statusCode != 200 {
      let code: String
      switch http.statusCode {
      case 401, 403: code = "UNAUTHORIZED"
      case 404: code = "NOT_FOUND"
      case 429: code = "RATE_LIMITED"
      case 500...599: code = "SERVER_ERROR"
      default: code = "INVALID_RESPONSE"
      }
      completionHandler(.cancel)
      finish(.failure(SDKError(code, "Query HTTP \(http.statusCode)")))
      return
    }
    completionHandler(.allow)
  }

  func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
    guard completion != nil else { return }
    guard data.count <= Self.byteLimit - body.count else {
      dataTask.cancel()
      finish(.failure(SDKError("INVALID_RESPONSE", "Response too large")))
      return
    }
    body.append(data)
  }

  func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    defer { session.finishTasksAndInvalidate() }
    if let error = error as NSError? {
      let timedOut = error.domain == NSURLErrorDomain && error.code == NSURLErrorTimedOut
      finish(.failure(SDKError(timedOut ? "TIMEOUT" : "NETWORK_ERROR", "Query failed")))
    } else {
      finish(.success(body))
    }
  }


  func urlSession(
    _ session: URLSession, task: URLSessionTask,
    willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest,
    completionHandler: @escaping (URLRequest?) -> Void
  ) { completionHandler(nil) }
}
public final class PatternSDK {
  private let baseURL: String
  private let apiPath: String
  private let apiKey: String
  private let timeout: TimeInterval
  public init(baseURL: String, apiKey: String, timeoutMs: Int = 15000, apiPath: String = "/v1/patterns") throws {
    guard let url = URL(string: baseURL), let scheme = url.scheme,
      ["https", "http"].contains(scheme), url.host != nil, url.user == nil, url.password == nil,
      url.query == nil, url.fragment == nil,
      !apiKey.isEmpty, apiKey.utf8.allSatisfy({ $0 >= 33 && $0 <= 126 }),
      (1...300000).contains(timeoutMs)
    else { throw SDKError("INVALID_INPUT", "Invalid configuration") }
    guard !apiPath.isEmpty,
      !apiPath.contains(where: {
        $0 == "#" || $0.isWhitespace
          || $0.unicodeScalars.contains { $0.value < 32 || $0.value == 127 }
      })
    else { throw SDKError("INVALID_INPUT", "Invalid configuration") }
    self.baseURL = baseURL.replacingOccurrences(of: "/+$", with: "", options: .regularExpression)
    self.apiPath = apiPath.hasPrefix("/") ? apiPath : "/" + apiPath
    self.apiKey = apiKey
    self.timeout = Double(timeoutMs) / 1000
  }
  private func queryURL(bits: String) throws -> URL {
    let separator = apiPath.contains("?") ? "&" : "?"
    guard let url = URL(string: baseURL + apiPath + separator + "bits=" + bits) else {
      throw SDKError("INVALID_INPUT", "Invalid configuration")
    }
    return url
  }
  @discardableResult
  public func startRecognize(
    pixels: Data, width: Int, height: Int, format: String = "RGBA",
    completion: @escaping (Result<Pattern, SDKError>) -> Void
  ) -> RecognitionTask {
    start(completion: completion) { task in
      try Self.recognizeWatermark(pixels: pixels, width: width, height: height, format: format, token: task.token)
    }
  }
  @discardableResult
  public func startRecognizeImage(at url: URL,
    completion: @escaping (Result<Pattern, SDKError>) -> Void) -> RecognitionTask {
    start(image: true, completion: completion) { task in
      guard url.isFileURL else { throw SDKError("INVALID_INPUT", "Expected local file URL") }
      let data = try Data(contentsOf: url, options: .mappedIfSafe)
      try task.check()
      let work = try ImagePreparation.prepare(data, token: task.token)
      try task.check()
      return try Self.recognizeWatermark(pixels: work.pixels, width: work.width, height: work.height, prepared: true, token: task.token)
    }
  }
  @discardableResult
  public func startRecognizeImage(data: Data,
    completion: @escaping (Result<Pattern, SDKError>) -> Void) -> RecognitionTask {
    start(image: true, completion: completion) { task in
      let work = try ImagePreparation.prepare(data, token: task.token)
      try task.check()
      return try Self.recognizeWatermark(pixels: work.pixels, width: work.width, height: work.height, prepared: true, token: task.token)
    }
  }
  private func start(image: Bool = false, completion: @escaping (Result<Pattern, SDKError>) -> Void,
                     operation: @escaping (RecognitionTask) throws -> (bits: String, mirror: Bool, watermark: Bool)) -> RecognitionTask {
    let task = RecognitionTask(completion: completion)
    task.deadline()
    DispatchQueue.global(qos: .userInitiated).async {
      do {
        try task.check()
        let mark = try operation(task)
        try task.check()
        self.query(bits: mark.bits, mirror: mark.mirror, watermark: mark.watermark, task: task, completion: task.finish)
      } catch let error as SDKError { task.finish(.failure(error)) }
      catch let error as ImagePreparation.RecognitionFailure { task.finish(.failure(SDKError(error.code, "Image processing failed"))) }
      catch { task.finish(.failure(SDKError(image ? "INVALID_INPUT" : "PROCESSING_ERROR", "Local processing failed"))) }
    }
    return task
  }
  static func recognizeBits(pixels: Data, width: Int, height: Int, prepared: Bool = false, format: String = "RGBA", token: OpaquePointer? = nil) throws -> String {
    try recognizeWatermark(pixels: pixels, width: width, height: height, prepared: prepared, format: format, token: token).bits
  }
  /// Share-id bits and the two rendering flags from the barcode.
  static func recognizeWatermark(pixels: Data, width: Int, height: Int, prepared: Bool = false, format: String = "RGBA", token: OpaquePointer? = nil) throws -> (bits: String, mirror: Bool, watermark: Bool) {
    guard ["RGB","RGBA"].contains(format), !prepared || format == "RGBA" else { throw SDKError("INVALID_INPUT","Invalid format") }
    let channels = format == "RGB" ? 3 : 4
    guard width > 0, height > 0, width <= Int(Int32.max), height <= Int(Int32.max),
      width <= Int.max / channels / height, pixels.count == width * height * channels
    else { throw SDKError("INVALID_INPUT", "Invalid RGBA input") }
    var out = pidan_watermark()
    let status = pixels.withUnsafeBytes { buffer in
      pidan_recognize_ex(
        buffer.bindMemory(to: UInt8.self).baseAddress, buffer.count, Int32(width), Int32(height),
        Int32(channels), prepared ? 1 : 0, &out, token)
    }
    guard status == 0 else {
      throw SDKError(
        status == 6 ? "CANCELLED" : status == 2 ? "NOT_RECOGNIZED" : status == 1 ? "INVALID_INPUT" : "PROCESSING_ERROR",
        "Watermark recognition failed")
    }
    let bits = withUnsafePointer(to: &out.share_id_bits) { pointer in
      String(cString: UnsafeRawPointer(pointer).assumingMemoryBound(to: CChar.self))
    }
    return (bits, out.mirror == 1, out.watermark == 1)
  }
  private func query(bits: String, mirror: Bool, watermark: Bool, task: RecognitionTask, completion: @escaping (Result<Pattern, SDKError>) -> Void) {
    let url: URL
    do { url = try queryURL(bits: bits) } catch let error as SDKError {
      completion(.failure(error)); return
    } catch {
      completion(.failure(SDKError("INVALID_INPUT", "Invalid configuration"))); return
    }
    var request = URLRequest(url: url)
    request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
    request.setValue("application/json", forHTTPHeaderField: "Accept")
    let configuration = URLSessionConfiguration.ephemeral
    configuration.timeoutIntervalForRequest = timeout
    configuration.timeoutIntervalForResource = timeout
    let delegate = QueryResponseDelegate { result in
      do {
        try task.check()
        let data = try result.get()
        completion(.success(try Self.decodeResponse(data, bits: bits, mirror: mirror, watermark: watermark)))
      } catch let error as SDKError { completion(.failure(error)) } catch {
        completion(.failure(SDKError("INVALID_RESPONSE", "Invalid response")))
      }
    }
    let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
    let requestTask = session.dataTask(with: request)
    task.attach(requestTask)
    requestTask.resume()
  }
  static func decodeResponse(_ raw: Data, bits: String, mirror: Bool = false, watermark: Bool = false) throws -> Pattern {
    func invalid() -> SDKError {
      SDKError("INVALID_RESPONSE", "Invalid or mismatched pattern response")
    }
    func integer(_ value: Any?, min: Int64, max: Int64) throws -> Int64 {
      guard let n = value as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID(),
        n.doubleValue.isFinite, n.doubleValue.rounded(.towardZero) == n.doubleValue,
        n.doubleValue >= Double(min), n.doubleValue <= Double(max)
      else { throw invalid() }
      return n.int64Value
    }
    // Contract: { author, name, M, N, createdAt, cells }. Other fields, including
    // shareIdBits, are ignored. The result carries the locally decoded bits.
    guard let data = try JSONSerialization.jsonObject(with: raw) as? [String: Any],
      let author = data["author"] as? String, let name = data["name"] as? String,
      let list = data["cells"] as? [Any]
    else { throw invalid() }
    let rows = Int(try integer(data["M"], min: 1, max: 104))
    let cols = Int(try integer(data["N"], min: 1, max: 104))
    let time = try integer(data["createdAt"], min: 0, max: 9_007_199_254_740_991)
    // cells: row-major M*N two-char uppercase hex codes; "00" empty, "01".."DD" palette.
    // Exactly two UTF-8 bytes each here; hex digits and range are validated by the core.
    guard list.count == rows * cols else { throw invalid() }
    var hex = [UInt8]()
    hex.reserveCapacity(rows * cols * 2)
    for item in list {
      guard let cell = item as? String, cell.utf8.count == 2 else { throw invalid() }
      hex.append(contentsOf: cell.utf8)
    }
    var indices = [UInt16](repeating: 0, count: rows * cols)
    let status = hex.withUnsafeBufferPointer { buffer in
      buffer.withMemoryRebound(to: CChar.self) { chars in
        pidan_decode_cells(
          chars.baseAddress, chars.count, Int32(rows), Int32(cols), &indices, indices.count)
      }
    }
    guard status == 0 else { throw invalid() }
    var cells = [[String?]]()
    for r in 0..<rows {
      cells.append(
        (0..<cols).map { c in
          let index = indices[r * cols + c]
          return index == 0 ? nil : String(cString: pidan_color_code(UInt32(index)))
        })
    }
    return Pattern(author: author, name: name, M: rows, N: cols, createdAt: time, cells: cells, shareIdBits: bits, mirror: mirror, watermark: watermark)
  }
}

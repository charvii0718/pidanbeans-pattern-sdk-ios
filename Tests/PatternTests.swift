import XCTest
import ImageIO
import CoreGraphics

@testable import PidanPattern

// No Content-Length. The oversized stream deliberately stays open so the test
// can only complete if the client rejects it while receiving the body.
private final class StreamedQueryFixture: URLProtocol {
  override class func canInit(with request: URLRequest) -> Bool { true }
  override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

  override func startLoading() {
    guard let url = request.url,
          let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1",
                                         headerFields: [:]) else { return }
    client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
    let chunk = Data(repeating: 32, count: 512 * 1024)
    client?.urlProtocol(self, didLoad: chunk)
    client?.urlProtocol(self, didLoad: chunk)
    if url.path == "/over" {
      client?.urlProtocol(self, didLoad: Data([32]))
    } else {
      client?.urlProtocolDidFinishLoading(self)
    }
  }

  override func stopLoading() {}
}

final class PatternTests: XCTestCase {
  func testQueryBodyLimitWithoutContentLength() {
    for path in ["exact", "over"] {
      let done = expectation(description: path)
      done.assertForOverFulfill = true
      let delegate = QueryResponseDelegate { result in
        switch (path, result) {
        case ("exact", .success(let data)): XCTAssertEqual(data.count, 1_048_576)
        case ("over", .failure(let error)): XCTAssertEqual(error.code, "INVALID_RESPONSE")
        default: XCTFail("Unexpected result for \(path)")
        }
        done.fulfill()
      }
      let config = URLSessionConfiguration.ephemeral
      config.protocolClasses = [StreamedQueryFixture.self]
      let session = URLSession(configuration: config, delegate: delegate, delegateQueue: nil)
      session.dataTask(with: URL(string: "https://fixture.invalid/\(path)")!).resume()
      wait(for: [done], timeout: 3)
      session.invalidateAndCancel()
    }
  }

  func testImagePreparation() throws {
    let root = ProcessInfo.processInfo.environment["PIDAN_FIXTURES"]!
    let pixels = try Data(contentsOf:URL(fileURLWithPath:root).appendingPathComponent("full-0.rgba"))
    let rgb = Data(pixels.enumerated().compactMap { $0.offset % 4 == 3 ? nil : $0.element })
    XCTAssertEqual(try PatternSDK.recognizeBits(pixels:rgb,width:800,height:200,format:"RGB"),try PatternSDK.recognizeBits(pixels:pixels,width:800,height:200))
    XCTAssertThrowsError(try PatternSDK.recognizeBits(pixels:rgb,width:800,height:200))
    let provider = CGDataProvider(data:pixels as CFData)!
    let image = CGImage(width:800,height:200,bitsPerComponent:8,bitsPerPixel:32,bytesPerRow:3200,
      space:CGColorSpace(name:CGColorSpace.sRGB)!,bitmapInfo:CGBitmapInfo(rawValue:CGImageAlphaInfo.last.rawValue),
      provider:provider,decode:nil,shouldInterpolate:false,intent:.defaultIntent)!
    let expected = try PatternSDK.recognizeBits(pixels:pixels,width:800,height:200)
    for type in ["public.png", "public.jpeg"] {
      let data = NSMutableData()
      let destination = CGImageDestinationCreateWithData(data,type as CFString,1,nil)!
      CGImageDestinationAddImage(destination,image,[kCGImagePropertyOrientation:6] as CFDictionary)
      XCTAssertTrue(CGImageDestinationFinalize(destination))
      let work = try ImagePreparation.prepare(data as Data)
      XCTAssertGreaterThan(work.width,work.height); XCTAssertEqual(work.height,200)
      XCTAssertEqual(try PatternSDK.recognizeBits(pixels:work.pixels,width:work.width,height:work.height,prepared:true),expected)
    }
    XCTAssertThrowsError(try ImagePreparation.prepare(Data("GIF89a".utf8)))
  }
  func testResponse() throws {
    let bits = String(repeating: "0", count: 44)
    let json =
      "{\"shareIdBits\":\"11111111111111111111111111111111111111111111\",\"author\":\"皮蛋\",\"name\":\"小猫\",\"M\":1,\"N\":2,\"createdAt\":1,\"cells\":[\"01\",\"00\"]}"
    let result = try PatternSDK.decodeResponse(Data(json.utf8), bits: bits)
    XCTAssertEqual(result.shareIdBits, bits)
    XCTAssertEqual(result.cells.count, 1)
    XCTAssertEqual(result.cells[0][0], "B3")
    XCTAssertNil(result.cells[0][1])
    XCTAssertFalse(result.mirror)
    XCTAssertFalse(result.watermark)
    let marked = try PatternSDK.decodeResponse(Data(json.utf8), bits: bits, mirror: true, watermark: true)
    XCTAssertTrue(marked.mirror)
    XCTAssertTrue(marked.watermark)
    // shareIdBits is not part of the contract and is ignored.
    XCTAssertNoThrow(try PatternSDK.decodeResponse(Data(json.utf8), bits: "other"))
    let grid = try PatternSDK.decodeResponse(
      Data("{\"author\":\"\",\"name\":\"\",\"M\":2,\"N\":2,\"createdAt\":0,\"cells\":[\"DD\",\"00\",\"13\",\"01\"]}".utf8),
      bits: bits)
    XCTAssertEqual(grid.cells, [["D4", nil], ["H1", "B3"]])
    let base = "{\"author\":\"\",\"name\":\"\",\"M\":1,\"N\":2,\"createdAt\":0,\"cells\":"
    for cells in ["null", "\"0100\"", "[\"01\"]", "[\"01\",\"dd\"]", "[\"1\",\"00\"]",
      "[\"01\",\"DE\"]", "[\"01\",\"0G\"]", "[\"01\",\"Ｄ1\"]", "[\"01\",1]"] {
      XCTAssertThrowsError(try PatternSDK.decodeResponse(Data((base + cells + "}").utf8), bits: bits))
    }
  }
  func testCancellationAndDeadline() throws {
    let sdk = try PatternSDK(baseURL: "http://127.0.0.1:1", apiKey: "key")
    let cancelled = expectation(description: "cancelled once")
    cancelled.assertForOverFulfill = true
    let pixels = Data(repeating: 255, count: 2000 * 2000 * 4)
    let task = sdk.startRecognize(pixels: pixels, width: 2000, height: 2000) { result in
      if case .failure(let error) = result { XCTAssertEqual(error.code, "CANCELLED") }
      else { XCTFail("Cancelled operation succeeded") }
      cancelled.fulfill()
    }
    task.cancel(); task.cancel()
    wait(for: [cancelled], timeout: 1)
    let timeout = expectation(description: "deadline")
    let began = Date()
    let timed = RecognitionTask { result in
      XCTAssertGreaterThanOrEqual(Date().timeIntervalSince(began), 14.5)
      if case .failure(let error) = result { XCTAssertEqual(error.code, "TIMEOUT") }
      else { XCTFail("Timed out operation succeeded") }
      timeout.fulfill()
    }
    timed.deadline()
    wait(for: [timeout], timeout: 18)
    timed.cancel()
    let failed = expectation(description: "independent task")
    sdk.startRecognize(pixels: Data(), width: 1, height: 1) { result in
      if case .failure(let error) = result { XCTAssertEqual(error.code, "INVALID_INPUT") }
      else { XCTFail("Invalid input succeeded") }
      failed.fulfill()
    }
    wait(for: [failed], timeout: 1)
  }
  func testCompletedTaskCannotBeCancelled() {
    var calls = 0
    let task = RecognitionTask { result in
      calls += 1
      if case .failure = result { XCTFail("Completed task changed outcome") }
    }
    task.finish(.success(Pattern(author: "", name: "", M: 1, N: 1, createdAt: 0, cells: [[nil]], shareIdBits: String(repeating: "0", count: 44), mirror: false, watermark: false)))
    task.cancel(); task.cancel()
    XCTAssertEqual(calls, 1)
  }
  func testBadPixels() {
    XCTAssertThrowsError(try PatternSDK.recognizeBits(pixels: Data(), width: 1, height: 1))
  }
  func testFixture() throws {
    guard let path = ProcessInfo.processInfo.environment["PIDAN_FIXTURES"] else {
      throw XCTSkip("Set PIDAN_FIXTURES for shared RGBA fixtures")
    }
    let bytes = try Data(
      contentsOf: URL(fileURLWithPath: path).appendingPathComponent("full-0.rgba"))
    XCTAssertEqual(
      try PatternSDK.recognizeBits(pixels: bytes, width: 800, height: 200),
      "10101101100101011111110001001001101100001110")
    XCTAssertFalse(try PatternSDK.recognizeWatermark(pixels: bytes, width: 800, height: 200).mirror)
    // full-1 carries the barcode mirror flag.
    let mirrored = try Data(
      contentsOf: URL(fileURLWithPath: path).appendingPathComponent("full-1.rgba"))
    let mark = try PatternSDK.recognizeWatermark(pixels: mirrored, width: 800, height: 200)
    XCTAssertEqual(mark.bits, "10101101100101011111110001001001101100001110")
    XCTAssertTrue(mark.mirror)
    XCTAssertFalse(mark.watermark)
    let corrected = try Data(contentsOf: URL(fileURLWithPath: path).appendingPathComponent("watermark-corrected.rgba"))
    let correctedMark = try PatternSDK.recognizeWatermark(pixels: corrected, width: 800, height: 200)
    XCTAssertTrue(correctedMark.watermark)
    XCTAssertTrue(correctedMark.mirror)
    XCTAssertEqual(correctedMark.bits, mark.bits)
  }
}

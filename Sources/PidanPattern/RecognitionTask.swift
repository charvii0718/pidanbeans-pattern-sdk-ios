import Foundation
#if canImport(PidanCore)
import PidanCore
#endif

/// Cancellation delivers one failure immediately. Work retains its token until it exits.
public final class RecognitionTask {
  private let lock = NSLock()
  private var completion: ((Result<Pattern, SDKError>) -> Void)?
  private var failure: SDKError?
  private var network: URLSessionTask?
  private var timer: DispatchWorkItem?
  let token: OpaquePointer?
  init(completion: @escaping (Result<Pattern, SDKError>) -> Void) {
    self.completion = completion
    token = pidan_cancel_create()
  }
  deinit { pidan_cancel_destroy(token) }
  public func cancel() { stop(SDKError("CANCELLED", "Recognition cancelled")) }
  private func stop(_ error: SDKError) {
    lock.lock()
    guard let callback = completion else { lock.unlock(); return }
    completion = nil; failure = error
    pidan_cancel_request(token)
    let request = network; network = nil
    timer?.cancel(); timer = nil
    lock.unlock()
    request?.cancel()
    callback(.failure(error))
  }
  func check() throws {
    lock.lock(); let error = failure; lock.unlock()
    if let error = error { throw error }
    if token == nil { throw SDKError("PROCESSING_ERROR", "Token allocation failed") }
  }
  func attach(_ request: URLSessionTask) {
    lock.lock()
    let stopped = completion == nil
    if !stopped { network = request }
    lock.unlock()
    if stopped { request.cancel() }
  }
  func deadline() {
    let work = DispatchWorkItem { [weak self] in self?.stop(SDKError("TIMEOUT", "Recognition timed out")) }
    lock.lock(); timer = work; lock.unlock()
    DispatchQueue.global().asyncAfter(deadline: .now() + .milliseconds(15000), execute: work)
  }
  func finish(_ result: Result<Pattern, SDKError>) {
    lock.lock()
    let callback = completion; completion = nil; network = nil
    timer?.cancel(); timer = nil
    lock.unlock()
    callback?(result)
  }
}

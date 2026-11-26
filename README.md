# Pidanbeans iOS / macOS 图纸识别 SDK

识别带有效条码的皮蛋豆图纸，输入本地 PNG/JPEG 或 RGB/RGBA 像素，获取图纸名称、作者和二维 MARD 色号数组。

## 接入

在 Xcode 中选择 **File → Add Package Dependencies**，输入下面的仓库地址，选择版本 `0.2.1`，再把 `PidanPattern` 产品添加到应用 target。

```text
https://github.com/charvii0718/pidanbeans-pattern-sdk-ios.git
```

最低版本为 iOS 15.1、macOS 11 和 Swift 5.7。iOS 支持 arm64 真机、arm64 和 x86_64 模拟器。

## 识别本地图片

```swift
import Foundation
import PidanPattern

let sdk = try PatternSDK(
    baseURL: "https://patterns.example.com",
    apiKey: "YOUR_QUERY_API_KEY"
)

let task = sdk.startRecognizeImage(at: URL(fileURLWithPath: localPath)) { result in
    switch result {
    case .success(let pattern):
        print(pattern.name, pattern.author)
    case .failure(let error):
        print(error.code, error.message)
    }
}
```

如果已拿到 PNG/JPEG 文件内容，可将其作为 `Data` 传入。

```swift
let task = sdk.startRecognizeImage(data: pngOrJpegData) { result in
    // 处理识图结果。
}
```

`PatternSDK` 可以复用。默认查询路径为 `/v1/patterns`，原图留在设备上，SDK 用本地解出的分享码查询图纸。更新界面前请切回主线程。

## 识别 RGB/RGBA 像素

```swift
sdk.startRecognize(pixels: rgba, width: width, height: height) { result in
    // 处理 RGBA 识图结果。
}

sdk.startRecognize(pixels: rgb, width: width, height: height, format: "RGB") { result in
    // 处理 RGB 识图结果。
}
```

RGBA 是默认格式，以 `Data` 传入紧密排列的非预乘像素，长度为 `width × height × 4`。RGB 长度为 `width × height × 3`。

## 结果数据结构

```swift
struct Pattern {
    let name: String         // 图纸名称
    let author: String       // 作者名称
    let M: Int               // 图纸行数
    let N: Int               // 图纸列数
    let cells: [[String?]]   // MARD 色号矩阵，nil 表示空格，按云端原始方向返回
    let mirror: Bool         // 当前图纸是否为镜像
    let watermark: Bool      // 当前图纸是否渲染作者水印，水印文字取 author
    let createdAt: Int64     // 图纸创建时间，Unix 毫秒时间戳
    let shareIdBits: String  // 二进制水印条
}
```

## 处理错误和取消

回调返回 `Result<Pattern, SDKError>`，通过 `SDKError.code` 区分失败原因。常见错误包括 `NOT_RECOGNIZED`、`NOT_FOUND`、`UNAUTHORIZED` 和 `TIMEOUT`。每次识图的全流程期限为 15 秒。页面离开或换图时，可以取消当前任务。

```swift
task.cancel()
```

## 许可证

[MIT](LICENSE)

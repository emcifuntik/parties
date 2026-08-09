import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2,
      let rawPID = Int32(CommandLine.arguments[1]) else {
    fputs("usage: macos-window-id PID\n", stderr)
    exit(2)
}

let options: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
guard let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID)
        as? [[String: Any]] else {
    exit(1)
}

var bestWindow: (id: Int, area: Double)?
for window in windows {
    guard (window[kCGWindowOwnerPID as String] as? Int32) == rawPID,
          (window[kCGWindowLayer as String] as? Int) == 0,
          let id = window[kCGWindowNumber as String] as? Int,
          let bounds = window[kCGWindowBounds as String] as? [String: Any],
          let width = bounds["Width"] as? Double,
          let height = bounds["Height"] as? Double else {
        continue
    }
    let area = width * height
    if bestWindow == nil || area > bestWindow!.area {
        bestWindow = (id, area)
    }
}

guard let result = bestWindow else { exit(1) }
print(result.id)

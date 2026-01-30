# LibPeer Unity Plugin

libpeer の Unity ネイティブプラグインです。WebRTC DataChannel とメディアストリーミングを提供します。

## Supported Platforms

| Platform | Architecture | Format | Size |
|----------|--------------|--------|------|
| macOS | arm64 + x86_64 | .bundle | ~4.0M |
| iOS Device | arm64 | .xcframework | ~4.3M |
| iOS Simulator | arm64 | .xcframework | ~4.4M |
| Android | arm64-v8a | .so | ~2.1M |
| Android | armeabi-v7a | .so | ~1.4M |
| Android | x86_64 | .so | ~2.2M |

## Package Structure

```
jp.co.mixi.libpeer/
├── package.json
├── README.md
├── Runtime/
│   ├── LibPeer.cs
│   └── jp.co.mixi.libpeer.asmdef
└── Plugins/
    ├── macOS/
    │   └── libpeer.bundle (Universal Binary)
    ├── iOS/
    │   ├── libpeer.xcframework
    │   └── Legacy/libpeer.a
    └── Android/
        ├── AndroidManifest.xml
        └── libs/
            ├── arm64-v8a/libpeer.so
            ├── armeabi-v7a/libpeer.so
            └── x86_64/libpeer.so
```

## Installation

### Unity Package Manager (Git URL)

1. Window > Package Manager を開く
2. "+" ボタン > "Add package from git URL..."
3. 以下を入力:
   ```
   https://github.com/dsugisawa-mixi/libpeer.git?path=upm/jp.co.mixi.libpeer
   ```

### Local Development

```bash
# リポジトリをクローン
git clone --recursive https://github.com/dsugisawa-mixi/libpeer.git
cd libpeer

# 全プラットフォームをビルド
./scripts/build_all.sh

# Unity プロジェクトの Packages/manifest.json に追加
{
  "dependencies": {
    "jp.co.mixi.libpeer": "file:../../libpeer/upm/jp.co.mixi.libpeer"
  }
}
```

## Building Native Libraries

### Requirements

- CMake 3.16+
- Xcode Command Line Tools (macOS/iOS)
- Android NDK 27+ (Android)

### Build Scripts

```bash
# macOS (Universal Binary: arm64 + x86_64)
./scripts/build_macos.sh

# iOS (Device + Simulator arm64)
./scripts/build_ios.sh

# Android (arm64-v8a, armeabi-v7a, x86_64)
export ANDROID_NDK_HOME=/path/to/ndk
./scripts/build_android.sh

# All platforms at once
./scripts/build_all.sh
```

### macOS Code Signing & Notarization

```bash
# 署名のみ
export CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
./scripts/build_macos.sh

# 署名 + Notarization
export CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
export NOTARIZE_PROFILE="your-profile-name"
./scripts/build_macos.sh
```

Notarization プロファイルの作成:
```bash
xcrun notarytool store-credentials "your-profile-name" \
  --apple-id "your@email.com" \
  --team-id "TEAMID" \
  --password "app-specific-password"
```

## Usage

```csharp
using Mixi.LibPeer;

// Initialize
LibPeerNative.peer_init();

// Create peer connection
var config = new PeerConfiguration
{
    DataChannel = DataChannelType.Binary,
    IceServers = new[]
    {
        new IceServer { Urls = "stun:stun.l.google.com:19302" }
    }
};

// ... WebRTC operations

// Cleanup
LibPeerNative.peer_deinit();
```

## Notes

- **iOS**: Unity 2021.3+ では xcframework を使用。古いバージョンでは `Legacy/libpeer.a` を使用
- **Android**: API Level 24+ が必要 (getifaddrs 依存)
- **macOS**: Hardened Runtime 対応済み。配布時は署名 + Notarization を推奨

## License

MIT License

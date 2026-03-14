# SubtitlesOctopus（HXLoLi 定制版）

[![Actions Status](https://github.com/libass/JavascriptSubtitlesOctopus/actions/workflows/emscripten.yml/badge.svg)](https://github.com/libass/JavascriptSubtitlesOctopus/actions/workflows/emscripten.yml?query=branch%3Amaster+event%3Apush)

基于 [JavascriptSubtitlesOctopus](https://github.com/libass/JavascriptSubtitlesOctopus) 的定制 fork，
专为 [HXLoLi](https://github.com/HXLoLi/HXLoLi) 音乐播放器的 ASS 字幕/歌词渲染而优化。

本引擎使用 [libass](https://github.com/libass/libass) + WebAssembly，在浏览器中高性能渲染 ASS/SSA 字幕，
并扩展支持了 VSFilterMod 的多项高级特效标签，能够正确渲染复杂的 K-ON 风格 ASS 歌词特效。

## ✨ 相比上游的增强特性

### VSFilterMod 扩展标签支持

| 标签 | 说明 | 状态 |
|------|------|------|
| `\1img(path)` ~ `\4img(path)` | 图片纹理填充（VSFilterMod Nimg） | ✅ 原生支持 |
| `\fsvp` | 垂直位置偏移（Vertical Shift in Pixels） | ✅ 已修复 |
| `\fsc` | 全局缩放 | ✅ |
| `\frs` | 相对旋转 | ✅ |
| `\z` | Z 轴深度 | ✅ |
| `\rnd` | 随机偏移 | ✅ |
| `\distort` | 透视变形 | ✅ |
| `\$vc` / `\$va` | 顶点颜色/透明度渐变 | ✅ |
| `\moves3(x1,y1,x2,y2,x3,y3)` | 三点二次贝塞尔曲线移动 | ✅ |
| `\moves4(x1,y1,x2,y2,x3,y3,x4,y4)` | 四点三次贝塞尔曲线移动 | ✅ |

### 图片渲染增强

- **`\1img` 原生渲染**：通过 stb_image 在 WASM 中直接加载 PNG/JPEG/BMP 图片，无需前端 JS 叠加渲染
- **图片纹理通道**：支持 4 个独立通道（primary / secondary / border / shadow）的图片纹理映射
- **逆变换矩阵**：图片纹理正确跟随文字的旋转、缩放、错切等变换，避免被拉伸或切割
- **图片缓存**：内置 LRU 缓存，避免重复加载相同图片

### 其他改进

- **`\fsvp` 修复**：从错误的水平错切实现修正为正确的垂直位置偏移，修复图片浮动和文字弹跳效果
- **`\moves3` / `\moves4`**：实现 VSFilterMod 的贝塞尔曲线移动，支持文字沿曲线路径运动
- **移除 git 子模块**：所有 lib 库代码纳入主仓库直接维护，patch 已直接应用到源码
- **CI/CD 优化**：缓存 key 包含 libass 源码 hash，确保代码变更后缓存正确失效

## 功能特性

- 支持绝大部分 SSA/ASS 特性（libass 支持的所有功能）
- 支持 VSFilterMod 扩展标签（图片纹理、贝塞尔移动等）
- 支持所有 OpenType 和 TrueType 字体（包括 woff2 字体）
- 基于 WebAssembly 高性能运行
- 使用 Web Workers 后台渲染，不阻塞主线程
- 不使用 DOM 操作，在单个 Canvas 上渲染字幕
- 易于集成 —— 只需连接到 video 或 canvas 元素

## 包含的库

| 库 | 用途 |
|---|---|
| libass | ASS/SSA 字幕渲染核心 |
| freetype | 字体光栅化 |
| harfbuzz | 文本整形引擎 |
| fribidi | Unicode 双向文本算法 |
| fontconfig | 字体匹配与配置 |
| expat | XML 解析（fontconfig 依赖） |
| brotli | WOFF2 字体解压 |

## 使用方法

### 基本用法

```javascript
var options = {
    video: document.getElementById('video'),
    subUrl: '/test/test.ass',
    fonts: ['/test/font-1.ttf', '/test/font-2.ttf'],
    workerUrl: '/libassjs-worker.js',
};
var instance = new SubtitlesOctopus(options);
```

### 仅使用 Canvas

```javascript
var options = {
    canvas: document.getElementById('canvas'),
    subUrl: '/test/test.ass',
    workerUrl: '/libassjs-worker.js',
};
var instance = new SubtitlesOctopus(options);
instance.setCurrentTime(15); // 渲染 00:15 时刻的字幕
```

### 加载 `\1img` 图片

本引擎原生支持 `\1img` 图片渲染。只需将 ASS 引用的图片文件写入 Worker 的虚拟文件系统：

```javascript
// 将图片文件写入 worker 虚拟 FS（在加载 ASS 之前）
instance.writeToFile('/path/to/image.png', imageArrayBuffer);

// ASS 中的 \1img(path/to/image.png) 将自动渲染
```

### 动态切换字幕

```javascript
instance.setTrackByUrl('/test/railgun_op.ass');  // 通过 URL
instance.setTrack(assContent);                    // 通过内容
instance.freeTrack();                             // 移除字幕
```

### 清理

```javascript
instance.dispose();
```

## 配置选项

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `video` | 关联的 video 元素 | - |
| `canvas` | 渲染目标 canvas | 自动创建 |
| `subUrl` | 字幕文件 URL | - |
| `subContent` | 字幕文件内容 | - |
| `workerUrl` | Worker JS 文件 URL | `libassjs-worker.js` |
| `fonts` | 字体文件 URL 数组 | `[]` |
| `availableFonts` | 字体名 → URL 映射 | `{}` |
| `fallbackFont` | 回退字体 URL | Liberation Sans |
| `timeOffset` | 字幕时间偏移（秒） | `0` |
| `renderMode` | 渲染模式 | `wasm-blend` |
| `targetFps` | 目标帧率 | `24` |
| `prescaleFactor` | 预缩放因子 | `1.0` |
| `maxRenderHeight` | 最大渲染高度 | `0`（无限制） |
| `dropAllAnimations` | 丢弃所有动画标签 | `false` |
| `debug` | 调试模式 | `false` |

### 渲染模式

- **`wasm-blend`**（默认）：在 WebAssembly 中混合位图，性能最优
- **`js-blend`**：在 JavaScript 中混合位图
- **`lossy`**：实验性有损渲染模式，使用 `createImageBitmap` 异步渲染

## 如何构建

### 依赖

- git、make、python3、cmake、pkgconfig
- emscripten（配置好环境变量）
- patch、libtool、autotools（autoconf、automake、autopoint）
- gettext、ragel、itstool、gperf
- python3-ply、licensecheck

### 获取源码

```bash
git clone https://github.com/HXLoLi/JavascriptSubtitlesOctopus.git
```

### Docker 构建

```bash
./run-docker-build.sh
# 构建产物在 dist/js/
```

### 本地构建

```bash
make
# macOS: LIBTOOLIZE=glibtoolize make
# 构建产物在 dist/js/
```

## 许可证

LGPL-2.1-or-later AND (FTL OR GPL-2.0-or-later) AND MIT AND MIT-Modern-Variant AND ISC AND NTP AND Zlib AND BSL-1.0

## 致谢

- [libass](https://github.com/libass/libass) — ASS/SSA 字幕渲染库
- [JavascriptSubtitlesOctopus](https://github.com/libass/JavascriptSubtitlesOctopus) — 原版浏览器端 libass 封装
- VSFilterMod — 扩展标签规范参考

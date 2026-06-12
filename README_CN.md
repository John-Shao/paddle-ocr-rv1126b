# paddle-ocr-rv1126b

中文 | [English](README.md)

RV1126B 上的纯 C/C++ OCR 文字识别和语音播报程序：

```text
板端摄像头 snapshot -> PP-OCRv4 RKNN -> stdout 文本 -> melottsd 播放
```

识别到文字时播放识别文本；未识别到文字时播放“没有识别到文字信息”。

## 约束

- 板端只运行 C/C++，不运行 Python。
- ONNX -> RKNN 只在 Ubuntu 交叉编译机上离线完成。
- 板端默认不直接打开 `/dev/video-camera0`，避免和 `camera_core_d` 抢摄像头。
- 默认通过板内接口抓图：`http://127.0.0.1:8080/api/v1/snapshot.jpg`。
- 默认通过 `/run/melottsd.sock` 调用板端 `melotts-rv1126b` 播放语音。

## 环境

交叉编译 Ubuntu：

```text
host: 192.168.126.129
user: alientek
repo: /home/alientek/paddle-ocr-rv1126b
```

RV1126B 开发板：

```text
host: 192.168.10.90
user: root
app:  /data/ppocr-text
```

密码不写入仓库。Windows PowerShell 当前会话可临时设置：

```powershell
$env:UBUNTU_PW="..."
$env:BOARD_PW="..."
```

## 目录

```text
CMakeLists.txt                  C++ 程序构建配置
src/ppocr_text.cc               板端 OCR CLI
scripts/build_deploy_ubuntu.sh  Ubuntu 上转换、交叉编译、部署
scripts/sync_to_ubuntu.ps1      Windows -> Ubuntu 同步源码/模型
scripts/build_deploy_from_windows.ps1
scripts/run_board.ps1           Windows 远程运行板端程序
tools/*.ps1                     单独转换/准备 Model Zoo demo 的辅助脚本
models/ppocrv4_det.onnx         PP-OCRv4 检测 ONNX
models/ppocrv4_rec.onnx         PP-OCRv4 识别 ONNX
```

生成物：

```text
models/*.rknn   由 RKNN-Toolkit2 生成，不提交
build/          Ubuntu 构建目录，不提交
install/        Ubuntu 安装/部署包，不提交
```

## 模型

使用瑞芯微 Model Zoo 已适配 RV1126B 的 PP-OCRv4 组合：

```text
ppocrv4_det.onnx -> ppocrv4_det_i8.rknn
ppocrv4_rec.onnx -> ppocrv4_rec_fp16.rknn
```

检测模型使用 INT8，识别模型使用 FP16。RKNN 转换脚本复用：

```text
/home/alientek/rknn_model_zoo/examples/PPOCR/PPOCR-Det/python/convert.py
/home/alientek/rknn_model_zoo/examples/PPOCR/PPOCR-Rec/python/convert.py
```

## 一键构建部署

在 Windows PowerShell：

```powershell
$env:UBUNTU_PW="<ubuntu-password>"
$env:BOARD_PW="<board-password>"
.\scripts\build_deploy_from_windows.ps1
```

这个命令会：

1. 同步本仓库到 Ubuntu：`/home/alientek/paddle-ocr-rv1126b`
2. 如缺少 RKNN，使用 RKNN-Toolkit2 转换 ONNX
3. 使用 SDK Buildroot 工具链交叉编译 `ppocr_text`
4. 部署到板子：`/data/ppocr-text`

只构建不部署：

```powershell
.\scripts\build_deploy_from_windows.ps1 -NoDeploy
```

## Ubuntu 上手动构建

```sh
cd /home/alientek/paddle-ocr-rv1126b
BOARD_PW=<board-password> scripts/build_deploy_ubuntu.sh
```

常用环境变量：

```sh
REPO=$HOME/paddle-ocr-rv1126b
MODEL_ZOO=$HOME/rknn_model_zoo
SDK=$HOME/atk-dlrv1126b-sdk
RKNN_PYTHON=$HOME/rknnenv/bin/python
BOARD=root@192.168.10.90
BOARD_DIR=/data/ppocr-text
SKIP_DEPLOY=1
```

## 板端运行

Windows 远程运行：

```powershell
$env:BOARD_PW="<board-password>"
.\scripts\run_board.ps1

# 带参数运行，例如识别静态图片且不播报
.\scripts\run_board.ps1 -ProgramArgs "--image test.jpg --no-tts"
```

直接在板子上运行：

```sh
cd /data/ppocr-text
LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text
```

输出规则：

```text
识别到文字：逐行输出 UTF-8 文本
未识别到文字：[NO_TEXT]
语音播报：默认开启
```

其他命令：

```sh
# 识别静态图片
LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text --image test.jpg

# 指定 snapshot URL
LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text --snapshot-url http://127.0.0.1:8080/api/v1/snapshot.jpg

# 调试模式，输出 RKNN tensor 信息
LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text --verbose

# 只输出文本，不调用 TTS
LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text --no-tts

# 指定 melottsd socket 和播放优先级
LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text --tts-socket /run/melottsd.sock --tts-priority 4

# 仅在摄像头未被 camera_core_d 占用时使用
LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text --camera /dev/video-camera0 --width 1280 --height 720
```

## 已验证

板端 `/data/ppocr-text` 已部署成功。

```text
./ppocr_text --image test.jpg
```

可识别瑞芯微 PPOCR-System 测试图中的中文文本。

```text
./ppocr_text
```

可通过 `camera_core_d` snapshot 抓取当前摄像头画面，并调用 melottsd 播放识别文本。
当前画面无文字时输出 `[NO_TEXT]`，并播放“没有识别到文字信息”。

## 文档同步

保持 [README.md](README.md) 和本文档结构同步：

- 保持相同章节顺序。
- 修改文档时同时更新中英文两个版本。
- 命令、路径、模型名和已验证行为必须一致，除非只是语言表达差异。

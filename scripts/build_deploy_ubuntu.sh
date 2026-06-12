#!/bin/bash
set -e

REPO=${REPO:-$HOME/paddle-ocr-rv1126b}
MODEL_ZOO=${MODEL_ZOO:-$HOME/rknn_model_zoo}
SDK=${SDK:-$HOME/atk-dlrv1126b-sdk}
RKNN_PYTHON=${RKNN_PYTHON:-$HOME/rknnenv/bin/python}
BOARD=${BOARD:-root@192.168.10.90}
BOARD_DIR=${BOARD_DIR:-/data/ppocr-text}
BUILD_DIR=${BUILD_DIR:-$REPO/build}
INSTALL_DIR=${INSTALL_DIR:-$REPO/install}
SKIP_DEPLOY=${SKIP_DEPLOY:-0}

BR="$SDK/buildroot/output/alientek_rv1126b/host"
SYSROOT="$BR/aarch64-buildroot-linux-gnu/sysroot"
CC="$BR/bin/aarch64-buildroot-linux-gnu-gcc"
CXX="$BR/bin/aarch64-buildroot-linux-gnu-g++"

cd "$REPO"

for path in "$MODEL_ZOO/examples/PPOCR/PPOCR-System/cpp" "$SDK" "$CC" "$CXX" "$RKNN_PYTHON"; do
    if [ ! -e "$path" ]; then
        echo "missing required path: $path" >&2
        exit 1
    fi
done

mkdir -p models

if [ ! -f models/ppocrv4_det.onnx ] || [ ! -f models/ppocrv4_rec.onnx ]; then
    echo "missing ONNX models under $REPO/models" >&2
    exit 1
fi

if [ ! -f models/ppocrv4_det_i8.rknn ] || [ ! -f models/ppocrv4_rec_fp16.rknn ]; then
    echo "== convert ONNX -> RKNN for rv1126b =="
    (
        cd "$MODEL_ZOO/examples/PPOCR/PPOCR-Det/python"
        "$RKNN_PYTHON" convert.py \
            "$REPO/models/ppocrv4_det.onnx" rv1126b i8 "$REPO/models/ppocrv4_det_i8.rknn"
    )
    (
        cd "$MODEL_ZOO/examples/PPOCR/PPOCR-Rec/python"
        "$RKNN_PYTHON" convert.py \
            "$REPO/models/ppocrv4_rec.onnx" rv1126b fp "$REPO/models/ppocrv4_rec_fp16.rknn"
    )
fi

echo "== build ppocr_text =="
rm -rf "$BUILD_DIR" "$INSTALL_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. \
    -DMODEL_ZOO_ROOT="$MODEL_ZOO" \
    -DTARGET_SOC=rv1126b \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_SYSROOT="$SYSROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
make -j4
make install

if [ "$SKIP_DEPLOY" = "1" ]; then
    echo "== skip deploy =="
    echo "Install dir: $INSTALL_DIR"
    exit 0
fi

echo "== deploy -> $BOARD:$BOARD_DIR =="
if [ -n "${BOARD_PW:-}" ]; then
    if ! command -v sshpass >/dev/null 2>&1; then
        echo "BOARD_PW was set but sshpass is not installed" >&2
        exit 1
    fi
    SSH=(sshpass -p "$BOARD_PW" ssh -o StrictHostKeyChecking=no)
    SCP=(sshpass -p "$BOARD_PW" scp -o StrictHostKeyChecking=no)
else
    SSH=(ssh -o StrictHostKeyChecking=no)
    SCP=(scp -o StrictHostKeyChecking=no)
fi
"${SSH[@]}" "$BOARD" "mkdir -p '$BOARD_DIR'"
"${SCP[@]}" -r "$INSTALL_DIR/"* "$BOARD:$BOARD_DIR/"

echo "DONE"
echo "Run on board:"
echo "  cd $BOARD_DIR"
echo "  LD_LIBRARY_PATH=./lib:/usr/lib ./ppocr_text"

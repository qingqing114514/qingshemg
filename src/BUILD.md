# 编译说明

## 环境

- Termux（或任意带 Android NDK 的环境）
- `aarch64-linux-android-clang`

## 编译

在源码目录（`src/`）执行：

```sh
aarch64-linux-android-clang -shared -fPIC -O2 -DNDEBUG \\
  -Wno-incompatible-function-pointer-types \\
  -I../include -Isrc \\
  -o libmodule.android.arm64.so core.c \\
  ../include/tefkernel-cpp-wrapper/tefkernel/tef_api_imp.c \\
  -llog -ldl
```

## 注意

- **必须带 `-DNDEBUG`**，否则部分断言会导致异常。
- **必须把 `tef_api_imp.c` 一起编译**。TEF 的 patchlib 函数是函数指针变量，不带它编译会变成 UND 符号，加载时内核报 `Failed to open dynamic library`。
- 编译后可用 `nm -D libmodule.android.arm64.so | grep " U "` 检查，UND 只应剩 libc 系列（`__android_log_print`、`fopen`、`mkdir`、`snprintf` 等），不应出现 `patchlib_*` / `tefstd_*`。

## 打包

编译出的 so 通过 TEFPkg 工具打包成 `*_extension.tefpkg`，外层再套 `Info.json` + `Manifest.json` 组成安装 zip。

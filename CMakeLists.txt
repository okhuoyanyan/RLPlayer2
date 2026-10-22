cmake_minimum_required(VERSION 3.20)
project(RLPlayer LANGUAGES CXX)

# ---------------- 分发兼容性基线（2026-10-21 事故：-march=znver5 在 5900X 上 0xC000001D） ----------------
# RTA_ARCH 默认 x86-64-v2（2009 年以后的 x86-64 都能跑），绝不把 AVX-512 带进分发版。
# 本机开发想要极限性能可显式 -DRTA_ARCH=znver5，但那样产物只能在本机跑。
set(RTA_ARCH "x86-64-v2" CACHE STRING "CPU baseline for the distribution build (x86-64-v2 default)")
# 静态链接 MSVC CRT：分发版不再要求用户安装 VC++ 2015-2022 运行库（否则干净系统直接 0xC000007B）
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus)
  add_compile_options($<$<CONFIG:Release>:/O2>)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  # 分发基线：不要改成 native/znver5 —— 那会把 AVX-512 编进产物，老 CPU 直接非法指令崩溃
  # -ffast-math 的 -ffinite-math-only/-fno-honor-nans 会让"NaN 防护"被整段优化掉
  # （实测：finite_bits_u64 在注入 NaN 时仍返回 true）。保留其余 fast-math 优化，
  # 但恢复 NaN/Inf 语义，让 DSP 环路里的非有限值检查真正生效。
  add_compile_options(-O3 -march=${RTA_ARCH} -ffast-math -fno-finite-math-only
                      -fhonor-nans -fhonor-infinities -fno-math-errno)
  add_compile_options(-fno-strict-aliasing)
  add_compile_options($<$<CONFIG:Release>:-DNDEBUG>)
endif()

# ---------------- 工具：WASAPI 能力探针 ----------------
add_executable(wasapi_probe tools/wasapi_probe.cpp)
target_link_libraries(wasapi_probe PRIVATE ole32 uuid mmdevapi)

# ---------------- 工具：WebView2 环境探针（不依赖 SDK）----------------
add_executable(wv2_probe tools/wv2_probe.cpp)
target_link_libraries(wv2_probe PRIVATE ole32 uuid)

# ---------------- 工具：逐像素测窗口边缘（只读诊断，UI 边缘缺陷的唯一判据）----------------
# 视觉模型会吞掉 1-4px 窄条（第十五轮教训），边缘问题一律以本工具的数值为准。
add_executable(pxprobe tools/pxprobe.cpp)
target_link_libraries(pxprobe PRIVATE user32 gdi32)

# ---------------- 工具：ASIO 能力探针（分步验证 vtable，不发声）----------------
add_executable(asio_probe tools/asio_probe.cpp)
target_include_directories(asio_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(asio_probe PRIVATE ole32 uuid)

# ---------------- 播放器 CLI ----------------
add_executable(rtaplay tools/rtaplay.cpp)
target_link_libraries(rtaplay PRIVATE rtadsp ole32 uuid mmdevapi ws2_32)

# ---------------- UPnP 设备发现（只读探测） ----------------
add_executable(upnp_scan tools/upnp_scan.cpp)
target_link_libraries(upnp_scan PRIVATE ws2_32)

# ---------------- GPU 升频核验证 ----------------
add_executable(gpu_resample_test tools/gpu_resample_test.cpp)
target_link_libraries(gpu_resample_test PRIVATE ws2_32)

# ---------------- 模拟 Navidrome（集成测试用）----------------
add_executable(mockemby tools/mockemby.cpp)
target_include_directories(mockemby PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(mockemby PRIVATE winhttp ws2_32)

add_executable(mocksub tools/mocksub.cpp)
target_include_directories(mocksub PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(mocksub PRIVATE winhttp ws2_32)

# ---------------- HTTP 客户端自检 ----------------
add_executable(httptest tools/httptest.cpp)
target_include_directories(httptest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(httptest PRIVATE winhttp ws2_32)

# ---------------- 桌面启动器（Win32 GUI，双击即用）----------------
# 产品名：RLPlayer
add_executable(rta_launcher WIN32 tools/launcher.cpp)
set_target_properties(rta_launcher PROPERTIES OUTPUT_NAME "RLPlayer")
# ole32/uuid/mmdevapi:启动横幅只读枚举输出端点(wasapi_out.h);dbghelp:崩溃转储(crash_dump.h)
target_link_libraries(rta_launcher PRIVATE ws2_32 shell32 user32 ole32 uuid mmdevapi dbghelp)
target_include_directories(rta_launcher PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(rta_launcher PRIVATE UNICODE _UNICODE)
# 内置 WebView2：官方 SDK 头文件（third_party/webview2/include），运行时用同目录的 WebView2Loader.dll
target_include_directories(rta_launcher PRIVATE ${CMAKE_SOURCE_DIR}/third_party/webview2/include)
# WebView2Loader.dll 必须和 exe 同目录（内置界面靠它创建 WebView2）
add_custom_command(TARGET rta_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/third_party/webview2/x64/WebView2Loader.dll"
          "$<TARGET_FILE_DIR:rta_launcher>/WebView2Loader.dll")

# 图标 + 版本信息：用 Windows SDK 的 rc.exe 编译 res/rlplayer.rc
# （clang/lld 自带不了资源编译器；找不到 rc 就跳过，程序仍可运行，只是没图标）
find_program(RC_EXE NAMES rc
             PATHS "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
             NO_DEFAULT_PATH)
if(NOT RC_EXE)
  find_program(RC_EXE NAMES rc)
endif()
if(RC_EXE)
  set(RL_ICON_RES ${CMAKE_BINARY_DIR}/rlplayer.res)
  add_custom_command(
    OUTPUT ${RL_ICON_RES}
    COMMAND "${RC_EXE}" /nologo /fo "${RL_ICON_RES}"
            /I "${CMAKE_SOURCE_DIR}/res"
            /I "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um"
            /I "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared"
            "${CMAKE_SOURCE_DIR}/res/rlplayer.rc"
    DEPENDS ${CMAKE_SOURCE_DIR}/res/rlplayer.rc ${CMAKE_SOURCE_DIR}/res/rlplayer.ico
    VERBATIM)
  set_source_files_properties(${RL_ICON_RES} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
  add_custom_target(rl_icon_res DEPENDS ${RL_ICON_RES})
  add_dependencies(rta_launcher rl_icon_res)
  target_sources(rta_launcher PRIVATE ${RL_ICON_RES})
  message(STATUS "RLPlayer icon: ${RC_EXE}")
else()
  message(WARNING "找不到 rc.exe，跳过图标与版本信息")
endif()

# ---------------- 多格式解码验证 ----------------
add_executable(aidecode tools/aidecode.cpp)
target_link_libraries(aidecode PRIVATE rtadsp)

# ---------------- 网页播放器 ----------------
# dbghelp:崩溃转储(crash_dump.h,SetUnhandledExceptionFilter + MiniDumpWriteDump)
add_executable(rtaweb tools/rtaweb.cpp)
target_link_libraries(rtaweb PRIVATE rtadsp ws2_32 winmm avrt dbghelp)

# ---------------- NAA 端点发现 ----------------
add_executable(naa_scan tools/naa_scan.cpp)
target_link_libraries(naa_scan PRIVATE ws2_32)

# NAA 控制通道黑盒探针（只做 TCP 交换，不发音频）
add_executable(naa_probe tools/naa_probe.cpp)
target_link_libraries(naa_probe PRIVATE ws2_32)

# ---------------- 元数据工具（ffprobe 标签 + 文件名/LLM 解析 + MetaBase 存取；不发声音，
# parse 不带 --llm 时除本地 ffprobe 外零网络请求） ----------------
add_executable(rtatag tools/rtatag.cpp)
target_link_libraries(rtatag PRIVATE rtadsp)

# ---------------- DoP 校验工具 ----------------
add_executable(dopinfo tools/dopinfo.cpp)
target_link_libraries(dopinfo PRIVATE rtadsp)

# ---------------- 逐位比对工具 ----------------
add_executable(wavcmp tools/wavcmp.cpp)
target_link_libraries(wavcmp PRIVATE rtadsp)

# ---------------- RTANet 端点（跑在接 DAC 的机器上） ----------------
add_executable(rtanet_rx tools/rtanet_rx.cpp)
target_link_libraries(rtanet_rx PRIVATE rtadsp ole32 uuid mmdevapi ws2_32)

# ---------------- DSP 核心（头文件库） ----------------
add_library(rtadsp INTERFACE)
target_include_directories(rtadsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------- 测试 ----------------
enable_testing()
add_executable(test_resampler tests/test_resampler.cpp)
target_link_libraries(test_resampler PRIVATE rtadsp)
add_test(NAME resampler COMMAND test_resampler)

add_executable(test_dither tests/test_dither.cpp)
target_link_libraries(test_dither PRIVATE rtadsp)
add_test(NAME dither COMMAND test_dither)

add_executable(test_modulator tests/test_modulator.cpp)
target_link_libraries(test_modulator PRIVATE rtadsp)
add_test(NAME modulator COMMAND test_modulator)

add_executable(test_filters tests/test_filters.cpp)
target_link_libraries(test_filters PRIVATE rtadsp)
add_test(NAME filters COMMAND test_filters)

add_executable(test_pipeline tests/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE rtadsp)
add_test(NAME pipeline COMMAND test_pipeline)

add_executable(test_conv_matrix tests/test_conv_matrix.cpp)
target_link_libraries(test_conv_matrix PRIVATE rtadsp)
add_test(NAME conv_matrix COMMAND test_conv_matrix)

add_executable(test_volume_eq tests/test_volume_eq.cpp)
target_link_libraries(test_volume_eq PRIVATE rtadsp)
add_test(NAME volume_eq COMMAND test_volume_eq)

add_executable(test_chain tests/test_chain.cpp)
target_link_libraries(test_chain PRIVATE rtadsp)
add_test(NAME chain COMMAND test_chain)

add_executable(test_rtanet tests/test_rtanet.cpp)
target_link_libraries(test_rtanet PRIVATE rtadsp ws2_32)
add_test(NAME rtanet COMMAND test_rtanet)

add_executable(test_rtanet_e2e tests/test_rtanet_e2e.cpp)
target_link_libraries(test_rtanet_e2e PRIVATE rtadsp ws2_32)
add_test(NAME rtanet_e2e COMMAND test_rtanet_e2e)

add_executable(test_drift tests/test_drift.cpp)
target_link_libraries(test_drift PRIVATE rtadsp)
add_test(NAME drift COMMAND test_drift)

add_executable(test_clock_loop tests/test_clock_loop.cpp)
target_link_libraries(test_clock_loop PRIVATE rtadsp)
add_test(NAME clock_loop COMMAND test_clock_loop)

add_executable(test_status tests/test_status.cpp)
target_link_libraries(test_status PRIVATE rtadsp)
add_test(NAME status COMMAND test_status)

add_executable(test_loudness tests/test_loudness.cpp)
target_link_libraries(test_loudness PRIVATE rtadsp)
add_test(NAME loudness COMMAND test_loudness)

add_executable(test_emby tests/test_emby.cpp)
target_include_directories(test_emby PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_emby PRIVATE winhttp ws2_32)
add_test(NAME emby COMMAND test_emby)

add_executable(test_subsonic tests/test_subsonic.cpp)
target_include_directories(test_subsonic PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_subsonic PRIVATE winhttp ws2_32)
add_test(NAME subsonic COMMAND test_subsonic)

add_executable(test_md5 tests/test_md5.cpp)
target_include_directories(test_md5 PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME md5 COMMAND test_md5)

add_executable(test_dop tests/test_dop.cpp)
target_link_libraries(test_dop PRIVATE rtadsp)
add_test(NAME dop COMMAND test_dop)

add_executable(test_dop_stream tests/test_dop_stream.cpp)
target_link_libraries(test_dop_stream PRIVATE rtadsp)
add_test(NAME dop_stream COMMAND test_dop_stream)

add_executable(test_parallel tests/test_parallel.cpp)
target_link_libraries(test_parallel PRIVATE rtadsp)
add_test(NAME parallel COMMAND test_parallel)

add_executable(test_two_stage tests/test_two_stage.cpp)
target_link_libraries(test_two_stage PRIVATE rtadsp)
add_test(NAME two_stage COMMAND test_two_stage)

add_executable(test_isp tests/test_isp.cpp)
target_link_libraries(test_isp PRIVATE rtadsp)
add_test(NAME isp COMMAND test_isp)

add_executable(test_library tests/test_library.cpp)
target_include_directories(test_library PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME library COMMAND test_library)

add_executable(test_stream_baseline tests/test_stream_baseline.cpp)
target_link_libraries(test_stream_baseline PRIVATE rtadsp)
add_test(NAME stream_baseline COMMAND test_stream_baseline)

# 每声道增益 / 多声道路由
add_executable(test_channel_gain tests/test_channel_gain.cpp)
target_link_libraries(test_channel_gain PRIVATE rtadsp)
add_test(NAME channel_gain COMMAND test_channel_gain)

# GPU 卷积桥接：DLL 里的符号必须真的被取到（Windows 分支曾漏 GetProcAddress）
add_executable(test_gpu_bridge tests/test_gpu_bridge.cpp)
target_link_libraries(test_gpu_bridge PRIVATE rtadsp)
add_test(NAME gpu_bridge COMMAND test_gpu_bridge)

# DSF / DFF 原生读取（ROADMAP P0-1）：写回读逐位一致 + 文件布局独立校验
add_executable(test_dsd_reader tests/test_dsd_reader.cpp)
target_link_libraries(test_dsd_reader PRIVATE rtadsp)
add_test(NAME dsd_reader COMMAND test_dsd_reader)

# 满幅 PCM 走完整管线的稳定性（2026-10-21 现场"播几秒变噪音"回归）
add_executable(test_fullscale tests/test_fullscale.cpp)
target_link_libraries(test_fullscale PRIVATE rtadsp)
add_test(NAME fullscale COMMAND test_fullscale)

# DSD 通路（P0-2 素材生成 / P0-3 DirectSDM / P0-4 抽取器）
add_executable(test_dsd_path tests/test_dsd_path.cpp)
target_link_libraries(test_dsd_path PRIVATE rtadsp)
add_test(NAME dsd_path COMMAND test_dsd_path)

# 元数据三件套（P0）：store / parse / llm —— 全离线（llm 只测纯函数链路与空 key 快速失败，不发网络）
add_executable(test_metastore tests/test_metastore.cpp)
target_include_directories(test_metastore PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME metastore COMMAND test_metastore)

add_executable(test_metaparse tests/test_metaparse.cpp)
target_include_directories(test_metaparse PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME metaparse COMMAND test_metaparse)

add_executable(test_metallm tests/test_metallm.cpp)
target_include_directories(test_metallm PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_metallm PRIVATE winhttp)
add_test(NAME metallm COMMAND test_metallm)

# 元数据 P1：providers / match / enrich —— 全离线（providers 只喂写死报文，enrich 只测纯函数）
add_executable(test_metaproviders tests/test_metaproviders.cpp)
target_include_directories(test_metaproviders PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_metaproviders PRIVATE winhttp)   # provider.h → http_client.h
add_test(NAME metaproviders COMMAND test_metaproviders)

add_executable(test_metamatch tests/test_metamatch.cpp)
target_include_directories(test_metamatch PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME metamatch COMMAND test_metamatch)

add_executable(test_enrich tests/test_enrich.cpp)
target_include_directories(test_enrich PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_enrich PRIVATE winhttp)          # enrich.h → provider.h → http_client.h
add_test(NAME enrich COMMAND test_enrich)

# dizzylab 源（同人曲目第一优先，provider.h「dizzylab」节）：离线为主，喂 2026-10-22 真实
# 抓取的页面切片；真网用例只在 RTA_NET_TESTS=1 时跑（默认断网也必须全绿）
add_executable(test_dizzylab tests/test_dizzylab.cpp)
target_include_directories(test_dizzylab PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_dizzylab PRIVATE winhttp)        # provider.h → http_client.h
add_test(NAME dizzylab COMMAND test_dizzylab)

# P2 写回 PCM 逐位门禁(writeback.h):需要环境里有 ffmpeg/ffprobe —— 工具缺失即测试
# 失败,不跳过(门禁就是要在有 ffmpeg 的环境跑)。素材现场生成于 %TEMP%,不碰 MUSIC。
add_executable(test_writeback tests/test_writeback.cpp)
target_include_directories(test_writeback PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME writeback COMMAND test_writeback)

# 热修复守卫（HF-03 解码/临时WAV/分配加固 + HF-01/02 probe/OpenFail 地基）：全离线，不开设备
add_executable(test_hotfix_guard tests/test_hotfix_guard.cpp)
target_include_directories(test_hotfix_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME hotfix_guard COMMAND test_hotfix_guard)

# WASAPI 共享后端（2026-10-22 VM 事故）：§1 纯离线（int32→float32 边界 / 可写帧数钳制），
# §2 设备 smoke 只写【数字静音】≤1 秒（输出先打印醒目横幅，铁律 1）
add_executable(test_wasapi_shared tests/test_wasapi_shared.cpp)
target_include_directories(test_wasapi_shared PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_wasapi_shared PRIVATE ole32 uuid mmdevapi)
add_test(NAME wasapi_shared COMMAND test_wasapi_shared)

# 渲染主循环 RENDERFAIL 归因回归（VM 事故：write_available 恒 0 静默 break → error=""）
# 用 mock 假死 sink 真跑 src/audio/render_loop.h 的循环 —— rtaweb 与测试共用同一份实现
add_executable(test_renderfail tests/test_renderfail.cpp)
target_include_directories(test_renderfail PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME renderfail COMMAND test_renderfail)

# 输出采样率选择纯逻辑（2026-10-22 事故：第二首 44.1k 按 44100 开流，因 rate=max 探测瞬时为空
# 被当成「跟随源率」）—— 全离线，不开设备（铁律 1）
add_executable(test_ratepick tests/test_ratepick.cpp)
target_include_directories(test_ratepick PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ratepick COMMAND test_ratepick)

# 最小 ZIP（STORE）打包结构（2026-10-22 功能3「导出诊断 = 一个压缩包」）。
# 纯内存、零外部依赖、不调 PowerShell（理由见 src/net/zipstore.h 头注）。
add_executable(test_zipstore tests/test_zipstore.cpp)
target_include_directories(test_zipstore PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME zipstore COMMAND test_zipstore)

# P3① 实体模型（schema/归一化/显式边/落盘）：全离线，不调任何网络 API ——
# 归一化与 id 是纯函数；落盘到 %TEMP%；真实曲库样本只读 D:\RTA-hqplayer\MUSIC
#（缺失时打 [SKIP]，不伪造成通过）。铁律 3：断言「全半角不折叠必然 FAIL」有反向验证。
add_executable(test_entities tests/test_entities.cpp)
target_include_directories(test_entities PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME entities COMMAND test_entities)

# probe_media 的 duration 字段（2026-10-22 跨模块缺陷）：ffprobe 原参数只有 format_tags，
# MediaInfo::duration 恒 0 → enrich 的 fileSec 恒 0 → match 的「时长差 ±3s」强信号从未生效。
# 本测试需要环境里的 ffprobe（工具缺失即失败，不跳过，与 writeback 同类门禁）；
# 已知时长的合成 WAV 落在 %TEMP%，不碰 MUSIC；真实 FLAC 素材缺失时打 [SKIP]。
add_executable(test_media_duration tests/test_media_duration.cpp)
target_include_directories(test_media_duration PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME media_duration COMMAND test_media_duration)

# ffmpeg 解码失败归因 + 瞬时故障重试（反馈 20261022-01 现象 B：日文名 FLAC 间歇性
# "Illegal byte sequence" / 退出码 -42）。判据：-42 是 ffmpeg 在非 ASCII 路径上回退 ANSI
# 代码页后的【掩码】，真实原因必须由 Win32 探针给出（ERROR_SHARING_VIOLATION 等），
# 且瞬时占用释放后应自动重试成功。素材全在 %TEMP%（含日文名），不碰 MUSIC；不开音频设备。
add_executable(test_decode_retry tests/test_decode_retry.cpp)
target_include_directories(test_decode_retry PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME decode_retry COMMAND test_decode_retry)

# 测试素材生成器：调制器 -> DSF/DFF（本机原本一个 DSD 文件都没有）
add_executable(make_dsd tools/make_dsd.cpp)
target_link_libraries(make_dsd PRIVATE rtadsp)

# DSD 域转换：抽取 / DirectSDM / 信息（命令行闭环验证）
add_executable(dsdconv tools/dsdconv.cpp)
target_link_libraries(dsdconv PRIVATE rtadsp)


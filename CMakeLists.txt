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

# Bind update migration to the exact application assets in this build.
set(RLPLAYER_UPDATE_ASSET_ENTRIES "")
foreach(lang ar en es fr ja ru)
  set(asset "${CMAKE_SOURCE_DIR}/data/i18n/${lang}.json")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asset}")
  file(SHA256 "${asset}" digest)
  string(APPEND RLPLAYER_UPDATE_ASSET_ENTRIES "    {\"data/i18n/${lang}.json\", \"${digest}\"},\n")
endforeach()


configure_file(src/net/update_assets_hashes.h.in generated/net/update_assets_hashes.h @ONLY)


if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  # /utf-8：本仓源码是 UTF-8 无 BOM，MSVC 默认按系统代码页（本机 936/GBK）解释，
  # 于是中文字符串常量报 "C2001 常量中有换行符"、中文注释报 C4819。clang 下默认就是
  # UTF-8，所以 /utf-8 是让两个编译器行为对齐，不是给 MSVC 开特例。
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
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
  # 2026-10-25（移植自协作者 macOS 分支 01f8787 的「编译选项」那一层）：
  # FMA 收缩会让 DSP 的定点/浮点轨迹在**指令集不同**时产生可测差异 —— 实测在 -DRTA_ARCH=znver5
  # 的开发构建里，EC-7@DSD512 的带内 SNR 从 121.8 dB 塌到 60.9 dB（239 条 vfmadd 融合）。
  # 分发基线 x86-64-v2 没有 FMA，这个旗标对发布产物是**逐字节 no-op**（已用 SHA256 验证）。
  # 需要极限性能时可显式 -DRTA_FP_CONTRACT=fast 关掉本守卫。
  set(RTA_FP_CONTRACT "off" CACHE STRING "FP contraction: off (deterministic) | fast")
  if (RTA_FP_CONTRACT STREQUAL "off")
    add_compile_options(-ffp-contract=off)
  endif()
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

# ---------------- WASAPI 采集 CLI ----------------
add_executable(rtacapture tools/rtacapture.cpp)
target_link_libraries(rtacapture PRIVATE rtadsp ole32 uuid mmdevapi ws2_32)

# ---------------- UPnP 设备发现（只读探测） ----------------
add_executable(upnp_scan tools/upnp_scan.cpp)
target_link_libraries(upnp_scan PRIVATE ws2_32)

# ---------------- UPnP/DLNA 投放 CLI（ROADMAP P5a） ----------------
add_executable(rtacast tools/rtacast.cpp)
target_include_directories(rtacast PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rtacast PRIVATE rtadsp winhttp ws2_32)

# ---------------- GPU 升频核验证 ----------------
add_executable(gpu_resample_test tools/gpu_resample_test.cpp)
target_include_directories(gpu_resample_test PRIVATE ${CMAKE_SOURCE_DIR}/src)   # dsp/gpu_pref.h：候选 DLL 顺序与管线同源
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

# ---------------- 一键反馈自检（诊断打包 + 自有服务器上传）----------------
add_executable(feedback_probe tools/feedback_probe.cpp)
target_include_directories(feedback_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(feedback_probe PRIVATE winhttp psapi user32 shell32 bcrypt)

# ---------------- 在线更新自检 / 演练 ----------------
add_executable(update_probe tools/update_probe.cpp)
target_include_directories(update_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(update_probe PRIVATE winhttp ws2_32 bcrypt)
add_test(NAME update_selftest COMMAND update_probe --selftest)
add_executable(test_update_assets tests/test_update_assets.cpp)
target_include_directories(test_update_assets PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_update_assets PRIVATE winhttp ws2_32 bcrypt)
target_compile_definitions(test_update_assets PRIVATE UPDATE_ASSET_SOURCE="${CMAKE_SOURCE_DIR}")
add_test(NAME update_assets COMMAND test_update_assets)
add_executable(test_update_unicode tests/test_update_unicode.cpp)
target_include_directories(test_update_unicode PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_update_unicode PRIVATE winhttp ws2_32 bcrypt)
add_test(NAME update_unicode COMMAND test_update_unicode)
add_executable(test_update_signature tests/test_update_signature.cpp)
target_include_directories(test_update_signature PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_update_signature PRIVATE winhttp ws2_32 bcrypt)
target_compile_definitions(test_update_signature PRIVATE RLPLAYER_UPDATE_SIGNATURE_TEST_MAIN)
add_test(NAME update_signature COMMAND test_update_signature)
add_executable(test_update_protocol tests/test_update_protocol.cpp)
target_include_directories(test_update_protocol PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_update_protocol PRIVATE winhttp ws2_32 bcrypt)
target_compile_definitions(test_update_protocol PRIVATE RLPLAYER_UPDATE_PROTOCOL_TEST_MAIN)
add_test(NAME update_protocol COMMAND test_update_protocol)

target_include_directories(update_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(test_update_assets PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# ---------------- 桌面启动器（Win32 GUI，双击即用）----------------
# 产品名：RLPlayer
add_executable(rta_launcher WIN32 tools/launcher.cpp)
set_target_properties(rta_launcher PROPERTIES OUTPUT_NAME "RLPlayer")
# ole32/uuid/mmdevapi:启动横幅只读枚举输出端点(wasapi_out.h);dbghelp:崩溃转储(crash_dump.h)
# 更新落地发生在启动器 main() 开头，rtaweb 尚未启动、文件未被占用。
target_link_libraries(rta_launcher PRIVATE ws2_32 shell32 user32 ole32 uuid mmdevapi dbghelp winhttp bcrypt)
target_include_directories(rta_launcher PRIVATE ${CMAKE_SOURCE_DIR}/src "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
  # 2026-10-25：版本号单一来源 —— 从 src/version.h 解析，代入 res/rlplayer.rc.in。
  # 之前 rc 里四处（FILEVERSION / PRODUCTVERSION / FileVersion 字符串 / ProductVersion 字符串）
  # 靠手改，结果 1.3.17~1.5.0 的包属性里一直写着 1.3.16.0。
  # ★ 2026-10-30：version.h 必须是**配置依赖**，否则改版本号不会重新 configure_file，
  #   rlplayer.rc 还是上一次的值，而自定义命令只按 DEPENDS 重跑 rc.exe ⟹ 会编出一个
  #   「包名 2.0.7、exe 资源 2.0.6」的包（实测踩到：make_dist 报 2.0.7，文件属性 2.0.6.0）。
  #   铁律 10.4「版本号单一来源」只有在它真的能传播时才成立。
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/version.h")
  file(STRINGS "${CMAKE_SOURCE_DIR}/src/version.h" RL_VER_LINE REGEX "#define[ ]+RLPLAYER_VERSION")
  string(REGEX MATCH "\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"" RL_VER_MATCH "${RL_VER_LINE}")
  set(RL_VER_MAJOR "${CMAKE_MATCH_1}")
  set(RL_VER_MINOR "${CMAKE_MATCH_2}")
  set(RL_VER_PATCH "${CMAKE_MATCH_3}")
  if(NOT RL_VER_MAJOR)
    set(RL_VER_MAJOR 0)
    set(RL_VER_MINOR 0)
    set(RL_VER_PATCH 0)
  endif()
  set(RL_FILEVER "${RL_VER_MAJOR},${RL_VER_MINOR},${RL_VER_PATCH},0")
  set(RL_STRVER "${RL_VER_MAJOR}.${RL_VER_MINOR}.${RL_VER_PATCH}.0")
  configure_file("${CMAKE_SOURCE_DIR}/res/rlplayer.rc.in" "${CMAKE_BINARY_DIR}/rlplayer.rc" @ONLY)
  message(STATUS "RLPlayer version resource: ${RL_STRVER}")
  set(RL_RC_FILE ${CMAKE_BINARY_DIR}/rlplayer.rc)

  set(RL_ICON_RES ${CMAKE_BINARY_DIR}/rlplayer.res)
  add_custom_command(
    OUTPUT ${RL_ICON_RES}
    COMMAND "${RC_EXE}" /nologo /fo "${RL_ICON_RES}"
            /I "${CMAKE_SOURCE_DIR}/res"
            /I "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um"
            /I "C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared"
            "${RL_RC_FILE}"
    DEPENDS ${RL_RC_FILE} ${CMAKE_SOURCE_DIR}/res/rlplayer.rc.in ${CMAKE_SOURCE_DIR}/src/version.h ${CMAKE_SOURCE_DIR}/res/rlplayer.ico
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
# iphlpapi: /api/naa/discover 通过 tools/naa_discover.h 枚举本机网卡（自动选网段）
target_link_libraries(rtaweb PRIVATE rtadsp ws2_32 winmm avrt dbghelp iphlpapi winhttp bcrypt)

# ---------------- NAA 端点发现（2026-10-24 用户要求彻底删除）----------------
# 为什么删：数播的 NAA 只认 HQPlayer 私有握手（服务端先发 16 字节挑战，客户端要用未公开
# 算法回应；EULA 禁止逆向、也禁止非 HQPlayer 推流）。回显/全零/MD5/只发 XML 等 8 种组合实测
# 全部无回应 ⟹ 这条通路不可能对接成功。用户「NAA 放弃，彻底删除这一功能」。
# 删除内容：naa_scan / naa_probe 两个工具 + tools/naa_discover.h + rtaweb 的 /api/naa/discover
#          + 前端 NAA 后端选项。
# 保留：底层 RTANet（我们自己的协议）——rtaplay --net 与 rtanet_rx 仍被回归测试使用。

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

# ---------------- 滤波器变体对比报告生成器（ROADMAP P2） ----------------
add_executable(filter_report tools/filter_report.cpp)
target_link_libraries(filter_report PRIVATE rtadsp)

# ---------------- RTANet 端点（跑在接 DAC 的机器上） ----------------
add_executable(rtanet_rx tools/rtanet_rx.cpp)
target_link_libraries(rtanet_rx PRIVATE rtadsp ole32 uuid mmdevapi ws2_32)

# ---------------- DSP 核心（头文件库） ----------------
add_library(rtadsp INTERFACE)
target_include_directories(rtadsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------- 测试 ----------------
enable_testing()
# 261024-02 #2：宿主 postMessage 协议 + 外链协议白名单（纯函数，可单测）
add_executable(test_host_msg tests/test_host_msg.cpp)
target_link_libraries(test_host_msg PRIVATE rtadsp)
add_test(NAME host_msg COMMAND test_host_msg)

# 261031：平台层自身的契约测试（text/env/fs/sysinfo/time/http）。
# 判据不是"能编译"，而是"与迁移前的 Win32 调用逐项等价"：显式长度语义、env 的三种返回、
# DeleteFileW/CopyFileW/MoveFileExW 的返回值语义、时区算式、GetTickCount64 同一时基、PUT 通道。
# ws2_32：第 5 组用本机回环服务器验证 http_upload 真的发 PUT（不联网）。
add_executable(test_platform tests/test_platform.cpp)
target_link_libraries(test_platform PRIVATE rtadsp ws2_32)
add_test(NAME platform COMMAND test_platform)

add_executable(test_resampler tests/test_resampler.cpp)
target_link_libraries(test_resampler PRIVATE rtadsp)
add_test(NAME resampler COMMAND test_resampler)

# ---------------- IR 采样率换算（房间校正 IR 与工程采样率不同时必须重采样，否则时间基准错 2.18 倍）----------------
add_executable(test_ir_resample tests/test_ir_resample.cpp)
target_link_libraries(test_ir_resample PRIVATE rtadsp)
add_test(NAME ir_resample COMMAND test_ir_resample)

add_executable(test_dither tests/test_dither.cpp)
target_link_libraries(test_dither PRIVATE rtadsp)
add_test(NAME dither COMMAND test_dither)

add_executable(test_modulator tests/test_modulator.cpp)
target_link_libraries(test_modulator PRIVATE rtadsp)
add_test(NAME modulator COMMAND test_modulator)

add_executable(test_modulator_families tests/test_modulator_families.cpp)
target_link_libraries(test_modulator_families PRIVATE rtadsp)
add_test(NAME modulator_families COMMAND test_modulator_families)

# 7 阶 SDM 全速率稳定（用户 261027-01 #5）：四速率 × 七阶 MSSA/噪声/THD+N + 铁律 7 恢复
add_executable(test_modulator_rates tests/test_modulator_rates.cpp)
target_link_libraries(test_modulator_rates PRIVATE rtadsp)
add_test(NAME modulator_rates COMMAND test_modulator_rates)

add_executable(test_filters tests/test_filters.cpp)
target_link_libraries(test_filters PRIVATE rtadsp)
add_test(NAME filters COMMAND test_filters)

# ---------------- 切趾（Apodizing）滤波器 ----------------
add_executable(test_apodizing tests/test_apodizing.cpp)
target_link_libraries(test_apodizing PRIVATE rtadsp)
add_test(NAME apodizing COMMAND test_apodizing)

add_executable(test_pipeline tests/test_pipeline.cpp)
target_link_libraries(test_pipeline PRIVATE rtadsp)
add_test(NAME pipeline COMMAND test_pipeline)

# 2026-10-29（两轮审查 F17）：DSP 流结束排空 —— 末尾源内容不得丢在卷积/重采样内部缓冲里。
add_executable(test_pipeline_drain tests/test_pipeline_drain.cpp)
target_link_libraries(test_pipeline_drain PRIVATE rtadsp)
add_test(NAME pipeline_drain COMMAND test_pipeline_drain)

add_executable(test_conv_matrix tests/test_conv_matrix.cpp)
target_link_libraries(test_conv_matrix PRIVATE rtadsp)
add_test(NAME conv_matrix COMMAND test_conv_matrix)

add_executable(test_volume_eq tests/test_volume_eq.cpp)
target_link_libraries(test_volume_eq PRIVATE rtadsp)
add_test(NAME volume_eq COMMAND test_volume_eq)

# ---------------- 20 段参数均衡 PEQ（261024-02 #6）：0dB 直通 / 6dB@fc / Q 带宽 /
#                  采样率重建 / 满幅过载恢复 / 热更新交叉淡入（铁律 7）----------------
add_executable(test_peq tests/test_peq.cpp)
target_link_libraries(test_peq PRIVATE rtadsp)
add_test(NAME peq COMMAND test_peq)

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

add_executable(test_ui_page_rates tests/test_ui_page_rates.cpp)
target_include_directories(test_ui_page_rates PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ui_page_rates COMMAND test_ui_page_rates)

add_executable(test_lyrics_echo tests/test_lyrics_echo.cpp)
target_include_directories(test_lyrics_echo PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME lyrics_echo COMMAND test_lyrics_echo)

# 2026-11-02（261102-01 #2）：源站占位词（「纯音乐，请欣赏」）被当成歌词显示
add_executable(test_lyric_placeholder tests/test_lyric_placeholder.cpp)
target_include_directories(test_lyric_placeholder PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME lyric_placeholder COMMAND test_lyric_placeholder)

add_executable(test_dsd1024_offline tests/test_dsd1024_offline.cpp)
target_include_directories(test_dsd1024_offline PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_dsd1024_offline PRIVATE rtadsp)
add_test(NAME dsd1024_offline COMMAND test_dsd1024_offline)

add_executable(test_filter_cache tests/test_filter_cache.cpp)
target_include_directories(test_filter_cache PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_filter_cache PRIVATE rtadsp)
add_test(NAME filter_cache COMMAND test_filter_cache)

add_executable(test_library tests/test_library.cpp)
target_include_directories(test_library PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME library COMMAND test_library)

# 2026-10-25（261025-01 二.3）CUE 整轨分轨：解析器 src/meta/cue.h + 库侧虚拟分轨展开（library.h）。
# 纯离线：不打开音频设备、不需要 ffprobe（探测结果由夹具构造），判据是逐字段断言 + 具体采样数。
# 2026-10-25（第二里程碑）：library 侧接入已恢复（LibTrack 的 cue* 字段 + lib_cue_expand 纯函数 +
# startScan 里的整轨认领/展开），test_cue 重新注册进 ctest。
add_executable(test_cue tests/test_cue.cpp)
target_include_directories(test_cue PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME cue COMMAND test_cue)

add_executable(test_scanpool tests/test_scanpool.cpp)
target_include_directories(test_scanpool PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME scanpool COMMAND test_scanpool)

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

# 2026-10-29（两轮审查 F16）：DSD 流式解码的「损坏必须明确失败」门禁。
# 同一份截断夹具以前整曲读拒绝、流式 open=true 且 read=declared、err 为空（补零）。
# 判据：头部伪造长度 / 播放中截短 / DFF 截断 都必须失败；合法末块 padding 仍须通过。
add_executable(test_dsd_stream_corrupt tests/test_dsd_stream_corrupt.cpp)
target_include_directories(test_dsd_stream_corrupt PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME dsd_stream_corrupt COMMAND test_dsd_stream_corrupt)

# 2026-10-29（两轮审查 F28/F29/F30/F32）：JSON 严格解析 + 代理对 + Knowledge 语言身份
# + HealthStore 全字段往返。全部离线（不联网、不开设备、只写 %TEMP%）。
add_executable(test_review2_json_cache tests/test_review2_json_cache.cpp)
target_include_directories(test_review2_json_cache PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_review2_json_cache PRIVATE winhttp ws2_32)
add_test(NAME review2_json_cache COMMAND test_review2_json_cache)

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

# DeepSeek Search/Fetch 的 function calling 环路（261026-06 #2）：起一个真的本地假服务端，
# 走 winhttp → 本机 socket 的完整往返。ws2_32 是假服务端自身的监听/收发，不是产品依赖。
add_executable(test_llmtools tests/test_llmtools.cpp)
target_include_directories(test_llmtools PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_llmtools PRIVATE winhttp ws2_32)
add_test(NAME llmtools COMMAND test_llmtools)

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

# 网页取图 / URL 安全策略 / 图片闸（261026-04 #7/#9，2026-10-27）：全离线纯函数。
# 覆盖六路抽取（og/twitter/JSON-LD/image_src/itemprop/JS 变量/<img>）、域名白名单删除后的
# 公网判据、1×1 占位图必须被拒、简繁/去括号的名字验收，以及扩展源"只认同名"的纪律。
add_executable(test_artpage tests/test_artpage.cpp)
target_include_directories(test_artpage PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_artpage PRIVATE winhttp)   # artwork.h → http_client.h
add_test(NAME artpage COMMAND test_artpage)

# P2 写回 PCM 逐位门禁(writeback.h):需要环境里有 ffmpeg/ffprobe —— 工具缺失即测试
# 失败,不跳过(门禁就是要在有 ffmpeg 的环境跑)。素材现场生成于 %TEMP%,不碰 MUSIC。
add_executable(test_writeback tests/test_writeback.cpp)
target_include_directories(test_writeback PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME writeback COMMAND test_writeback)

# P1 精简 ffmpeg 的音频格式矩阵门禁（2026-10-22）：
# 用产品自己的 rta::audio::read_any 逐格式断言 rate/channels/frames/电平，样本由 ffmpeg
# 现场合成（−60 dBFS，不打开任何音频设备）。被测二进制是 build/ffmpeg-lite/（自建精简版）；
# 该目录不存在时打印 [SKIP] 不判失败（CI 云机没有 MSYS2，编不出精简版）。
# RTA_SOURCE_DIR 用于读取 tools/ffmpeg_lite_required.txt（与出包守卫同一份白名单）。
add_executable(test_ffmpeg_matrix tests/test_ffmpeg_matrix.cpp)
target_include_directories(test_ffmpeg_matrix PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_ffmpeg_matrix PRIVATE RTA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ffmpeg_matrix COMMAND test_ffmpeg_matrix)

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

# 2026-10-24（N300 欠载方案 C4）：C1 缓冲策略 + C2/C3 遥测/端点消失，全部 mock 驱动，
# 绝不打开音频设备（铁律 1）。被测实现：src/audio/buffer_policy.h、src/audio/render_loop.h。
add_executable(test_buffer_policy tests/test_buffer_policy.cpp)
target_link_libraries(test_buffer_policy PRIVATE rtadsp)
add_test(NAME buffer_policy COMMAND test_buffer_policy)

add_executable(test_render_loop_dry tests/test_render_loop_dry.cpp)
target_link_libraries(test_render_loop_dry PRIVATE rtadsp)
add_test(NAME render_loop_dry COMMAND test_render_loop_dry)

# 音频回放链路边界/数值回归（2026-10-25 深度 DEBUG 路线）：单声道 → 多声道上混、
# NaN/Inf 容器不变式、每声道延迟跨块连续性、WAV 采样率=0、RENDERFAIL 归因时长口径。
# 全部离线（mock sink / 内存 WAV），绝不打开音频设备（铁律 1）。
add_executable(test_playback_edges tests/test_playback_edges.cpp)
target_link_libraries(test_playback_edges PRIVATE rtadsp)
add_test(NAME playback_edges COMMAND test_playback_edges)

# 输出采样率选择纯逻辑（2026-10-22 事故：第二首 44.1k 按 44100 开流，因 rate=max 探测瞬时为空
# 被当成「跟随源率」）—— 全离线，不开设备（铁律 1）
add_executable(test_ratepick tests/test_ratepick.cpp)
target_include_directories(test_ratepick PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME ratepick COMMAND test_ratepick)

# rateAuto（自动换率）策略纯函数单测（元数据文档 1.6.E #10）—— 全离线，不开设备（铁律 1）
add_executable(test_rate_decision tests/test_rate_decision.cpp)
target_include_directories(test_rate_decision PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rate_decision COMMAND test_rate_decision)
add_executable(test_rate_family_policy tests/test_rate_family_policy.cpp)
target_include_directories(test_rate_family_policy PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME rate_family_policy COMMAND test_rate_family_policy)
# 261030-01 规格 v2.1：预设指标与排除规则（家族天花板 / D-15 排除判据）
add_executable(test_preset_metrics tests/test_preset_metrics.cpp)
target_include_directories(test_preset_metrics PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME preset_metrics COMMAND test_preset_metrics)

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

# E-15: 古典 COMPOSER→Work 四层端到端（合成样本 + 真实批量入口）
add_executable(test_classical_e2e tests/test_classical_e2e.cpp)
target_include_directories(test_classical_e2e PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_classical_e2e PRIVATE winhttp)
add_test(NAME classical_e2e COMMAND test_classical_e2e)

# E-14 年份清洗纯函数（统一窗口 [1000, 2100] 与清洗规则）：全离线
add_executable(test_year tests/test_year.cpp)
target_include_directories(test_year PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME year COMMAND test_year)

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

# P3 资料层② 直连层：实体 -> 权威文本材料（维基/萌娘百科 + 缓存/离线回退 + 令牌桶限频/熔断）
add_executable(test_knowledge tests/test_knowledge.cpp)
target_include_directories(test_knowledge PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_knowledge PRIVATE winhttp)
add_test(NAME knowledge COMMAND test_knowledge)

# P3 资料层③ 检索层：DeepSeek search/fetch + rerank/authority/白名单过滤 + 引用纪律
add_executable(test_retrieval tests/test_retrieval.cpp)
target_include_directories(test_retrieval PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_retrieval PRIVATE winhttp)
add_test(NAME retrieval COMMAND test_retrieval)

# 2026-10-24（用户 261024-04 #6/#4 后端侧）：实体资料缓存是否**共用**、第二次是否**零 LLM 调用**、
# 命中是否 ≤300ms。全离线：L2 材料直接写缓存，chat/search/fetch 走计数型 mock transport
# （LlmConfig::transport 钩子），判据是"调用计数不变"而不是"耗时不抖"。
add_executable(test_metacache tests/test_metacache.cpp)
target_include_directories(test_metacache PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_metacache PRIVATE winhttp)
add_test(NAME metacache COMMAND test_metacache)

# 2026-10-24（用户 261024-04 #8）：/api/status 的 gpu（可用性）与 gpuUsed（本次真的参与）
# 组合语义真值表。纯函数（src/meta/statusflags.h），不碰 GPU、不开设备。
add_executable(test_playback_gpu tests/test_playback_gpu.cpp)
target_include_directories(test_playback_gpu PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME playback_gpu COMMAND test_playback_gpu)

# OPRA 耳机 EQ 适配门禁（离线纯函数 + RTA_NET_TESTS=1 真网 opt-in）
add_executable(test_opra_eq tests/test_opra_eq.cpp)
target_include_directories(test_opra_eq PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_opra_eq PRIVATE rtadsp winhttp ws2_32)
add_test(NAME opra_eq COMMAND test_opra_eq)

# P3 资料层④b：Discogs L4 古典作品版本推荐（令牌桶/熔断 + 缓存 + 版本推荐列表）
add_executable(test_discogs tests/test_discogs.cpp)
target_include_directories(test_discogs PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_discogs PRIVATE winhttp)
add_test(NAME discogs COMMAND test_discogs)

# 测试素材生成器：调制器 -> DSF/DFF（本机原本一个 DSD 文件都没有）
add_executable(make_dsd tools/make_dsd.cpp)
target_link_libraries(make_dsd PRIVATE rtadsp)

# DSD 域转换：抽取 / DirectSDM / 信息（命令行闭环验证）
add_executable(dsdconv tools/dsdconv.cpp)
target_link_libraries(dsdconv PRIVATE rtadsp)

# WASAPI 采集核心（ROADMAP P4a）：环形缓冲 / 格式转换 / 离线分析 / --src null 门禁
add_executable(test_capture tests/test_capture.cpp)
target_include_directories(test_capture PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_capture PRIVATE rtadsp ole32 uuid mmdevapi ws2_32)
target_compile_definitions(test_capture PRIVATE RTA_CAPTURE_EXE="$<TARGET_FILE:rtacapture>")
add_test(NAME capture COMMAND test_capture)

# ---------------- UPnP 投放控制面与 HTTP 供源测试（ROADMAP P5a） ----------------
add_executable(test_upnp_control tests/test_upnp_control.cpp)
target_include_directories(test_upnp_control PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_upnp_control PRIVATE rtadsp winhttp ws2_32)
add_test(NAME upnp_control COMMAND test_upnp_control)

# WebView2 双轨版本护栏纯逻辑门禁（P3 双轨运行库护栏）
add_executable(test_wv2_version tests/test_wv2_version.cpp)
target_include_directories(test_wv2_version PRIVATE
    ${CMAKE_SOURCE_DIR}/tools
    ${CMAKE_SOURCE_DIR}/third_party/webview2/include
)
target_link_libraries(test_wv2_version PRIVATE version user32 ole32 advapi32)
add_test(NAME wv2_version COMMAND test_wv2_version)

# ---------------- UPnP 投放后台服务与编排测试（ROADMAP P5b） ----------------
add_executable(test_cast_service tests/test_cast_service.cpp)
target_include_directories(test_cast_service PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_cast_service PRIVATE rtadsp winhttp ws2_32)
add_test(NAME cast_service COMMAND test_cast_service)

# ---------------- 解码免临时 WAV（流式读 ffmpeg 输出，ROADMAP P6b） ----------------
add_executable(test_stream_decode tests/test_stream_decode.cpp)
target_include_directories(test_stream_decode PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_stream_decode PRIVATE rtadsp)
add_test(NAME stream_decode COMMAND test_stream_decode)

# ---------------- DSD 两级抽取测试（ROADMAP P6c） ----------------
add_executable(test_dsd_two_stage tests/test_dsd_two_stage.cpp)
target_include_directories(test_dsd_two_stage PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_dsd_two_stage PRIVATE rtadsp psapi)
add_test(NAME dsd_two_stage COMMAND test_dsd_two_stage)


# ---------------- ASIO 原生 DSD（261024-05）----------------
add_executable(test_asio_native_dsd tests/test_asio_native_dsd.cpp)
target_link_libraries(test_asio_native_dsd PRIVATE rtadsp ole32 uuid advapi32)
add_test(NAME asio_native_dsd COMMAND test_asio_native_dsd)

# ---------------- 监听走 DSP（261024-05）----------------
add_executable(test_monitor_dsp tests/test_monitor_dsp.cpp)
target_link_libraries(test_monitor_dsp PRIVATE rtadsp ole32 uuid advapi32)
add_test(NAME monitor_dsp COMMAND test_monitor_dsp)

# ---------------- 大 DSD 流式解码（261024-06 #2）----------------
add_executable(test_dsd_stream_decode tests/test_dsd_stream_decode.cpp)
target_link_libraries(test_dsd_stream_decode PRIVATE rtadsp)
add_test(NAME dsd_stream_decode COMMAND test_dsd_stream_decode)

# ---------------- 原生标签快速通道（261025-01 一.1/8）----------------
add_executable(test_tags_native tests/test_tags_native.cpp)
target_include_directories(test_tags_native PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME tags_native COMMAND test_tags_native)

# ---------------- 三档官方推荐预设的决策纯函数（2026-10-25）----------------
# 只 include engine/preset_profiles.h：不开设备、不读文件 ⟹ 无声卡/无驱动也能跑。
# 探测侧（WASAPI IsFormatSupported / ASIO canSampleRate、kAsioCanDoIoFormat）在 rtaweb 里，
# 由 GET /api/preset/caps 的实测输出验证 —— 探针与决策分开测，判据才不互相污染。
add_executable(test_preset_profiles tests/test_preset_profiles.cpp)
target_include_directories(test_preset_profiles PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME preset_profiles COMMAND test_preset_profiles)

# ================= 音频域六包（外部评审会话 audio-health，2026-10-26 交接）=================
# A 包：GPU 精度探针（依赖本机 GPU，不注册 add_test；无卡机器会打印"不可用"并 exit 2）
add_executable(gpu_precision tools/gpu_precision.cpp)
target_include_directories(gpu_precision PRIVATE ${CMAKE_SOURCE_DIR}/src)

# B1a：动态测量（TT DR / BS.1770-4 LUFS / LRA）+ 假无损检测
add_executable(test_dynamics tests/test_dynamics.cpp)
target_include_directories(test_dynamics PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME dynamics COMMAND test_dynamics)

add_executable(test_fake_lossless tests/test_fake_lossless.cpp)
target_include_directories(test_fake_lossless PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fake_lossless COMMAND test_fake_lossless)

# B1b：体检编排 + 缓存 + CLI
add_executable(rtahealth tools/rtahealth.cpp)
target_link_libraries(rtahealth PRIVATE rtadsp)
target_include_directories(rtahealth PRIVATE ${CMAKE_SOURCE_DIR}/src)

# B2：音质分析（THD+N / 本底噪声 / SNR / 频响 / 削波 / 立体声 / 动态细化）
add_executable(test_quality tests/test_quality.cpp)
target_include_directories(test_quality PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME quality COMMAND test_quality)

# B2 置信度专项（DEBUG）：19 项的重复性 / 有效动态范围 / 误报边界 / 说谎场景
# 与 test_quality 的分工：那边证明"给定已知答案测得对"，这边量化"什么时候它会错"。
add_executable(test_quality_confidence tests/test_quality_confidence.cpp)
target_include_directories(test_quality_confidence PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME quality_confidence COMMAND test_quality_confidence)

# B2 真实文件探针（不注册 add_test：依赖本机曲库；只读分析，不开音频设备）
add_executable(quality_probe tools/quality_probe.cpp)
target_link_libraries(quality_probe PRIVATE rtadsp)
target_include_directories(quality_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)

# B3：同一首多版本对比（包络互相关对齐 / 可比性标注 / 加权排序 / 确定性解释）
add_executable(test_version_compare tests/test_version_compare.cpp)
target_link_libraries(test_version_compare PRIVATE rtadsp)
target_include_directories(test_version_compare PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME version_compare COMMAND test_version_compare)

# B4：AI 解释层（KEY.txt / 降级 / 脱敏；离线注入 transport，不联网）
add_executable(test_quality_ai tests/test_quality_ai.cpp)
target_link_libraries(test_quality_ai PRIVATE rtadsp)
target_include_directories(test_quality_ai PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME quality_ai COMMAND test_quality_ai)

# B2/B3/B4 真实曲库测量工具（不注册 add_test：依赖本机曲库与网络）
add_executable(measure_quality_lib tests/measure_quality_lib.cpp)
target_link_libraries(measure_quality_lib PRIVATE rtadsp)
target_include_directories(measure_quality_lib PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(test_lyrics_ai tests/test_lyrics_ai.cpp)
target_include_directories(test_lyrics_ai PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_lyrics_ai PRIVATE winhttp)
add_test(NAME lyrics_ai COMMAND test_lyrics_ai)

add_executable(test_track_health tests/test_track_health.cpp)
target_link_libraries(test_track_health PRIVATE rtadsp)
target_include_directories(test_track_health PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME track_health COMMAND test_track_health)

# C1：房间校正算法核心
add_executable(test_room_correction tests/test_room_correction.cpp)
target_link_libraries(test_room_correction PRIVATE rtadsp)
target_include_directories(test_room_correction PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME room_correction COMMAND test_room_correction)

# C2：测量会话编排（传输接口注入）
add_executable(test_room_measurement tests/test_room_measurement.cpp)
target_link_libraries(test_room_measurement PRIVATE rtadsp)
target_include_directories(test_room_measurement PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME room_measurement COMMAND test_room_measurement)

# GPU 卷积流式正确性（261026-07）：CPU 流式必须与整块逐位一致；GPU 部分只诊断。
# 无卡/无 rtagpu.dll 的机器 GPU 段自动 SKIP，CPU 判据照跑。
add_executable(test_gpu_stream tests/test_gpu_stream.cpp)
target_link_libraries(test_gpu_stream PRIVATE rtadsp)
target_include_directories(test_gpu_stream PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME gpu_stream COMMAND test_gpu_stream)

# ---------------- DSD→DSD 速率转换（2026-10-27，DSD 调制专线并入主线）----------------
# 判据：THD+N / 通带平坦度 / 折叠残留 / 级联 / 满幅与单样本越界恢复 / 真实 DSF 端到端。
# 全程离线：不开任何音频设备（铁律 1）；真实素材缺失时该节自动 SKIP。
add_executable(test_dsd_rate_convert tests/test_dsd_rate_convert.cpp)
target_link_libraries(test_dsd_rate_convert PRIVATE rtadsp)
add_test(NAME dsd_rate_convert COMMAND test_dsd_rate_convert)

# ================= ReplayGain / Album Gain（261027-01 #2，用户明确接受破坏 bit-perfect）=================
# 后端：src/dsp/replaygain.h（标签读取 + RG2.0 换算 + 三模式策略 + 防削波）。
# 独立验证程序：rg_probe（只读标签/音频，ffmpeg 解码走内存管道，不落盘、不开音频设备）。
add_executable(rg_probe tools/rg_probe.cpp)
target_link_libraries(rg_probe PRIVATE rtadsp)
target_include_directories(rg_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)

# 门禁：容器键名实读（ffmpeg 现场写夹具 + ffprobe 交叉验证）/ 防削波 / 三模式差异 /
# 与 loudness 串联不双倍提升 / 专辑归组 / 真实库只读统计。
# 环境里没有 ffmpeg 时 [2] 节打印 [SKIP] 不判失败（与 writeback/ffmpeg_matrix 同类处理），
# 其余各节（[1][3]~[7]）纯合成信号，不依赖任何外部工具。
add_executable(test_replaygain tests/test_replaygain.cpp)
target_link_libraries(test_replaygain PRIVATE rtadsp)
target_include_directories(test_replaygain PRIVATE ${CMAKE_SOURCE_DIR}/src)
# [10] 节要用 rg_probe 做文件级端到端闭环（标签 → 解码 → 判定），路径编译期注入
target_compile_definitions(test_replaygain PRIVATE RTA_RG_PROBE="$<TARGET_FILE:rg_probe>")
add_dependencies(test_replaygain rg_probe)
add_test(NAME replaygain COMMAND test_replaygain)

# ==================== 2026-10-27 DEBUG 新增测试（主控统一登记） ====================
# 来源：5 条 DEBUG 线（19 项置信度 / 假无损标定 / 版本对比稳定性 / 端到端 fuzz / 真窗口 UI）。
# 每个测试都要能"失败"（铁律 3），不是只断言不崩。

# 【health 契约】BUG-2：四种错误响应逐字段同构（65 条键路径）；
# BUG-3：半截文件必须报 incomplete（容器声明时长 vs 实际解码时长）。
# 缺省 SKIP（不读本机曲库）；带一个真 FLAC 路径参数可跑端到端。
add_executable(test_health_completeness tests/test_health_completeness.cpp)
target_include_directories(test_health_completeness PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME health_completeness COMMAND test_health_completeness)

# 【health 健壮性】48 条断言：非法 UTF-8 / 缺失参数 / 目录 / 0 字节 / 假扩展名 / 超长路径。
# 需要 psapi（内存与句柄统计）。
add_executable(test_health_robust tests/test_health_robust.cpp)
target_include_directories(test_health_robust PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_health_robust PRIVATE psapi)
add_test(NAME health_robust COMMAND test_health_robust)

# 【版本对比稳定性】§1 确定性 / §2 容器互换 Δ=0 / §3 对齐 / §4 不同录音拒绝 / §5 可比性 /
# §6 分差<1 提示 / §7 第 8 维带宽。
add_executable(test_version_compare_stability tests/test_version_compare_stability.cpp)
target_include_directories(test_version_compare_stability PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_version_compare_stability PRIVATE rtadsp)
add_test(NAME version_compare_stability COMMAND test_version_compare_stability)

# ---- 探针：故意不注册 add_test（要读本机曲库或标定素材，进 ctest 必然红） ----
add_executable(fl_probe tools/fl_probe.cpp)
target_include_directories(fl_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(vc_probe tools/vc_probe.cpp)
target_include_directories(vc_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_executable(health_fuzz tools/health_fuzz.cpp)
target_include_directories(health_fuzz PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(health_fuzz PRIVATE psapi winhttp)

# tools/wv2_shot.cpp 故意不登记：它是一次性取证用的最小 WebView2 宿主，
# 与产品无关，进构建只会拖慢全量。

# ---------------- 离线滤波器全量测量工具（261030-01） ----------------
add_executable(filter_sweep tools/filter_sweep.cpp)
target_link_libraries(filter_sweep PRIVATE rtadsp)
# Strict production quality: original F1, DD dense scan and peak refinement.
# No failure allowance; historical known failures remain in the identity fixture.
add_test(NAME filter_sweep COMMAND filter_sweep --output ${CMAKE_CURRENT_BINARY_DIR}/filter_sweep_audit.tsv)

add_executable(apod_check tools/apod_check.cpp)
target_include_directories(apod_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME apod_design COMMAND apod_check)
# ---------------- int64/int128 精确卷积验收（261030-01 D-11） ----------------
add_executable(test_fixed_conv tests/test_fixed_conv.cpp)
target_include_directories(test_fixed_conv PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME fixed_conv COMMAND test_fixed_conv)
# 261030-01：1-bit 调制器前级余量目标的安全性验收
add_executable(test_sdm_headroom tests/test_sdm_headroom.cpp)
target_include_directories(test_sdm_headroom PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME sdm_headroom COMMAND test_sdm_headroom)

# 261030-01：SDM 稳定性/裕度矩阵（诊断工具，不注册 add_test —— 29 秒，判据已由 sdm_headroom 覆盖）
add_executable(sdm_matrix tools/sdm_matrix.cpp)
target_include_directories(sdm_matrix PRIVATE ${CMAKE_SOURCE_DIR}/src)

# 2026-10-31 集中审核：真实生产路径的离线回归，不打开音频设备。
add_executable(test_audit_dsd tests/test_audit_dsd.cpp)
target_link_libraries(test_audit_dsd PRIVATE rtadsp)
add_test(NAME audit_dsd COMMAND test_audit_dsd)
add_executable(test_audit_app tests/test_audit_app.cpp)
target_include_directories(test_audit_app PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_audit_app PRIVATE winhttp ws2_32)
add_test(NAME audit_app COMMAND test_audit_app)
add_executable(test_audit_pcm tests/test_audit_pcm.cpp)
target_link_libraries(test_audit_pcm PRIVATE rtadsp)
add_test(NAME audit_pcm COMMAND test_audit_pcm)

# Independent DSP fixes and negative-control regressions.
foreach(name dsp_phase dsp_linear_quality dsp_apod_quality)
  add_executable(test_${name} tests/test_${name}.cpp)
  target_include_directories(test_${name} PRIVATE src tools)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

add_executable(review2_spectrum tools/review2_spectrum.cpp)
target_link_libraries(review2_spectrum PRIVATE rtadsp)
foreach(name review2_spectrum review2_apod sdm_block)
  add_executable(test_${name} tests/test_${name}.cpp)
  target_include_directories(test_${name} PRIVATE src tools)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()
add_executable(test_review2_apod_mutant tests/test_review2_apod.cpp)
target_include_directories(test_review2_apod_mutant PRIVATE src)
target_compile_definitions(test_review2_apod_mutant PRIVATE REVIEW2_APOD_MUTANT_WEAK_BOUNDARY)
add_test(NAME review2_apod_mutant COMMAND test_review2_apod_mutant)
set_tests_properties(review2_apod_mutant PROPERTIES WILL_FAIL TRUE)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME dsp_measure_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_dsp_measure_cli.py --exe $<TARGET_FILE:filter_sweep>)
add_test(NAME filter_report_cli COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/test_filter_report_cli.py --exe $<TARGET_FILE:filter_report>
    --sweep $<TARGET_FILE:filter_sweep>)

add_executable(test_dsp_routing tests/test_dsp_routing.cpp)
target_link_libraries(test_dsp_routing PRIVATE rtadsp)
add_test(NAME dsp_routing COMMAND test_dsp_routing)
add_executable(dsp_pipeline_benchmark tools/dsp_pipeline_benchmark.cpp)
target_link_libraries(dsp_pipeline_benchmark PRIVATE rtadsp psapi)

add_executable(test_dsp_phase_even tests/test_dsp_phase_even.cpp)
target_include_directories(test_dsp_phase_even PRIVATE src tools)
add_test(NAME dsp_phase_even COMMAND test_dsp_phase_even)
add_executable(dsp_measure_dsd tools/dsp_measure_dsd.cpp)
target_include_directories(dsp_measure_dsd PRIVATE src tools)
add_test(NAME dsd_streamed_fft COMMAND dsp_measure_dsd --selftest)

# Reference envelopes are regenerated from current production coefficients at build time.
add_executable(dsp_ringing_reference tools/dsp_ringing_reference.cpp)
target_include_directories(dsp_ringing_reference PRIVATE src tools)
target_link_libraries(dsp_ringing_reference PRIVATE bcrypt)
set(RING_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/dsp_ringing_reference.h)
file(GLOB RING_DSP_DEPENDENCIES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/dsp/*.h)
add_custom_command(OUTPUT ${RING_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND dsp_ringing_reference --production --header ${RING_HEADER}
          --json ${CMAKE_CURRENT_BINARY_DIR}/generated/dsp_ringing_reference.json
  DEPENDS dsp_ringing_reference ${RING_DSP_DEPENDENCIES}
  VERBATIM)
add_custom_target(generate_ringing_reference DEPENDS ${RING_HEADER})
add_dependencies(rtaweb generate_ringing_reference)
target_include_directories(rtaweb PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

add_test(NAME dsp_ringing_definition COMMAND dsp_ringing_reference --selftest)

# 滤波器详情静态数据：生产系数在构建期测量，UI 只负责画。
add_executable(filter_detail_reference tools/filter_detail_reference.cpp)
target_include_directories(filter_detail_reference PRIVATE src tools)
set(FILTER_DETAIL_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/filter_detail_reference.h)
file(GLOB FILTER_DETAIL_TOOL_DEPS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/*.h)
add_custom_command(OUTPUT ${FILTER_DETAIL_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND filter_detail_reference --header ${FILTER_DETAIL_HEADER}
  DEPENDS filter_detail_reference ${RING_DSP_DEPENDENCIES} ${FILTER_DETAIL_TOOL_DEPS}
  VERBATIM)
add_custom_target(generate_filter_detail DEPENDS ${FILTER_DETAIL_HEADER})
add_dependencies(rtaweb generate_filter_detail)
add_executable(test_filter_detail_reference tests/test_filter_detail_reference.cpp)
target_include_directories(test_filter_detail_reference PRIVATE src tools ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_dependencies(test_filter_detail_reference generate_filter_detail)
add_test(NAME filter_detail_reference COMMAND test_filter_detail_reference)
add_executable(test_asio_buffer_policy tests/test_asio_buffer_policy.cpp)
target_include_directories(test_asio_buffer_policy PRIVATE src tools)
add_test(NAME asio_buffer_policy COMMAND test_asio_buffer_policy)
add_executable(test_native_dsd_pack tests/test_native_dsd_pack.cpp)
target_include_directories(test_native_dsd_pack PRIVATE src tools)
add_test(NAME native_dsd_pack COMMAND test_native_dsd_pack)

add_executable(test_cda_path tests/test_cda_path.cpp)
target_include_directories(test_cda_path PRIVATE src tools)
target_link_libraries(test_cda_path PRIVATE winmm)
add_test(NAME cda_path COMMAND test_cda_path)

add_executable(test_dsp_measure_stages tests/test_dsp_measure_stages.cpp)
target_include_directories(test_dsp_measure_stages PRIVATE src tools)
add_test(NAME dsp_measure_stages COMMAND test_dsp_measure_stages)

add_executable(dsp_dop_benchmark tools/dsp_dop_benchmark.cpp)
target_link_libraries(dsp_dop_benchmark PRIVATE rtadsp bcrypt)
add_executable(dsp_sdm_block_benchmark tools/dsp_sdm_block_benchmark.cpp)
target_link_libraries(dsp_sdm_block_benchmark PRIVATE rtadsp)
# GPU observation fixture integration: append to source/CMakeLists.txt.
# All test DLLs live in private executable directories. Never copy them to
# a release directory or the root build directory containing production DLLs.
add_executable(test_dsp_gpu_observation tests/test_dsp_gpu_observation.cpp)
target_link_libraries(test_dsp_gpu_observation PRIVATE rtadsp)
set_target_properties(test_dsp_gpu_observation PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gpu-observation/common")

function(add_gpu_observation_fixture fixture_name fixture_mode)
    set(fixture_target "gpu_observation_fixture_${fixture_name}")
    set(fixture_dir "${CMAKE_BINARY_DIR}/gpu-observation/${fixture_name}")
    add_library(${fixture_target} SHARED tests/test_dsp_gpu_observation_fixture.cpp)
    target_compile_definitions(${fixture_target} PRIVATE FIXTURE_MODE=${fixture_mode})
    set_target_properties(${fixture_target} PROPERTIES
        OUTPUT_NAME rtagpu PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${fixture_dir}"
        LIBRARY_OUTPUT_DIRECTORY "${fixture_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${fixture_dir}")
    # A custom target runs every build so the copied executable cannot lag
    # behind a header-only production change when the fixture DLL did not relink.
    add_custom_target(gpu_observation_prepare_${fixture_name} ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:test_dsp_gpu_observation>"
            "$<TARGET_FILE_DIR:${fixture_target}>/test_dsp_gpu_observation.exe"
        DEPENDS test_dsp_gpu_observation ${fixture_target}
        VERBATIM)
    add_test(NAME dsp_gpu_observation_${fixture_name}
        COMMAND "$<TARGET_FILE_DIR:${fixture_target}>/test_dsp_gpu_observation.exe"
                "${fixture_name}")
    set_tests_properties(dsp_gpu_observation_${fixture_name} PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${fixture_target}>")
endfunction()

add_gpu_observation_fixture(fail 0)
add_gpu_observation_fixture(first 1)
add_gpu_observation_fixture(second 2)
add_gpu_observation_fixture(success 3)

# Hardware verification is explicit; the four injected cases do not need GPU
# hardware and remain normal gates. This workstation already passed 'real'.
option(RTA_GPU_OBSERVATION_REAL_TEST "Run real GPU dispatch observation test" OFF)
set(RTA_GPU_OBSERVATION_REAL_DLL "${CMAKE_SOURCE_DIR}/build/rtagpu.dll"
    CACHE FILEPATH "Production HIP backend for isolated observation test")
set(RTA_GPU_OBSERVATION_HIP_RUNTIME "${CMAKE_SOURCE_DIR}/build/amdhip64_7.dll"
    CACHE FILEPATH "Bundled HIP runtime for isolated observation test")
if(RTA_GPU_OBSERVATION_REAL_TEST)
    if(NOT EXISTS "${RTA_GPU_OBSERVATION_REAL_DLL}" OR
       NOT EXISTS "${RTA_GPU_OBSERVATION_HIP_RUNTIME}")
        message(FATAL_ERROR "Real GPU observation requested but backend/runtime is missing")
    endif()
    set(real_dir "${CMAKE_BINARY_DIR}/gpu-observation/real")
    add_custom_target(gpu_observation_prepare_real ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${real_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:test_dsp_gpu_observation>"
            "${real_dir}/test_dsp_gpu_observation.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTA_GPU_OBSERVATION_REAL_DLL}" "${real_dir}/rtagpu.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTA_GPU_OBSERVATION_HIP_RUNTIME}" "${real_dir}/amdhip64_7.dll"
        DEPENDS test_dsp_gpu_observation
        VERBATIM)
    add_test(NAME dsp_gpu_observation_real
        COMMAND "${real_dir}/test_dsp_gpu_observation.exe" real)
    set_tests_properties(dsp_gpu_observation_real PROPERTIES WORKING_DIRECTORY "${real_dir}")
endif()


add_executable(test_dsp_fixed_quality tests/test_dsp_fixed_quality.cpp)
target_include_directories(test_dsp_fixed_quality PRIVATE src tools)
target_link_libraries(test_dsp_fixed_quality PRIVATE rtadsp)
add_test(NAME dsp_fixed_quality COMMAND test_dsp_fixed_quality)

# Append to the project's test section. Production source has no fault switches.
add_executable(test_dsp_gpu_compact tests/test_dsp_gpu_compact.cpp)
target_link_libraries(test_dsp_gpu_compact PRIVATE rtadsp)
set_target_properties(test_dsp_gpu_compact PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/gpu-compact/common")

function(add_gpu_compact_fixture fixture_name export_compact)
    set(fixture_target "gpu_compact_fixture_${fixture_name}")
    set(fixture_dir "${CMAKE_BINARY_DIR}/gpu-compact/${fixture_name}")
    add_library(${fixture_target} SHARED tests/test_dsp_gpu_compact_fixture.cpp)
    target_link_libraries(${fixture_target} PRIVATE rtadsp)
    target_compile_definitions(${fixture_target} PRIVATE COMPACT_EXPORT=${export_compact})
    set_target_properties(${fixture_target} PROPERTIES
        OUTPUT_NAME rtagpu PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${fixture_dir}"
        LIBRARY_OUTPUT_DIRECTORY "${fixture_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${fixture_dir}")
    add_custom_target(gpu_compact_prepare_${fixture_name} ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:test_dsp_gpu_compact>"
            "$<TARGET_FILE_DIR:${fixture_target}>/test_dsp_gpu_compact.exe"
        DEPENDS test_dsp_gpu_compact ${fixture_target}
        VERBATIM)
    add_test(NAME dsp_gpu_compact_${fixture_name}
        COMMAND "$<TARGET_FILE_DIR:${fixture_target}>/test_dsp_gpu_compact.exe" "${fixture_name}")
    set_tests_properties(dsp_gpu_compact_${fixture_name} PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${fixture_target}>")
endfunction()
add_gpu_compact_fixture(legacy 0)
add_gpu_compact_fixture(fail 1)


# Private real-HIP failure injection is built from current source, never packaged.
option(RTA_GPU_COMPACT_FAULT_TEST "Enable developer-only real HIP fault fixture" OFF)
set(RTA_GPU_COMPACT_HIP_RUNTIME "${CMAKE_SOURCE_DIR}/build/amdhip64_7.dll"
    CACHE FILEPATH "HIP runtime for the private developer test")
if(RTA_GPU_COMPACT_FAULT_TEST)
    find_program(RTA_HIPCC NAMES hipcc.bat hipcc
        HINTS "$ENV{HIP_PATH}/bin" "C:/Program Files/AMD/ROCm/7.2/bin" REQUIRED)
    if(NOT EXISTS "${RTA_GPU_COMPACT_HIP_RUNTIME}")
        message(FATAL_ERROR "Real HIP fault test requested but runtime is missing")
    endif()
    set(fault_dir "${CMAKE_BINARY_DIR}/gpu-compact/real")
    set(fault_dll "${fault_dir}/rtagpu.dll")
    add_custom_command(OUTPUT "${fault_dll}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${fault_dir}"
        COMMAND "${RTA_HIPCC}" --offload-arch=gfx1201 -O3 -march=x86-64-v2
            -ffp-contract=off -shared -I "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/tests/test_dsp_gpu_compact_fault_fixture.cpp" -o "${fault_dll}"
        DEPENDS tests/test_dsp_gpu_compact_fault_fixture.cpp src/gpu/gpu_resample_hip.cpp
        VERBATIM)
    add_custom_target(gpu_compact_prepare_real ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:test_dsp_gpu_compact>" "${fault_dir}/test_dsp_gpu_compact.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTA_GPU_COMPACT_HIP_RUNTIME}" "${fault_dir}/amdhip64_7.dll"
        DEPENDS test_dsp_gpu_compact "${fault_dll}"
        VERBATIM)
    add_test(NAME dsp_gpu_compact_fault_real COMMAND "${fault_dir}/test_dsp_gpu_compact.exe" real)
    set_tests_properties(dsp_gpu_compact_fault_real PROPERTIES WORKING_DIRECTORY "${fault_dir}")
endif()

add_executable(test_dop_render_seek tests/test_dop_render_seek.cpp)
target_include_directories(test_dop_render_seek PRIVATE src)
add_test(NAME dop_render_seek COMMAND test_dop_render_seek)

# Exact-output performance regression gates (offline; no device API).
add_executable(test_resampler_simd tests/test_resampler_simd.cpp)
target_link_libraries(test_resampler_simd PRIVATE rtadsp)
add_test(NAME resampler_simd COMMAND test_resampler_simd)
add_executable(test_resampler_rational_simd tests/test_resampler_rational_simd.cpp)
target_link_libraries(test_resampler_rational_simd PRIVATE rtadsp)
add_test(NAME resampler_rational_simd COMMAND test_resampler_rational_simd)
add_executable(test_resampler_rational_scalar tests/test_resampler_rational_simd.cpp)
target_compile_definitions(test_resampler_rational_scalar PRIVATE RTA_RESAMPLER_DISABLE_AVX2=1)
target_link_libraries(test_resampler_rational_scalar PRIVATE rtadsp)
add_test(NAME resampler_rational_scalar COMMAND test_resampler_rational_scalar)
add_executable(test_resampler_simd_early tests/test_resampler_simd.cpp)
target_compile_definitions(test_resampler_simd_early PRIVATE RTA_TEST_EARLY_INTRINSICS=1)
target_link_libraries(test_resampler_simd_early PRIVATE rtadsp)
add_test(NAME resampler_simd_early COMMAND test_resampler_simd_early)
add_executable(test_resampler_rational_early tests/test_resampler_rational_simd.cpp)
target_compile_definitions(test_resampler_rational_early PRIVATE RTA_TEST_EARLY_INTRINSICS=1)
target_link_libraries(test_resampler_rational_early PRIVATE rtadsp)
add_test(NAME resampler_rational_early COMMAND test_resampler_rational_early)
add_executable(test_fixed_resampler_simd tests/test_fixed_resampler_simd.cpp)
target_link_libraries(test_fixed_resampler_simd PRIVATE rtadsp)
add_test(NAME fixed_resampler_simd COMMAND test_fixed_resampler_simd)
add_executable(test_resampler_fixed_simd tests/test_resampler_fixed_simd.cpp)
target_link_libraries(test_resampler_fixed_simd PRIVATE rtadsp)
add_test(NAME resampler_fixed_simd COMMAND test_resampler_fixed_simd)
add_executable(test_resampler_fixed_scalar tests/test_resampler_fixed_simd.cpp)
target_compile_definitions(test_resampler_fixed_scalar PRIVATE RTA_RESAMPLER_DISABLE_AVX2=1)
target_link_libraries(test_resampler_fixed_scalar PRIVATE rtadsp)
add_test(NAME resampler_fixed_scalar COMMAND test_resampler_fixed_scalar)
add_executable(test_fft_precision tests/test_fft_precision.cpp)
target_link_libraries(test_fft_precision PRIVATE rtadsp)
add_test(NAME fft_precision COMMAND test_fft_precision)
add_executable(filter_numerical_audit tools/filter_numerical_audit.cpp)
target_link_libraries(filter_numerical_audit PRIVATE rtadsp)
add_executable(test_parallel_pool tests/test_parallel_pool.cpp)
target_link_libraries(test_parallel_pool PRIVATE rtadsp)
add_test(NAME parallel_pool COMMAND test_parallel_pool)
add_executable(test_sdm_fast tests/test_sdm_fast.cpp)
target_link_libraries(test_sdm_fast PRIVATE rtadsp)
add_test(NAME sdm_fast COMMAND test_sdm_fast)
add_executable(test_sdm_round2 tests/test_sdm_round2.cpp)
target_link_libraries(test_sdm_round2 PRIVATE rtadsp)
add_test(NAME sdm_stereo_state COMMAND test_sdm_round2)
add_executable(test_sdm_sections tests/test_sdm_sections.cpp)
target_link_libraries(test_sdm_sections PRIVATE rtadsp)
add_test(NAME sdm_sections COMMAND test_sdm_sections)
add_executable(test_sdm_multilevel tests/test_sdm_multilevel.cpp)
target_link_libraries(test_sdm_multilevel PRIVATE rtadsp)
add_test(NAME sdm_multilevel COMMAND test_sdm_multilevel)
add_test(NAME sdm_multilevel_441_1024 COMMAND test_sdm_multilevel --rate 45158400)
add_test(NAME sdm_multilevel_48_1024 COMMAND test_sdm_multilevel --rate 49152000)
add_executable(test_sdm_highrate_quality tests/test_sdm_highrate_quality.cpp)
target_link_libraries(test_sdm_highrate_quality PRIVATE rtadsp)
add_test(NAME sdm_highrate_quality COMMAND test_sdm_highrate_quality --full)
add_test(NAME sdm_highrate_ahm_quality COMMAND test_sdm_highrate_quality --full --ahm)
add_executable(test_dop_stereo tests/test_dop_stereo.cpp)
target_link_libraries(test_dop_stereo PRIVATE rtadsp)
add_test(NAME dop_stereo COMMAND test_dop_stereo)
add_executable(test_dop_stereo_scalar tests/test_dop_stereo.cpp)
target_compile_definitions(test_dop_stereo_scalar PRIVATE RTA_DISABLE_SDM_STEREO_SIMD=1)
target_link_libraries(test_dop_stereo_scalar PRIVATE rtadsp)
add_test(NAME dop_stereo_scalar COMMAND test_dop_stereo_scalar)
add_executable(dsp_efficiency_benchmark tools/dsp_efficiency_benchmark.cpp)
target_link_libraries(dsp_efficiency_benchmark PRIVATE rtadsp psapi bcrypt)
add_test(NAME dsp_efficiency_selftest COMMAND dsp_efficiency_benchmark --selftest)

add_executable(test_dop_async tests/test_dop_async.cpp)
target_link_libraries(test_dop_async PRIVATE rtadsp)
add_test(NAME dop_async COMMAND test_dop_async)

add_executable(test_dop_async_integration tests/test_dop_async_integration.cpp)
target_link_libraries(test_dop_async_integration PRIVATE rtadsp)
add_test(NAME dop_async_integration COMMAND test_dop_async_integration)

add_executable(test_pcm_pack tests/test_pcm_pack.cpp)
target_link_libraries(test_pcm_pack PRIVATE rtadsp)
add_test(NAME pcm_pack COMMAND test_pcm_pack)
add_executable(test_dsd_native_fast tests/test_dsd_native_fast.cpp)
target_link_libraries(test_dsd_native_fast PRIVATE rtadsp)
add_test(NAME dsd_native_fast COMMAND test_dsd_native_fast)

# Optional developer-only real HIP cache regression; never shipped in the app.
option(RTA_GPU_MULTIPHASE_CACHE_TEST "Enable developer HIP cache regression" OFF)
if(RTA_GPU_MULTIPHASE_CACHE_TEST)
  find_program(RTA_CACHE_HIPCC NAMES hipcc.bat hipcc
    HINTS "$ENV{HIP_PATH}/bin" "C:/Program Files/AMD/ROCm/7.2/bin" REQUIRED)
  set(cache_test "${CMAKE_BINARY_DIR}/test_gpu_multiphase_cache.exe")
  add_custom_command(OUTPUT "${cache_test}"
    COMMAND "${RTA_CACHE_HIPCC}" --offload-arch=gfx1201 -O3
      -march=x86-64-v2 -ffp-contract=off -std=c++17
      "${CMAKE_SOURCE_DIR}/tests/test_gpu_multiphase_cache.cpp" -o "${cache_test}"
    DEPENDS tests/test_gpu_multiphase_cache.cpp src/gpu/gpu_resample_hip.cpp
    VERBATIM)
  add_custom_target(gpu_multiphase_cache_prepare ALL DEPENDS "${cache_test}")
  add_test(NAME gpu_multiphase_cache_real COMMAND "${cache_test}")
endif()

# Round 4 independent measurement gates and source/toolchain-bound filter cache.
file(GLOB_RECURSE RTA_FILTER_CACHE_INPUTS CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/src/dsp/*.h" "${CMAKE_SOURCE_DIR}/src/dsp/*.hpp"
     "${CMAKE_SOURCE_DIR}/src/dsp/*.inl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${RTA_FILTER_CACHE_INPUTS} "${CMAKE_SOURCE_DIR}/tools/review4_cache_fingerprint.py"
    "${CMAKE_CXX_COMPILER}")
get_directory_property(RTA_DIRECTORY_COMPILE_OPTIONS COMPILE_OPTIONS)
string(REPLACE ";" " " RTA_DIRECTORY_COMPILE_OPTIONS "${RTA_DIRECTORY_COMPILE_OPTIONS}")
string(TOUPPER "${CMAKE_BUILD_TYPE}" RTA_BUILD_CONFIG)
set(RTA_FILTER_FINGERPRINT_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${RTA_BUILD_CONFIG}} ${RTA_DIRECTORY_COMPILE_OPTIONS} config=${CMAKE_BUILD_TYPE} arch=${RTA_ARCH} fp=${RTA_FP_CONTRACT} crt=${CMAKE_MSVC_RUNTIME_LIBRARY} system=${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} compiler=${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}")
set(RTA_FILTER_RUNTIME_BINDING_DEFAULT OFF)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
    set(RTA_FILTER_RUNTIME_BINDING_DEFAULT ON)
endif()
option(RTA_FILTER_CACHE_BIND_RUNTIME "Bind Windows Clang static runtime archives to the cache key" ${RTA_FILTER_RUNTIME_BINDING_DEFAULT})
set(RTA_FILTER_RUNTIME_ARGS)
if(RTA_FILTER_CACHE_BIND_RUNTIME)
    list(APPEND RTA_FILTER_RUNTIME_ARGS --auto-runtime-libs
        "--runtime-probe-arg=-march=${RTA_ARCH}" --runtime-probe-arg=-D_MT
        --runtime-probe-arg=-Xclang --runtime-probe-arg=--dependent-lib=libcmt)
else()
    message(WARNING "Filter cache runtime is UNBOUND on this toolchain; runtime identity is not certified")
endif()
set(RTA_FILTER_FINGERPRINT_COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/review4_cache_fingerprint.py"
    --source "${CMAKE_SOURCE_DIR}" --compiler "${CMAKE_CXX_COMPILER}"
    "--flags=${RTA_FILTER_FINGERPRINT_FLAGS}"
    ${RTA_FILTER_RUNTIME_ARGS}
    --output "${CMAKE_BINARY_DIR}/generated/dsp/filter_build_fingerprint.h")
execute_process(COMMAND ${RTA_FILTER_FINGERPRINT_COMMAND}
    RESULT_VARIABLE RTA_CACHE_FINGERPRINT_EXIT OUTPUT_VARIABLE RTA_CACHE_FINGERPRINT_RESULT)
if(NOT RTA_CACHE_FINGERPRINT_EXIT EQUAL 0)
    message(FATAL_ERROR "Filter cache build fingerprint generation failed")
endif()
file(READ "${CMAKE_BINARY_DIR}/generated/dsp/filter_build_fingerprint.json" RTA_FILTER_FINGERPRINT_JSON)
string(JSON RTA_RUNTIME_COUNT LENGTH "${RTA_FILTER_FINGERPRINT_JSON}" linked_runtime_artifacts)
if(RTA_RUNTIME_COUNT GREATER 0)
    math(EXPR RTA_RUNTIME_LAST "${RTA_RUNTIME_COUNT} - 1")
    foreach(i RANGE 0 ${RTA_RUNTIME_LAST})
        string(JSON RTA_RUNTIME_PATH MEMBER "${RTA_FILTER_FINGERPRINT_JSON}" linked_runtime_artifacts ${i})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RTA_RUNTIME_PATH}")
    endforeach()
endif()
# Re-run at every build as well: a changed driver environment can select a new
# SDK without touching the previously discovered archives. Unchanged content
# preserves the generated header timestamp and does not force recompilation.
add_custom_target(review4_cache_fingerprint_refresh
    COMMAND ${RTA_FILTER_FINGERPRINT_COMMAND}
    BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/dsp/filter_build_fingerprint.h"
               "${CMAKE_BINARY_DIR}/generated/dsp/filter_build_fingerprint.json"
    VERBATIM)
include_directories("${CMAKE_BINARY_DIR}/generated")
add_executable(test_review4_measurement tests/test_review4_measurement.cpp)
target_link_libraries(test_review4_measurement PRIVATE rtadsp)
add_test(NAME review4_reference_contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tests/test_review4_reference.py" --exe $<TARGET_FILE:test_review4_measurement>)
add_executable(test_review4_cache tests/test_review4_cache.cpp)
target_link_libraries(test_review4_cache PRIVATE rtadsp psapi)
target_compile_definitions(test_review4_cache PRIVATE REVIEW4_FIXED _CRT_SECURE_NO_WARNINGS)
add_test(NAME review4_cache_contract COMMAND test_review4_cache)
add_executable(test_review4_dsd_contract tests/test_review4_dsd_contract.cpp)
target_link_libraries(test_review4_dsd_contract PRIVATE rtadsp)
add_test(NAME review4_dsd_contract COMMAND test_review4_dsd_contract)
get_property(RTA_REVIEW4_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS RTA_REVIEW4_TARGETS)
    if(NOT target STREQUAL "review4_cache_fingerprint_refresh")
        add_dependencies(${target} review4_cache_fingerprint_refresh)
    endif()
endforeach()

# Independently replayed application persistence and asynchronous-state regressions.
find_program(RLPLAYER_NODE_EXECUTABLE node REQUIRED)
add_test(NAME app_lyrics_state COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_lyrics_state.js")
add_test(NAME app_settings_state COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_settings_state.js")
add_test(NAME app_i18n_dynamic COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_i18n_dynamic.js")
# 2026-11-15：语言包完整性（六语种键集合一致 / 无专辑首字垃圾键 / 用户报过的文案都在）。
# 纯 JSON 判据，不启动界面；漏一条键就是一个语种在界面上露中文。
add_test(NAME i18n_packs COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_i18n_packs.js")
add_test(NAME app_queue_persistence COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_queue_persistence.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_settings_persistence COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_settings_persistence.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_prewarm COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_prewarm.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_launcher_quit COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_launcher_quit.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_upnp_token COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_upnp_token.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_http_security COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_http_security.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_design_cancel COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_design_cancel.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_playback_errors COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_playback_errors.js")
add_test(NAME app_playback_error_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_playback_error_contract.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_enrich_lyrics COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_enrich_lyrics.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_credit_roles COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_credit_roles.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_cue_entities COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_cue_entities.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_local_album COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_local_album.js")
add_test(NAME app_feedback_recovery COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_feedback_recovery.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_feedback_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_feedback_ui.js")
add_test(NAME private_material_gate COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_private_material.py")
add_test(NAME rlplayer_server COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_rlplayer_server.py")
add_test(NAME rlplayer_publish COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_rlplayer_publish.py")
add_test(NAME app_artist_cache COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_artist_cache.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME release_gate_regression COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/test_release_gates.ps1")
add_test(NAME asio_probe_fill COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_asio_probe_fill.py" --compiler "${CMAKE_CXX_COMPILER}")
add_test(NAME safe_manifest_controls COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test_safe_manifest.py")
add_test(NAME hip_provenance_controls COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test_hip_provenance.py")
add_test(NAME cuda_release_controls COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test_cuda_release_gate.py")
add_executable(test_cache_transactions tests/test_cache_transactions.cpp)
target_link_libraries(test_cache_transactions PRIVATE rtadsp)
target_compile_definitions(test_cache_transactions PRIVATE RTA_FILTER_CACHE_TESTING)
add_test(NAME cache_transactions COMMAND test_cache_transactions)
add_executable(test_phase_memory_contract tests/test_phase_memory_contract.cpp)
target_link_libraries(test_phase_memory_contract PRIVATE rtadsp)
add_test(NAME phase_memory_contract COMMAND test_phase_memory_contract)
add_executable(test_rational_cascade tests/test_rational_cascade.cpp)
target_link_libraries(test_rational_cascade PRIVATE rtadsp)
add_test(NAME rational_cascade COMMAND test_rational_cascade)
add_executable(test_resampler_plan tests/test_resampler_plan.cpp)
target_link_libraries(test_resampler_plan PRIVATE rtadsp)
target_compile_definitions(test_resampler_plan PRIVATE RTA_PLAN_CHECKS)
add_test(NAME resampler_plan COMMAND test_resampler_plan)
add_executable(test_pipeline_downsample tests/test_pipeline_downsample.cpp)
target_link_libraries(test_pipeline_downsample PRIVATE rtadsp)
add_test(NAME pipeline_downsample COMMAND test_pipeline_downsample)
add_executable(test_design_cancel tests/test_design_cancel.cpp)
target_link_libraries(test_design_cancel PRIVATE rtadsp)
add_test(NAME design_cancel COMMAND test_design_cancel)
set_tests_properties(design_cancel PROPERTIES ENVIRONMENT "RLPLAYER_FILTER_CACHE=0")
add_executable(dsp_export_pipeline tools/dsp_export_pipeline.cpp)
target_link_libraries(dsp_export_pipeline PRIVATE rtadsp)
foreach(target test_cache_transactions test_phase_memory_contract test_rational_cascade test_resampler_plan test_pipeline_downsample test_design_cancel dsp_export_pipeline)
    add_dependencies(${target} review4_cache_fingerprint_refresh)
endforeach()

# Independent arbitrary-precision test oracle. This Python dependency is only
# needed to build/test the source tree; playback has no Python or Flint runtime.
set(RTA_CERTIFICATE_PYTHON_PATH "" CACHE PATH "Private directory containing audit Python dependencies")
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import sys; sys.path.insert(0,sys.argv[1]); import flint; print(flint.__version__)"
    "${RTA_CERTIFICATE_PYTHON_PATH}"
    RESULT_VARIABLE RTA_FLINT_EXIT OUTPUT_VARIABLE RTA_FLINT_VERSION ERROR_VARIABLE RTA_FLINT_ERROR)
if(NOT RTA_FLINT_EXIT EQUAL 0)
    message(FATAL_ERROR "Independent certificate dependency missing. Install tools/requirements-audit.txt into a private directory and configure -DRTA_CERTIFICATE_PYTHON_PATH=<directory>. ${RTA_FLINT_ERROR}")
endif()
foreach(kind spectral cascade single_rate)
    add_test(NAME review5_${kind}_certificate COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/test_review5_${kind}_certificate.py"
        --flint-path "${RTA_CERTIFICATE_PYTHON_PATH}"
        --evidence "${CMAKE_BINARY_DIR}/review5-tests/${kind}")
endforeach()
add_executable(review5_precision_design tools/review5_precision_design.cpp)
target_link_libraries(review5_precision_design PRIVATE rtadsp)
add_dependencies(review5_precision_design review4_cache_fingerprint_refresh)
add_test(NAME review5_precision_design COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tests/test_review5_precision_design.py"
    --exe $<TARGET_FILE:review5_precision_design>
    --flint-path "${RTA_CERTIFICATE_PYTHON_PATH}"
    --evidence "${CMAKE_BINARY_DIR}/review5-tests/precision")
add_executable(test_multilevel_sdm_paths tests/test_multilevel_sdm_paths.cpp)
target_link_libraries(test_multilevel_sdm_paths PRIVATE rtadsp)
add_dependencies(test_multilevel_sdm_paths review4_cache_fingerprint_refresh)
add_test(NAME multilevel_sdm_paths COMMAND test_multilevel_sdm_paths)
add_executable(test_sdm_config_validation tests/test_sdm_config_validation.cpp)
target_link_libraries(test_sdm_config_validation PRIVATE rtadsp)
add_dependencies(test_sdm_config_validation review4_cache_fingerprint_refresh)
add_test(NAME sdm_config_validation COMMAND test_sdm_config_validation)
add_test(NAME audio_clock_units COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_audio_clock_units.py" "${CMAKE_CXX_COMPILER}")
add_executable(rlmultilevel tools/rlmultilevel.cpp)
target_link_libraries(rlmultilevel PRIVATE rtadsp shell32)
add_dependencies(rlmultilevel review4_cache_fingerprint_refresh)
add_executable(test_multilevel_file tests/test_multilevel_file.cpp)
target_link_libraries(test_multilevel_file PRIVATE rtadsp)
add_dependencies(test_multilevel_file review4_cache_fingerprint_refresh)
add_test(NAME multilevel_format COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_multilevel_format.py" --writer $<TARGET_FILE:test_multilevel_file>)
add_test(NAME multilevel_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_multilevel_cli.py" --exe $<TARGET_FILE:rlmultilevel>)
add_executable(test_multilevel_wav_stream tests/test_multilevel_wav_stream.cpp)
target_include_directories(test_multilevel_wav_stream PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME multilevel_wav_stream COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_multilevel_wav_stream.py" --reader $<TARGET_FILE:test_multilevel_wav_stream>)
add_executable(test_gpu_pipeline_real tests/test_gpu_pipeline_real.cpp)
target_link_libraries(test_gpu_pipeline_real PRIVATE rtadsp)
add_dependencies(test_gpu_pipeline_real review4_cache_fingerprint_refresh)
add_test(NAME gpu_pipeline_real COMMAND test_gpu_pipeline_real)
add_executable(test_filter_input_validation tests/test_filter_input_validation.cpp)
target_link_libraries(test_filter_input_validation PRIVATE rtadsp)
target_compile_definitions(test_filter_input_validation PRIVATE RTA_VALIDATION_OVERFLOW_CHECK=1)
add_dependencies(test_filter_input_validation review4_cache_fingerprint_refresh)
add_test(NAME filter_input_validation COMMAND test_filter_input_validation)
add_executable(test_cache_observability tests/test_cache_observability.cpp)
target_link_libraries(test_cache_observability PRIVATE rtadsp)
add_dependencies(test_cache_observability review4_cache_fingerprint_refresh)
add_test(NAME cache_observability COMMAND test_cache_observability)
add_executable(test_resampler_capacity tests/test_resampler_capacity.cpp)
target_link_libraries(test_resampler_capacity PRIVATE rtadsp)
add_dependencies(test_resampler_capacity review4_cache_fingerprint_refresh)
add_test(NAME resampler_capacity COMMAND test_resampler_capacity)
add_test(NAME app_enrich_budget COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_enrich_budget.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_enrich_budget_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_enrich_budget_ui.js")
add_test(NAME app_sdm_status COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_sdm_status.js")
add_test(NAME app_sdm_signal_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_sdm_signal_contract.py" "${CMAKE_CXX_COMPILER}")

add_test(NAME app_writeback_transaction COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_writeback_transaction.py" "${CMAKE_CXX_COMPILER}")

add_test(NAME app_roomcorr_auto COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_roomcorr_auto.py" "${CMAKE_CXX_COMPILER}")

add_test(NAME app_manual_ir COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_manual_ir.py" "${CMAKE_CXX_COMPILER}")

add_test(NAME app_writeback_cover_real COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_writeback_cover_real.py" "${CMAKE_CXX_COMPILER}" "ffmpeg.exe" "ffmpeg-lite/ffmpeg.exe" "ffmpeg-lite/ffprobe.exe")

add_test(NAME update_failure_process COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_update_failure_process.py" "$<TARGET_FILE:update_probe>")
add_test(NAME update_intent_recovery COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_update_intent_recovery.py" --probe "$<TARGET_FILE:update_probe>")
add_test(NAME update_delete_locked COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_update_delete_locked.py" --probe "$<TARGET_FILE:update_probe>" --compiler "${CMAKE_CXX_COMPILER}")

add_test(NAME launcher_startup_exit COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_launcher_startup_exit.py" "${CMAKE_CXX_COMPILER}")

add_test(NAME app_update_recovery COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_update_recovery.js")
add_test(NAME app_update_manual COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_update_manual.js")

add_test(NAME app_playback_caption COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_playback_caption.js")

add_test(NAME app_sdm_corner COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_sdm_corner.js")

add_executable(test_sdm_output_policy tests/test_sdm_output_policy.cpp)
target_link_libraries(test_sdm_output_policy PRIVATE rtadsp)
add_test(NAME sdm_output_policy COMMAND test_sdm_output_policy)

# Cover/album identity contract: all fixtures and caches stay in private test directories.
add_executable(test_album_cover_identity tests/test_album_cover_identity.cpp tests/test_wav_japanese_metadata.cpp)
target_include_directories(test_album_cover_identity PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_album_cover_identity PRIVATE winhttp ws2_32)
add_test(NAME album_cover_identity COMMAND test_album_cover_identity "ffmpeg.exe")
add_test(NAME wav_japanese_metadata COMMAND test_album_cover_identity --japanese)
add_test(NAME track_cover_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_track_cover_ui.js")
add_test(NAME track_cover_resolution COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_track_cover_resolution.py"
  --compiler "${CMAKE_CXX_COMPILER}"
  --exe "$<TARGET_FILE:rtaweb>" --ffmpeg "ffmpeg.exe" --product-ffmpeg "ffmpeg-lite/ffmpeg.exe"
  --ffprobe "ffmpeg-lite/ffprobe.exe" --evidence-dir "track-cover-evidence")

# 3.3 correctness and format acceptance: safe runner stages each executable/profile.
add_executable(test_audio_integrity_round tests/test_audio_integrity_round.cpp)
target_link_libraries(test_audio_integrity_round PRIVATE rtadsp)
add_dependencies(test_audio_integrity_round review4_cache_fingerprint_refresh)
add_test(NAME audio_integrity_round COMMAND test_audio_integrity_round)

add_executable(test_cast_stream_prepare tests/test_cast_stream_prepare.cpp)
target_link_libraries(test_cast_stream_prepare PRIVATE rtadsp winhttp ws2_32)
add_dependencies(test_cast_stream_prepare review4_cache_fingerprint_refresh)
add_test(NAME cast_stream_prepare COMMAND test_cast_stream_prepare)

add_executable(test_sacd_iso tests/test_sacd_iso.cpp)
target_link_libraries(test_sacd_iso PRIVATE rtadsp)
add_test(NAME sacd_iso COMMAND test_sacd_iso)

add_test(NAME app_dsp_alignment COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_dsp_alignment.py" "${CMAKE_CXX_COMPILER}")
add_test(NAME app_upnp_queue COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_upnp_queue.py" "$<TARGET_FILE:rtaweb>")
set_tests_properties(app_upnp_queue PROPERTIES RUN_SERIAL TRUE)
add_test(NAME quick_start COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_quick_start.js")

add_executable(test_recording_cleanup tests/test_recording_cleanup.cpp)
target_link_libraries(test_recording_cleanup PRIVATE rtadsp)
add_dependencies(test_recording_cleanup review4_cache_fingerprint_refresh)
add_test(NAME recording_cleanup COMMAND test_recording_cleanup)
add_test(NAME recording_cleanup_oracle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_recording_cleanup_oracle_run.py" "$<TARGET_FILE:test_recording_cleanup>")

add_executable(test_dst_codec tests/test_dst_codec.cpp)
target_link_libraries(test_dst_codec PRIVATE rtadsp)
add_test(NAME dst_codec COMMAND test_dst_codec)
add_executable(test_udf_navigation tests/test_udf_navigation.cpp)
target_link_libraries(test_udf_navigation PRIVATE rtadsp)
add_test(NAME udf_navigation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_udf_navigation_fixture.py" "$<TARGET_FILE:test_udf_navigation>")
add_executable(test_dvd_audio tests/test_dvd_audio.cpp)
target_link_libraries(test_dvd_audio PRIVATE rtadsp winhttp ws2_32)
add_executable(test_image_library tests/test_image_library.cpp)
target_link_libraries(test_image_library PRIVATE rtadsp winhttp ws2_32)
add_executable(test_source_depth_migration tests/test_source_depth_migration.cpp)
target_link_libraries(test_source_depth_migration PRIVATE rtadsp winhttp ws2_32)
add_test(NAME source_depth_migration COMMAND test_source_depth_migration)
add_test(NAME iso_end_to_end COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_iso_end_to_end.py"
  --exe "$<TARGET_FILE:rtaweb>" --sacd "$<TARGET_FILE:test_sacd_iso>"
  --dvd "$<TARGET_FILE:test_dvd_audio>" --library "$<TARGET_FILE:test_image_library>"
  --encoder "ffmpeg.exe" --decoder "ffmpeg-lite/ffmpeg.exe" --probe "ffmpeg-lite/ffprobe.exe")
set_tests_properties(iso_end_to_end PROPERTIES RUN_SERIAL TRUE TIMEOUT 240)
add_test(NAME app_preset_processing COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_app_preset_processing.py" "$<TARGET_FILE:rtaweb>")
add_test(NAME ui_processing COMMAND "${RLPLAYER_NODE_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_ui_processing.js" "$<TARGET_FILE:rtaweb>")
set_tests_properties(ui_processing PROPERTIES RUN_SERIAL TRUE TIMEOUT 240)
add_executable(test_cast_iso tests/test_cast_iso.cpp)
target_link_libraries(test_cast_iso PRIVATE rtadsp winhttp ws2_32)
add_test(NAME iso_upnp COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_iso_upnp.py"
  --exe "$<TARGET_FILE:rtaweb>" --sacd "$<TARGET_FILE:test_sacd_iso>" --cast "$<TARGET_FILE:test_cast_iso>"
  --encoder "ffmpeg.exe" --decoder "ffmpeg-lite/ffmpeg.exe" --probe "ffmpeg-lite/ffprobe.exe")
set_tests_properties(iso_upnp PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
add_test(NAME startup_queue COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_startup_queue.py"
  --exe "$<TARGET_FILE:rtaweb>")
set_tests_properties(startup_queue PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)

# 3.3.1 artwork regression: private profiles, local HTTP and actual decoded pixels.
add_executable(test_profile_artwork_migration tests/test_profile_artwork_migration.cpp)
target_include_directories(test_profile_artwork_migration PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_profile_artwork_migration PRIVATE bcrypt)
add_test(NAME profile_artwork_migration COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_profile_artwork_migration.py"
  --probe "$<TARGET_FILE:test_profile_artwork_migration>" --evidence-dir "profile-artwork-evidence")
set_tests_properties(profile_artwork_migration PROPERTIES TIMEOUT 180)
add_test(NAME cover_local_resolution COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_cover_local_resolution.py"
  --exe "$<TARGET_FILE:rtaweb>" --encoder "ffmpeg.exe"
  --decoder "ffmpeg-lite/ffmpeg.exe" --probe "ffmpeg-lite/ffprobe.exe"
  --compiler "${CMAKE_CXX_COMPILER}" --evidence-dir "cover-local-evidence")
set_tests_properties(cover_local_resolution PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
add_test(NAME cover_refresh_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_cover_refresh_ui.js"
  --source "${CMAKE_SOURCE_DIR}" --work-root "cover-refresh-evidence" --cxx "${CMAKE_CXX_COMPILER}")
set_tests_properties(cover_refresh_ui PROPERTIES TIMEOUT 240)

add_executable(test_cover_sources tests/test_cover_sources.cpp)
target_include_directories(test_cover_sources PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_cover_sources PRIVATE winhttp)
add_test(NAME cover_sources COMMAND test_cover_sources)
set_tests_properties(cover_sources PROPERTIES TIMEOUT 120)

add_test(NAME cover_source_controller COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_cover_source_controller.py"
  --exe "$<TARGET_FILE:rtaweb>" --encoder "ffmpeg.exe"
  --decoder "ffmpeg-lite/ffmpeg.exe" --probe "ffmpeg-lite/ffprobe.exe"
  --evidence-dir "cover-source-controller-evidence")
set_tests_properties(cover_source_controller PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)

# 3.3.2 preferences, conservative cleanup learning and failed-check throttling.
add_test(NAME ui_preferences_backend COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_ui_preferences_backend.py")
set_tests_properties(ui_preferences_backend PROPERTIES TIMEOUT 180)
add_test(NAME quickstart_preference COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_quickstart_preference.js"
  --work-root "quickstart-preference-evidence")
set_tests_properties(quickstart_preference PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
add_test(NAME cleanup_auto_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_cleanup_auto_ui.js" --work-root "cleanup-auto-ui-evidence")
set_tests_properties(cleanup_auto_ui PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME listening_modes_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_listening_modes.js" --work-root "listening-modes-evidence")
set_tests_properties(listening_modes_ui PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME transport_usability_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_transport_usability.js" --work-root "transport-usability-evidence")
set_tests_properties(transport_usability_ui PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_executable(test_recording_cleanup_auto tests/test_recording_cleanup_auto.cpp)
target_link_libraries(test_recording_cleanup_auto PRIVATE rtadsp)
add_dependencies(test_recording_cleanup_auto review4_cache_fingerprint_refresh)
add_test(NAME recording_cleanup_auto COMMAND test_recording_cleanup_auto)
set_tests_properties(recording_cleanup_auto PROPERTIES TIMEOUT 180)
add_test(NAME recording_cleanup_auto_oracle COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_recording_cleanup_auto_oracle.py"
  "$<TARGET_FILE:test_recording_cleanup_auto>" --work-dir "cleanup-auto-oracle-evidence")
set_tests_properties(recording_cleanup_auto_oracle PROPERTIES TIMEOUT 180)
add_test(NAME update_check_retry COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_update_check_retry.py" --exe "$<TARGET_FILE:rtaweb>")
set_tests_properties(update_check_retry PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME rlplayer_shared_quota COMMAND "${Python3_EXECUTABLE}" -m unittest discover
  -s "${CMAKE_SOURCE_DIR}/tests" -p test_shared_monthly_quota.py -v)
set_tests_properties(rlplayer_shared_quota PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# Unified listening controls and session-preserving digital comparison.
add_test(NAME workbench_switch COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_workbench_switch.py" "$<TARGET_FILE:rtaweb>"
  --make-dsd "$<TARGET_FILE:make_dsd>" --compiler "${CMAKE_CXX_COMPILER}" --evidence "workbench-evidence")
set_tests_properties(workbench_switch PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
add_test(NAME workbench_profiles_ui COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_workbench_profiles_ui.js")
set_tests_properties(workbench_profiles_ui PROPERTIES TIMEOUT 60)
add_test(NAME digital_console335 COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_digital_console335.js" --source "${CMAKE_SOURCE_DIR}"
  --work-root "console-evidence")
set_tests_properties(digital_console335 PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME workspace335_integration COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_workspace335_integration.js" --source "${CMAKE_SOURCE_DIR}"
  --work-root "workspace-evidence")
set_tests_properties(workspace335_integration PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# 3.3.6: real production WASAPI packing and pause/resume continuity.
if(WIN32)
  add_executable(test_pause_resume336 tests/test_pause_resume336.cpp)
  target_include_directories(test_pause_resume336 PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(test_pause_resume336 PRIVATE ole32 uuid mmdevapi)
  add_test(NAME pause_resume336 COMMAND test_pause_resume336)
  set_tests_properties(pause_resume336 PROPERTIES TIMEOUT 120)
  # 3.3.10: physical pace guard (ZiSan 1.536 MHz exclusive: GetCurrentPadding does not throttle)
  add_executable(test_pace_guard tests/test_pace_guard.cpp)
  target_include_directories(test_pace_guard PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(test_pace_guard PRIVATE ole32 uuid mmdevapi)
  add_test(NAME pace_guard COMMAND test_pace_guard)
  set_tests_properties(pace_guard PROPERTIES TIMEOUT 60)
  add_executable(test_library_sources336 tests/test_library_sources336.cpp)
  target_link_libraries(test_library_sources336 PRIVATE rtadsp winhttp ole32 uuid)
  add_test(NAME library_sources336 COMMAND test_library_sources336)
  set_tests_properties(library_sources336 PROPERTIES TIMEOUT 120)
  add_executable(test_waveform336 tests/test_waveform336.cpp)
  target_link_libraries(test_waveform336 PRIVATE rtadsp)
  add_test(NAME waveform336 COMMAND test_waveform336
    "${CMAKE_CURRENT_BINARY_DIR}/waveform336-evidence"
    "${CMAKE_SOURCE_DIR}/tests/fixtures/waveform336.flac")
  set_tests_properties(waveform336 PROPERTIES TIMEOUT 180)
endif()
add_test(NAME controls336 COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_controls336.js" --source "${CMAKE_SOURCE_DIR}"
  --work-root "controls336-evidence")
set_tests_properties(controls336 PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME workbench_profiles336 COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_profiles336.py" "$<TARGET_FILE:rtaweb>")
set_tests_properties(workbench_profiles336 PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
add_test(NAME profiles_migration336 COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_profiles_migration336.py"
  --build "${CMAKE_CURRENT_BINARY_DIR}" --evidence-dir "profiles-migration336-evidence")
set_tests_properties(profiles_migration336 PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

# 3.4.0: real dither behavior, request scheduling, and pinned release runtime.
add_executable(test_dither340 tests/test_dither340.cpp)
target_link_libraries(test_dither340 PRIVATE rtadsp)
add_test(NAME dither340 COMMAND test_dither340)
set_tests_properties(dither340 PROPERTIES TIMEOUT 120)
add_test(NAME ui_request_scheduling COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_ui_request_scheduling.js")
set_tests_properties(ui_request_scheduling PROPERTIES TIMEOUT 60)
add_test(NAME webview_runtime_pin COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_webview_pin.py")
set_tests_properties(webview_runtime_pin PROPERTIES TIMEOUT 60)
add_executable(test_crossfeed tests/test_crossfeed.cpp)
target_link_libraries(test_crossfeed PRIVATE rtadsp)
add_test(NAME crossfeed COMMAND test_crossfeed)
set_tests_properties(crossfeed PROPERTIES TIMEOUT 120)
add_test(NAME ui_shaper_guidance COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_ui_shaper_guidance.js")
set_tests_properties(ui_shaper_guidance PROPERTIES TIMEOUT 60)
add_test(NAME shaper_api340 COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_shaper_api340.py" "$<TARGET_FILE:rtaweb>"
  --cli "$<TARGET_FILE:rtaplay>" --run-root "shaper-api340-evidence")
set_tests_properties(shaper_api340 PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME ui_crossfeed_controls COMMAND "${RLPLAYER_NODE_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_ui_crossfeed_controls.js")
set_tests_properties(ui_crossfeed_controls PROPERTIES TIMEOUT 60)

add_executable(test_pcm_valid_bits340 tests/test_pcm_valid_bits340.cpp)
target_link_libraries(test_pcm_valid_bits340 PRIVATE rtadsp)
add_test(NAME pcm_valid_bits340 COMMAND test_pcm_valid_bits340)
set_tests_properties(pcm_valid_bits340 PROPERTIES TIMEOUT 120)

add_executable(test_asio_pcm_precision340 tests/test_asio_pcm_precision340.cpp)
target_link_libraries(test_asio_pcm_precision340 PRIVATE rtadsp ole32 uuid advapi32)
add_test(NAME asio_pcm_precision340 COMMAND test_asio_pcm_precision340)
set_tests_properties(asio_pcm_precision340 PROPERTIES TIMEOUT 120)

add_executable(test_pcm_auto_runtime340 tests/test_pcm_auto_runtime340.cpp)
target_link_libraries(test_pcm_auto_runtime340 PRIVATE rtadsp)
add_test(NAME pcm_auto_runtime340 COMMAND test_pcm_auto_runtime340)
set_tests_properties(pcm_auto_runtime340 PROPERTIES TIMEOUT 120)

add_executable(test_pcm_live_volume340 tests/test_pcm_live_volume340.cpp)
target_link_libraries(test_pcm_live_volume340 PRIVATE rtadsp)
add_test(NAME pcm_live_volume340 COMMAND test_pcm_live_volume340)
set_tests_properties(pcm_live_volume340 PROPERTIES TIMEOUT 120)

add_executable(test_asio_probe_options340 tests/test_asio_probe_options340.cpp)
target_link_libraries(test_asio_probe_options340 PRIVATE rtadsp)
add_test(NAME asio_probe_options340 COMMAND test_asio_probe_options340)
set_tests_properties(asio_probe_options340 PROPERTIES TIMEOUT 120)

add_test(NAME asio_selection340 COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_SOURCE_DIR}/tests/test_asio_selection340.py"
  --compiler "${CMAKE_CXX_COMPILER}" --evidence "asio-selection340-evidence")
set_tests_properties(asio_selection340 PROPERTIES TIMEOUT 120)

# 3.4.0: exact renderer EOF race and actual ASIO callback lifetime without a device.
add_executable(test_render_eof340 tests/test_render_eof340.cpp)
target_link_libraries(test_render_eof340 PRIVATE rtadsp ole32 uuid advapi32)
add_test(NAME render_eof340 COMMAND test_render_eof340)
set_tests_properties(render_eof340 PROPERTIES TIMEOUT 120)

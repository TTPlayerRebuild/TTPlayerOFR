# ttp_ofr

独立的 TTPlayer x86 OptimFROG Reader，支持 `.ofr` / `.ofs`。**静态链接 OpenFROG 源码**，不再内嵌或释放官方 `OptimFROG.dll`。

- 不依赖外部 `OpenFROG.dll` / `OptimFROG.dll` 或编码器 EXE，运行包仍只有一个插件 DLL。
- 播放、信息读取和定位不需要把解码库写入用户 TEMP；保留标准 Windows 加载与兼容 CRT，不手写 PE 映射器。
- 通过 SDK 流回调读取宿主 IStream；支持整数 8/16/24/32 bit、OFRX/OFF IEEE FLOAT32 和定位。
- 整数不经 float32。WAVE 有符号/无符号适配只翻转符号位。
- SDK 报告容器位宽；原 WAVE EXTENSIBLE 有效位从真实 HEAD 读取，不根据零样本猜测。
- APEv2/ID3v1、APE 封面、终止式 Commit、Probe/SourceInfo。不可定位流明确拒绝。
- OFS 是 DualStream 有损数据，不能把解码器输出位宽当作恢复原始无损精度的保证。
- OFRX/OFF 使用真实 COMP/DETA 配对重建 IEEE32 原始位模式，保留 ±0、subnormal、Inf 和 NaN payload；浮点不翻转符号位、不转整数或经 host float 运算。
- 损坏的浮点细节明确失败，不以静音掩盖；默认解码沿用上游不验证存储 CRC/MD5 的策略，不提供独立 verify/check 接口。

## 静态后端与边界

源自用户指定 OpenFROG 工程的干净提交 `c37926c`，24 个解码生产源码/头/许可证文件及逐文件、整体快照 SHA256 一并保存在 `third_party/openfrog`，配置/打包时核验；详见其 `ORIGIN.md`。使用 `/fp:strict` 保留预测计算语义，所有实例直接调用私有静态后端；没有运行时 DLL 搜索、提取或内存映射后备路径。

仅更新 Reader 私有解码后端，既有宿主 ABI 与 `.ofr` / `.ofs` 文件过滤不变（OFF/OFRX 是内容格式，不要求新扩展名）。上游新增的编码器、WAVE/MD5/verify 命令行工具不加入插件，不新增 TTPlayer 编码器能力。

保留 OpenFROG 的安全拒绝：LPC 阶数超过 128、未证明的预测参数域等不支持；普通块有 64 MiB 预算，不声称穷尽全部 OptimFROG 比特流。

“不释放临时 DLL”不等于删除标签事务保护：写标签/封面仍可能在音频所在目录创建替换及备份文件，安全提交后清理；不会为此恢复用户 TEMP 中的解码 DLL。

## 构建

```powershell
./build.ps1 -Package -SourcePackage
```

需要 MSVC / Win32、CMake、Python。固定 VC-LTL 5.3.1、YY-Thunks 1.2.2；最终 DLL 执行 XP/Win7 导入检查，兼容组件支持 FetchContent 本地缓存。
不需要相邻 OpenFROG 工程或官方 SDK 下载；OpenFROG 生产源码已随本仓库和源码包提供。

运行 ZIP 仅含 `AddIn/ttp_ofr.dll`、SHA256。source 附件包含适配层、固定的 OpenFROG 生产源码、许可证及构建脚本，不包含官方 DLL/SDK/编码器。
Action 构建、审计、生成 source 附件；仅手动选择 `release_a_version` 才发布。

插件适配层与 OpenFROG 代码为 MIT；VC-LTL / YY-Thunks 的许可证保留在 `docs/licenses/`，不能将各项依赖的许可混为一谈。
本轮更新的私有测试在工作区 `rebuild/tests/ofr_openfrog_update_20261113`，覆盖真实 OFF 位模式、既有整数/OFS、跨块定位、无效 TEMP、并发和调用者 FPU；实际结果见重建版 `docs/OFR_OPENFROG_UPDATE_20261113.md`。上一轮整数限定结果仍保留在 `docs/OFR_OPENFROG_STATIC_20261112.md`，不冒充本轮验证；未实际运行的环境/原版 GUI 不作兼容性保证。

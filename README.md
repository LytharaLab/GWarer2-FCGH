# GWater2 For Con's GMod Host

**GWater2FCGH** 是 [GWater2](https://github.com/meetric1/gwater2) 面向 Windows x64 和 [Con's GMod Host（CGH）](https://github.com/LytharaLab/ConsGHost) 的非官方独立移植版。原版由 **Meetric 与 GWater2 贡献者**开发。原版支持者、翻译人员及其他贡献者的署名保留在游戏内「制作者名单」。

源代码以 [GWater2 提交 fb4fba3](https://github.com/meetric1/gwater2/tree/fb4fba381a6ef9f98bf42a67a69e86d3628ee504) 为基础。GWater2FCGH 贡献者于 **2026-11-22** 完成 CGH 原生模块适配、Win64 单包构建及界面标识更新。流体、布料、武器、实体、预设和游戏联机协议沿用上游。

*GWater2FCGH is an independent, unofficial Windows x64 port of Meetric's GWater2 for Con's GMod Host. It is maintained separately from upstream.*

## 安装

需要 Windows 版 Garry’s Mod（由 CGH 启动的 `gmod_win64.exe`）、支持 Direct3D 11 的显卡、Con's GMod Host 1.2.0，以及已启用且已安装客户端与服务端 Lua 桥的 CGH-LAPI 0.1.1。

1. 解压构建得到的 `GWater2FCGH.zip`，把整个 `GWater2FCGH` 文件夹放进 CGH Launcher 的 `Mods`。
2. 在 Steam Workshop 和本地 addons 中停用原版 GWater2，以免游戏挂载两套同名 Lua 和资源。
3. 从 CGH Launcher 启动游戏。进图后按 **G** 打开 GWater2FCGH 菜单。

CGH 从模组的 `Native/` 加载后端和 FleX，游戏内容经 `Addons/gwater2/` 挂载。GWater2FCGH 所需的原生文件都在模组包内。

## 从源码构建

克隆本仓库后初始化固定版本的子模块。构建需要 Visual Studio 2022 的 C++ x64 工具、Windows SDK、Premake 5.0.0-beta2，以及 CGH 安装包中的 SDK。

```powershell
git submodule update --init --recursive
./build-cgh.ps1 -Premake 'C:/tools/premake5.exe' -CghSdk 'C:/path/to/CGH/SDK'
```

输出为 `out/GWater2FCGH.zip`。Addon 通过 CGH-LAPI 绑定原版 Lua API。模拟和渲染直接调用原生的 `FlexSolver`、`FlexRenderer` 及其方法。

## 署名、许可与分发

- **原版与移植版。** GWater2 的原始代码和内容归 Meetric 与相应贡献者。源自 GWater2 的代码以及本移植的修改使用 [GNU GPL v3](LICENSE)。软件按现状提供，担保和责任限制见许可证第 15、16 节。
- **原生依赖。** NVIDIA FleX、AMD AGS、BSPParser、garrysmod_common、ThreadPool 与 Valve Source 1 SDK 各有自己的许可或声明。Source 1 SDK 的许可和第三方声明随源码保留，许可中列有免费分发条件。已确认的依赖许可随构建包放在 `licenses/`。 [FleX 许可](https://github.com/NVIDIAGameWorks/FleX/blob/b1ea0f87b72582649c935d53fd8531b1e7335160/LICENSE.txt) · [BSPParser 许可](https://github.com/Derpius/BSPParser/blob/d86a41b0b043d868634c7676b80abeac63dafd1a/LICENSE) · [Source 1 SDK 许可](licenses/Source-1-SDK-LICENSE.txt)
- **字体。** Roboto、Space Mono 和 Titillium Web 采用 SIL Open Font License 1.1。所附 Roboto Mono 文件的嵌入许可为 Apache 2.0，版权归 Roboto Mono Project Authors。字体自身的版权信息和 [许可证文本](licenses/) 随包保留。
- **公开发布。** 源码仓库通过固定的 Git 子模块引用 GMFS。在所用提交中未找到独立许可声明。公开发布包含 GMFS 的原生二进制前，应确认该依赖的再分发权限，并核对 FleX、Source SDK 及其他构建依赖的适用条款。根据 GPL v3 第 6 节分发二进制时，也应提供该版本的对应源码。 [GMFS 固定提交](https://github.com/Derpius/GMFS/tree/7ca6cd94fb8912d549cc0c288c437e46232929e4) · [GPL v3 第 6 节](https://www.gnu.org/licenses/gpl-3.0.html#section6)
- **项目关系。** 本项目独立维护，没有原版作者、Facepunch Studios、Valve 或 NVIDIA 的官方背书。产品名称及商标归各自权利人，本文中的名称用于说明来源和兼容性。

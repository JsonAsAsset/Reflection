<br/>
<div align="center">
  <!--
  Also popularly known as "JAA", standing for Json As Asset.
  -->
  
  <a href="https://github.com/JsonAsAsset/Reflection">
    <p>
      <img src="https://github.com/user-attachments/assets/8bfa632d-876f-4165-a720-98e93563e877" alt="Logo" width="200">
    </p>
  </a>

  <h3 align="center">Reflection</h3>

  <p align="center">
    Reconstruction Toolkit for Unreal Engine
    <br />
    <a href="#table-of-contents"><strong>Explore the docs »</strong></a>
  </p>
</div>

<div align="center">
<br/>

[![GitHub Repo stars](https://img.shields.io/github/stars/JsonAsAsset/Reflection?style=for-the-badge&logo=&color=fcca03)](/../../stargazers)
[![GitHub Downloads Total Count (all assets, all releases)](https://img.shields.io/github/downloads/JsonAsAsset/Reflection/total?style=for-the-badge&label=DOWNLOADS&color=05c1ff)](/../../releases)

[![Discord](https://img.shields.io/badge/Join%20Discord-Collector?color=0363ff&logo=discord&logoColor=white&style=for-the-badge)](https://discord.gg/xXEw4jc2UT)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-Ko--fi?color=ff0de7&logo=ko-fi&logoColor=white&style=for-the-badge)](https://ko-fi.com/tectory)

[![Unreal Engine 5 Supported)](https://img.shields.io/badge/5.0+-black?logo=unrealengine&style=for-the-badge&labelColor=grey)](#installation)
[![Unreal Engine 4.27.2 Supported)](https://img.shields.io/badge/4.27.2-black?logo=unrealengine&style=for-the-badge&labelColor=grey)](#installation)
[![Unreal Engine 4.26.2 Supported)](https://img.shields.io/badge/4.26.2-black?logo=unrealengine&style=for-the-badge&labelColor=grey)](#installation)
[![Unreal Engine 4.26.0 Supported)](https://img.shields.io/badge/4.26.0-black?logo=unrealengine&style=for-the-badge&labelColor=grey)](#installation)

</div>

### Description

Reflection is an [Unreal Engine](https://www.unrealengine.com/en-US) plugin that reads [JSON](https://www.json.org/json-en.html) files from FModel [(UEParse)](https://github.com/FabianFG/CUE4Parse), and rebuilds assets like materials, data tables, physics assets, and more.

✨ [Contributors](#contribute)

### Example Use

* Importing **materials**, data assets, and data tables — [see more](#asset-types)
* Porting **physics assets** for skeletal meshes
* Porting **sound effects** to Unreal Engine
* Automating asset porting workflows

This project aims to streamline the porting and modding experience, making it easier to bring game assets into Unreal Engine.

<a name="licensing"></a>
### Licensing

Reflection is licensed under the MIT License. Read more in the [LICENSE](https://github.com/JsonAsAsset/Reflection/blob/main/LICENSE) file. The plugin also uses [Detex](https://github.com/hglm/detex) and [NVIDIA Texture Tools](https://docs.nvidia.com/texture-tools/index.html).

-----------------

### **Table of Contents**

> 1. [Asset Types](#asset-types)  
> 2. [Installation](#installation)  
> 3. [→ Workflow](#workflow)

**Extras**:
<br>
> - [Common Errors 🐛](#common-errors)

-----------------

> [!CAUTION]
> Please note that this plugin is intended solely for **personal and educational use**.
> 
> Do not use it to create or distribute **commercial products** without obtaining the necessary **licenses and permissions**. It is important to respect **intellectual property rights** and only use assets that you are **authorized to use**.
>
> We **do not assume any responsibility** for the way the created content is used.

-----------------

<a name="asset-types"></a>
## Asset Types
If an asset type isn't listed below, **it's not currently supported by the plugin**.

|  | Asset Types |
|--------------------------------|------------------------------------------------------------------------------------------------------------------------|
| 🟢 **Curve** | CurveFloat, CurveVector, CurveLinearColor, CurveLinearColorAtlas |
| 🟣 **Data** | DataAsset, SlateBrushAsset, SlateWidgetStyleAsset, AnimBoneCompressionSettings, AnimCurveCompressionSettings, UserDefinedEnum, UserDefinedStruct |
| 🔵 **Table** | CurveTable, DataTable, StringTable |
| 🟠 **Material** | Material, MaterialFunction, MaterialInstanceConstant, MaterialParameterCollection, SubsurfaceProfile |
| 🟧 **Texture** | TextureRenderTarget2D, RuntimeVirtualTexture |
| 🟡 **Sound** | Most/all sound classes are supported. SoundWave is downloaded by a [Cloud Server](#cloud)! |
| 🔴 **Animation** | PoseAsset, Skeleton, SkeletalMeshLODSettings, BlendSpace, BlendSpace1D, AimOffsetBlendSpace, AimOffsetBlendSpace1D |
| ⚪ **Physics** | PhysicsAsset, PhysicalMaterial |
| 🟤 **Sequencer** | CameraAnim |
| ✨ **Effects** | ParticleSystem |
| 🟩 **Landscape** | LandscapeGrassType, FoliageType_InstancedStaticMesh, FoliageType_Actor |

#### The following asset types add onto a pre-existing asset
|  | Asset Types |
|-----------------------------------|------------------------------------------------------------------------------------------------------------------------------------|
| **🔴 Animation** | AnimSequence, AnimMontage **(Animation Curves)** |

#### Partial Blueprint Importing
If the parent class exists, it'll import the components and defaults of the blueprint.

<a name="material-data-prerequisites"></a>
#### 🟠 Material Data Prerequisites
Unreal Engine games made below 4.12 (a guess) will have material data. *Games made above that version will most definitely not have any material data*, and therefore the actual data will be stripped and cannot be imported. **Unless you are using a User Generated Content editor**, then it's possible material data will be there.

#### 🟣 C++ Classes Prerequisites
If your game uses custom C++ classes or structures, **you need to define them**.

See [Unreal Engine Modding Projects](https://github.com/Buckminsterfullerene02/UE-Modding-Tools?tab=readme-ov-file#game-specific-template-projects) for game-specific template projects.

<a name="installation"></a>
## Installation
[<img align="left" width="150" src="https://github.com/user-attachments/assets/d8e4f9c9-1268-4aee-ab1a-dabee31b3069?raw=true">](https://fmodel.app)

> [!IMPORTANT]
> If you haven't already, **install [FModel](https://fmodel.app) and set it up correctly, then proceed with the setup**.
>
>
> ​There is a specific FModel version for **material data support** found in the discord server.            
> [**Material Data Prerequisites still apply.**](#material-data-prerequisites)
>  ​

We strongly recommend using the latest **commit** of Reflection to ensure compatibility with recent Unreal updates and access to the newest features.

<details>
  <summary>UE4</summary>
  <br>
  
Some UE4 builds may fail to locate **UnrealBuildTool.exe** due to missing platform path definitions. You can fix this by manually adding a `PlatformPaths` entry to `BaseEngine.ini`.

#### Steps

1. Go to your Unreal Engine install directory:
   ```
   UE_[VERSION]/Engine/Config/
   ```
2. Open:
   ```
   BaseEngine.ini
   ```
3. Scroll to the very bottom of the file and add:
   ```ini
   [PlatformPaths]
   UnrealBuildTool=Engine/Binaries/DotNet/UnrealBuildTool.exe
   ```
4. Save the file.
</details>

Follow these steps to install **Reflection**:

1. **Downloading Reflection:**   
   There are two ways you can download Reflection. You can either stay up-to-date with the latest development changes (recommended if you want the newest features and fixes, but may occasionally require manual fixing), or download stable releases.

   1. **Up-to-date (Git Submodule)**   
      Open a terminal in your Unreal project root (where your `.uproject` file is located), then run:

      ```sh
      git submodule add https://github.com/JsonAsAsset/Reflection.git Plugins/Reflection
      git submodule update --init --recursive
      ```

   2. **Stable Releases (Manual Download)**   
      Go to the [Releases page](/../../releases) for the plugin.
      Download the **Source code (zip)**, extract it into your project's `Plugins` folder (create it if it doesn't exist), so the final path looks like:

      ```
      Project/Plugins/Reflection/
      ```
3. **Compile Reflection:**   
   Follow [**this tutorial**](https://dev.epicgames.com/community/learning/tutorials/qz93/unreal-engine-building-plugins) to build the plugin for your Unreal Engine version.
4. **Launch Your Project:**   
   Open your Unreal Engine project.

<a name="cloud"></a>
### Set up the Cloud ✨
Make sure Cloud is enabled in Plugin Settings, and set up a Cloud Server. [Click here to get to Cloud releases.](https://github.com/Reflection/Core/releases)

Once the Cloud is started, Reflection will fetch almost every referenced asset for you hands-free.

<a name="workflow"></a>
## Workflow

1. Find an asset in [FModel](https://fmodel.app), and save it by right-clicking and pressing `Save Properties`. Locate the file on your computer and copy the location.

2. Press the [Reflection](https://github.com/JsonAsAsset/Reflection) button on your tool-bar, and a file import should pop up. <img align="right" width="206" height="96" src=https://github.com/user-attachments/assets/6a9bf925-484b-4c23-b0ed-c59d58c5d07c>

3. Select the file and press Open.

4. The asset will import, and bring you to the created asset in the content browser.

<a name="common-errors"></a>
### Common Errors 🐛
<details>
  <summary>Assertion failed: TextureReferenceIndex != INDEX_NONE</summary>

------------
  
This is a known issue in our code that we haven't fully resolved yet. While previous attempts to fix it have been unsuccessful, here's a partial solution to reduce its occurrence:

- Re-launch your Unreal Engine project, go to Reflection's plugin settings and enable ***"Disconnect Root"***. Also enable ***"Save Assets"***.
</details>

<a name="contribute"></a>
## ✨ Contributors

Thanks go to these wonderful people:

<a href="https://github.com/JsonAsAsset/Reflection/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=JsonAsAsset/Reflection" />
</a>

<!-- prettier-ignore-start -->
<!-- markdownlint-disable -->
<table>
  <tr>
    <td align="center"><a href="https://github.com/Tectfy"><img src="https://github.com/Tectfy.png" width="150px"/><br/><sub><b>Tector</b></sub></a><br/>Creator</td>
    <td align="center"><a href="https://github.com/GMatrixGames"><img src="https://github.com/GMatrixGames.png" width="130px"/><br/><sub><b>GMatrixGames</b></sub></a><br/>Co-Founder</td>
  </tr>
</table>

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-Ko--fi?color=ff0de7&logo=ko-fi&logoColor=white&style=for-the-badge)](https://ko-fi.com/tectory)

- Thanks to the people who contributed to [UEAssetToolkit](https://github.com/Buckminsterfullerene02/UEAssetToolkit-Fixes)! They have helped a lot.
- Also, thanks to the people who contributed to [Advanced Locomotion System V4](https://www.fab.com/listings/ef9651a4-fb55-4866-a2d9-1b38b028f9c7)!
- Logo uses a font by [Brylark](https://ko-fi.com/brylark), support him at his ko-fi!

##### Cloning Reflection
```
git clone https://github.com/JsonAsAsset/Reflection --recursive
```
##### Adding Reflection as a sub-module
```
git submodule add https://github.com/JsonAsAsset/Reflection Plugins/Reflection
git submodule update --init --recursive
```
#### Reflection
Reflection uses data based off [UEParse](https://github.com/FabianFG/CUE4Parse)'s JSON export format.

#### Cloud ☁️
Cloud Server's API is located at [JsonAsAsset/Core](https://github.com/JsonAsAsset/Core/tree/main/Source/Core.Cloud)

#### Settings
Link: [`Public/Settings/ReflectionSettings.h`](https://github.com/JsonAsAsset/Reflection/blob/main/Source/Reflection/Public/Settings/ReflectionSettings.h)

#### Adding Asset Types
> *Asset types without manual code will use **basic** importing, meaning it will only take the properties of the base object and import them.*
- Normal Asset types are found in [`Reflection/Private/Importers/Constructor/Types.cpp`](https://github.com/JsonAsAsset/Reflection/blob/main/Source/Reflection/Private/Importers/Constructor/Types.cpp#9) You don't need to add anything here if you made a custom IImporter with the REGISTER_IMPORTER macro.

#### Custom Logic for Asset Types

Adding **manual** asset type imports is done in the [`Reflection/Public/Importers/Types`](https://github.com/JsonAsAsset/Reflection/tree/main/Source/Reflection/Public/Importers/Types) folder. Use other importers for reference on how to create one.

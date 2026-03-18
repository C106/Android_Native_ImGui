

# 通过 GUObjectArray 遍历查找指定类的所有实例对象

## 1. 核心数据结构

### 1.1 GUObjectArray 布局

```
基址: 0x147063A0

+0x00  FCriticalSection (96 bytes)
+0x70  off_14706410    — 分片 vtable/指针
+0xE0  unk_14706480    — FChunkedFixedUObjectArray ObjObjects
+0x19C dword_1470653C  — NumElements (当前对象数量)
+0x1A0 dword_14706540  — MaxElements (最大容量, 400000)
```

### 1.2 FChunkedFixedUObjectArray 布局 (基于 0x14706480)

```
+0x00 (0x14706480)  预留/标志
+0x08 (0x14706488)  Objects[0] — 第一个 chunk 指针
       ...
+0xC8 (qword 偏移 25 起) — chunk 指针数组 (QWORD*)
+0xE8 (dword 偏移 58 起) — 累计大小数组 (DWORD*)
+0xF8 (dword_14706578)   — NumChunks
+0x100(dword_14706580)   — MaxElements
```

### 1.3 FUObjectItem (每个条目 24 字节)

```cpp
struct FUObjectItem {
    UObject* Object;          // +0x00 (8 bytes) — 对象指针
    int32    Flags;            // +0x08 (4 bytes) — 内部标志
    int32    ClusterRootIndex; // +0x0C (4 bytes)
    int32    SerialNumber;     // +0x10 (4 bytes) — 序列号/generation
    int32    Padding;          // +0x14 (4 bytes)
};  // sizeof = 24 (0x18)
```

### 1.4 UObjectBase 布局

```cpp
struct UObjectBase {
    void*    vtable;        // +0x00
    int32    ObjectFlags;   // +0x08
    int32    InternalIndex; // +0x0C
    UClass*  ClassPrivate;  // +0x10
    FName    NamePrivate;   // +0x18
    UObject* OuterPrivate;  // +0x20
};
```

---

## 2. 遍历算法

### 2.1 索引 → FUObjectItem 的分页定位

`FChunkedFixedUObjectArray` 使用分页存储。给定全局索引 `Index`，定位步骤如下：

```
ChunkPtrArray  = (QWORD*)(0x14706480 + 25 * 8)   // chunk 指针数组
ChunkSizeArray = (DWORD*)(0x14706480 + 58 * 4)    // 累计大小数组
NumChunks      = *(DWORD*)0x14706578

LocalIndex = Index
ChunkIndex = 0

while (ChunkIndex < NumChunks && LocalIndex >= ChunkSizeArray[ChunkIndex]) {
    LocalIndex -= ChunkSizeArray[ChunkIndex]
    ChunkIndex++
}

ChunkBase = ChunkPtrArray[ChunkIndex]
FUObjectItem* Item = (FUObjectItem*)(ChunkBase + 24 * LocalIndex)
```

### 2.2 简化版（单 chunk 场景）

如果只有 1 个 chunk（`NumChunks <= 1`），可以简化为：

```
ChunkBase = *(QWORD*)0x14706488
FUObjectItem* Item = (FUObjectItem*)(ChunkBase + 24 * Index)
```

---

## 3. 查找指定类的所有实例

### 3.1 伪代码

```cpp
// 目标：查找所有 ClassPrivate == TargetClass 的 UObject

void FindAllInstancesOfClass(UClass* TargetClass, TArray<UObject*>& OutResults)
{
    int32 MaxElements = *(int32*)0x14706540;  // GUObjectArray + 0x1A0

    for (int32 Index = 0; Index < MaxElements; Index++)
    {
        // ---- 步骤1: 通过分页定位 FUObjectItem ----
        FUObjectItem* Item = GetObjectItem(Index);  // 见 2.1 节

        // ---- 步骤2: 获取 UObject 指针 ----
        UObject* Object = *(UObject**)(Item + 0x00);
        if (!Object)
            continue;

        // ---- 步骤3: 检查类匹配 ----
        UClass* ObjClass = *(UClass**)(Object + 0x10);  // ClassPrivate

        if (ObjClass == TargetClass)
        {
            OutResults.Add(Object);
        }
    }
}
```

### 3.2 包含子类的查找

如果需要查找某个类及其所有子类的实例（类似 `GetObjectsOfClass(bIncludeDerivedClasses=true)`）：

```cpp
bool IsChildOf(UClass* TestClass, UClass* ParentClass)
{
    UClass* Current = TestClass;
    while (Current)
    {
        if (Current == ParentClass)
            return true;
        Current = *(UClass**)(Current + SuperClassOffset);  // UClass::SuperStruct
    }
    return false;
}

void FindAllInstancesOfClassIncludingDerived(UClass* TargetClass, TArray<UObject*>& OutResults)
{
    int32 MaxElements = *(int32*)0x14706540;

    for (int32 Index = 0; Index < MaxElements; Index++)
    {
        FUObjectItem* Item = GetObjectItem(Index);
        UObject* Object = *(UObject**)(Item + 0x00);
        if (!Object)
            continue;

        UClass* ObjClass = *(UClass**)(Object + 0x10);
        if (IsChildOf(ObjClass, TargetClass))
        {
            OutResults.Add(Object);
        }
    }
}
```

---

## 4. 完整 C++ 实现

```cpp
#include <cstdint>
#include <vector>

// ============================================================
// 地址常量（基于分析结果）
// ============================================================
constexpr uintptr_t GUObjectArray_Base       = 0x147063A0;
constexpr uintptr_t ObjObjects_Base          = 0x14706480;
constexpr uintptr_t MaxElements_Addr         = 0x14706540;  // +0x1A0
constexpr uintptr_t NumChunks_Addr           = 0x14706578;
constexpr uintptr_t ChunkPtrArray_Offset     = 25;          // QWORD index
constexpr uintptr_t ChunkSizeArray_Offset    = 58;          // DWORD index

constexpr int UOBJECT_ITEM_SIZE = 24;

// ============================================================
// UObjectBase 偏移
// ============================================================
constexpr int OFFSET_CLASS_PRIVATE  = 0x10;   // UClass*
constexpr int OFFSET_NAME_PRIVATE   = 0x18;   // FName
constexpr int OFFSET_OUTER_PRIVATE  = 0x20;   // UObject*

// ============================================================
// 模块基址（运行时需要获取）
// ============================================================
uintptr_t ModuleBase = 0;  // 需要在运行时设置

// ============================================================
// 辅助函数
// ============================================================

uintptr_t Addr(uintptr_t offset)
{
    return ModuleBase + offset;
}

template<typename T>
T Read(uintptr_t address)
{
    return *(T*)address;
}

// ============================================================
// 分页定位: Index → FUObjectItem*
// ============================================================
uintptr_t GetObjectItem(int32_t Index)
{
    uintptr_t ObjObjectsBase = Addr(ObjObjects_Base);

    int32_t NumChunks = Read<int32_t>(Addr(NumChunks_Addr));
    int32_t LocalIndex = Index;
    int32_t ChunkIndex = 0;

    // 遍历 chunk 累计大小数组，确定 Index 属于哪个 chunk
    for (int32_t i = 0; i < NumChunks; i++)
    {
        int32_t ChunkSize = Read<int32_t>(
            ObjObjectsBase + (ChunkSizeArray_Offset + i) * sizeof(int32_t)
        );

        if (LocalIndex < ChunkSize)
            break;

        LocalIndex -= ChunkSize;
        ChunkIndex = i + 1;
    }

    // 获取 chunk 基址指针
    uintptr_t ChunkBase = Read<uintptr_t>(
        ObjObjectsBase + (ChunkPtrArray_Offset + ChunkIndex) * sizeof(uintptr_t)
    );

    // 定位到具体的 FUObjectItem
    return ChunkBase + UOBJECT_ITEM_SIZE * LocalIndex;
}

// ============================================================
// 查找指定类的所有实例
// ============================================================
struct FUObjectItem
{
    uintptr_t Object;         // +0x00
    int32_t   Flags;          // +0x08
    int32_t   ClusterRootIndex; // +0x0C
    int32_t   SerialNumber;   // +0x10
    int32_t   Padding;        // +0x14
};

std::vector<uintptr_t> FindInstancesOfClass(uintptr_t TargetClass, bool bIncludeDerived = false)
{
    std::vector<uintptr_t> Results;

    int32_t MaxElements = Read<int32_t>(Addr(MaxElements_Addr));

    for (int32_t i = 0; i < MaxElements; i++)
    {
        uintptr_t ItemAddr = GetObjectItem(i);
        uintptr_t ObjectPtr = Read<uintptr_t>(ItemAddr);  // FUObjectItem::Object

        if (!ObjectPtr)
            continue;

        uintptr_t ObjClass = Read<uintptr_t>(ObjectPtr + OFFSET_CLASS_PRIVATE);

        if (bIncludeDerived)
        {
            // 遍历继承链
            uintptr_t CurrentClass = ObjClass;
            while (CurrentClass)
            {
                if (CurrentClass == TargetClass)
                {
                    Results.push_back(ObjectPtr);
                    break;
                }
                // UStruct::SuperStruct 偏移需要根据实际版本确定
                // UE 4.18 中通常在 UStruct + 0x30 或 0x40
                CurrentClass = Read<uintptr_t>(CurrentClass + 0x40);
            }
        }
        else
        {
            if (ObjClass == TargetClass)
            {
                Results.push_back(ObjectPtr);
            }
        }
    }

    return Results;
}
```

---

## 5. 通过类名查找 UClass*

在遍历之前，通常需要先找到目标 `UClass*`。方法：

### 5.1 遍历 GUObjectArray 查找 UClass 本身

```cpp
uintptr_t FindClassByName(const char* ClassName)
{
    int32_t MaxElements = Read<int32_t>(Addr(MaxElements_Addr));

    for (int32_t i = 0; i < MaxElements; i++)
    {
        uintptr_t ItemAddr = GetObjectItem(i);
        uintptr_t ObjectPtr = Read<uintptr_t>(ItemAddr);

        if (!ObjectPtr)
            continue;

        uintptr_t ObjClass = Read<uintptr_t>(ObjectPtr + OFFSET_CLASS_PRIVATE);

        // 检查这个对象本身是否是 UClass 类型
        // UClass 的 ClassPrivate 指向 UClass 的元类
        // 简单判断：检查 FName 是否匹配

        // 读取 FName
        int32_t NameIndex = Read<int32_t>(ObjectPtr + OFFSET_NAME_PRIVATE);
        int32_t NameNumber = Read<int32_t>(ObjectPtr + OFFSET_NAME_PRIVATE + 4);

        // 通过 GNames (FNamePool) 解析 NameIndex → 字符串
        // 然后与 ClassName 比较
        const char* ResolvedName = ResolveFName(NameIndex);  // 需要实现

        if (ResolvedName && strcmp(ResolvedName, ClassName) == 0)
        {
            // 进一步验证这是一个 UClass 对象
            // （检查其 ClassPrivate 的 FName 是否为 "Class"）
            return ObjectPtr;
        }
    }

    return 0;
}
```

### 5.2 使用示例

```cpp
// 查找所有 AActor 实例
uintptr_t ActorClass = FindClassByName("Actor");
auto Actors = FindInstancesOfClass(ActorClass, true);

// 查找所有 APlayerController 实例
uintptr_t PCClass = FindClassByName("PlayerController");
auto PlayerControllers = FindInstancesOfClass(PCClass, false);

// 查找所有 USkeletalMeshComponent 实例
uintptr_t SMCClass = FindClassByName("SkeletalMeshComponent");
auto SkeletalMeshComponents = FindInstancesOfClass(SMCClass, true);
```

---

## 6. 关键地址速查表

| 用途 | 地址 | 类型 | 说明 |
|---|---|---|---|
| GUObjectArray 基址 | `0x147063A0` | struct | 完整结构体起始 |
| ObjObjects | `0x14706480` | FChunkedFixedUObjectArray | 分块对象数组 |
| Chunk 指针数组 | `ObjObjects + 25*8` | QWORD[] | 每个 chunk 的基址指针 |
| Chunk 累计大小 | `ObjObjects + 58*4` | DWORD[] | 每个 chunk 的容量 |
| NumChunks | `0x14706578` | int32 | chunk 数量 |
| MaxElements | `0x14706580` | int32 | ObjObjects 最大容量 |
| NumElements(全局) | `0x1470653C` | int32 | 当前对象总数 ≈312,778 |
| MaxUObjects(全局) | `0x14706540` | int32 | 全局最大值 = 400,000 |
| FUObjectItem 大小 | 24 bytes | — | `{Object, Flags, ClusterRoot, Serial, Pad}` |
| UObjectBase::Class | Object + 0x10 | UClass* | 对象的类指针 |
| UObjectBase::Name | Object + 0x18 | FName | 对象名称 |
| UObjectBase::Outer | Object + 0x20 | UObject* | 外部对象 |

---

## 7. 注意事项

1. **地址为静态地址**：以上所有地址是 IDA 中的静态地址，实际运行时需要加上模块基址偏移（`ASLR`）
2. **线程安全**：遍历时应获取 `0x147063A0` 处的 `FCriticalSection` 锁，或在游戏主线程执行
3. **空指针检查**：`FUObjectItem::Object` 可能为 `nullptr`（已释放的槽位），必须检查
4. **PendingKill**：`FUObjectItem::Flags` 的某些位标记对象为 `PendingKill`，根据需求决定是否跳过
5. **SuperStruct 偏移**：`IsChildOf` 中的 `SuperStruct` 偏移（示例中用 `0x40`）需要根据实际 UE 4.18 版本验证
6. **FName 解析**：查找类名需要额外实现 `GNames` / `FNamePool` 的解析逻辑
#pragma once
#include <cstdint>
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef int SceKernelMemBlockType;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 1
extern "C" {
SceUID sceKernelAllocMemBlock(const char*,SceKernelMemBlockType,SceSize,void*);
int sceKernelGetMemBlockBase(SceUID,void**);
int sceKernelFreeMemBlock(SceUID);
}

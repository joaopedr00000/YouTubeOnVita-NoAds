#pragma once
#include <psp2/kernel/sysmem.h>
#define KU_KERNEL_PROT_READ  1u
#define KU_KERNEL_PROT_WRITE 2u
#define KU_KERNEL_PROT_EXEC  4u
extern "C" int kuKernelMemProtect(void*,SceSize,unsigned);

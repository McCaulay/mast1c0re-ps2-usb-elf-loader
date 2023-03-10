#pragma once

#include <types.hpp>

#define PT_NULL    0
#define PT_LOAD    1

#pragma pack(push, 1)
class ElfProgram
{
public:
    uint32_t p_type;
    uint32_t p_offset;
    void*    p_vaddr;
    void*    p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};
#pragma pack(pop)
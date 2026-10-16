/*
 *

 * DynamicHeapArray<u8> dropped - `u8* data + size_t size` heap pair.
 */
#ifndef CUPID_UTIL_ELF_FILE_H
#define CUPID_UTIL_ELF_FILE_H

#include "common/types.h"

typedef struct Error Error;

#define ELF_EI_NIDENT   16
#define ELF_ET_EXEC     2
#define ELF_ET_DYN      3
#define ELF_EM_MIPS     8
#define ELF_SHN_UNDEF   0
#define ELF_SHT_NULL    0
#define ELF_SHT_PROGBITS 1
#define ELF_SHT_SYMTAB  2
#define ELF_SHT_STRTAB  3
#define ELF_SHT_RELA    4
#define ELF_SHT_HASH    5
#define ELF_SHT_DYNAMIC 6
#define ELF_SHT_NOTE    7
#define ELF_SHT_NOBITS  8
#define ELF_SHT_REL     9
#define ELF_SHT_SHLIB   10
#define ELF_SHT_DYNSYM  11
#define ELF_SHT_NUM     12
#define ELF_PT_NULL     0
#define ELF_PT_LOAD     1
#define ELF_PT_DYNAMIC  2
#define ELF_PT_INTERP   3
#define ELF_PT_NOTE     4
#define ELF_PT_SHLIB    5
#define ELF_PT_PHDR     6
#define ELF_PT_TLS      7

typedef struct {
  u8  e_ident[ELF_EI_NIDENT];
  u16 e_type;
  u16 e_machine;
  u32 e_version;
  u32 e_entry;
  u32 e_phoff;
  u32 e_shoff;
  u32 e_flags;
  u16 e_ehsize;
  u16 e_phentsize;
  u16 e_phnum;
  u16 e_shentsize;
  u16 e_shnum;
  u16 e_shstrndx;
} elf32_ehdr_t;

typedef struct {
  u32 sh_name;
  u32 sh_type;
  u32 sh_flags;
  u32 sh_addr;
  u32 sh_offset;
  u32 sh_size;
  u32 sh_link;
  u32 sh_info;
  u32 sh_addralign;
  u32 sh_entsize;
} elf32_shdr_t;

typedef struct {
  u32 p_type;
  u32 p_offset;
  u32 p_vaddr;
  u32 p_paddr;
  u32 p_filesz;
  u32 p_memsz;
  u32 p_flags;
  u32 p_align;
} elf32_phdr_t;

typedef struct {
  u8*    data;   /* heap, owned */
  size_t size;
} elf_file_t;

void elf_file_init(elf_file_t* ef);
void elf_file_destroy(elf_file_t* ef);

bool elf_file_is_valid_header_buf(const u8* data, size_t len, Error* err);
bool elf_file_is_valid_header    (const elf32_ehdr_t* hdr, Error* err);

const elf32_ehdr_t* elf_file_get_header(const elf_file_t* ef);
u32                 elf_file_get_entry_point(const elf_file_t* ef);

const elf32_shdr_t* elf_file_get_section(const elf_file_t* ef, u32 idx);
void                elf_file_get_section_name(const elf_file_t* ef, const elf32_shdr_t* s,
                                              const char** out_data, u32* out_len);
u32                 elf_file_get_section_count(const elf_file_t* ef);

const elf32_phdr_t* elf_file_get_program_header(const elf_file_t* ef, u32 idx);
u32                 elf_file_get_program_header_count(const elf_file_t* ef);

bool elf_file_open_path(elf_file_t* ef, const char* path, Error* err);
bool elf_file_open_buffer(elf_file_t* ef, u8* data, size_t len, Error* err);

/* Per-load-segment callback.  Returns false to abort the walk. */
typedef bool (*elf_file_load_section_cb_t)(void* user, const u8* data, u32 file_len,
                                           u32 dest_vaddr, u32 dest_size, Error* err);

bool elf_file_load_executable_sections(const elf_file_t* ef, elf_file_load_section_cb_t cb,
                                       void* user, Error* err);

#endif /* CUPID_UTIL_ELF_FILE_H */

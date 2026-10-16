#include "elf_file.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(FileLoader);

static const u8 EXPECTED_ELF_HEADER[4] = { 0x7f, 'E', 'L', 'F' };
#define MAX_ELF_FILE_SIZE (32 * 1024 * 1024)

void elf_file_init(elf_file_t* ef)
{
  ef->data = NULL;
  ef->size = 0;
}

void elf_file_destroy(elf_file_t* ef)
{
  free(ef->data);
  ef->data = NULL;
  ef->size = 0;
}

const elf32_ehdr_t* elf_file_get_header(const elf_file_t* ef)
{
  return (const elf32_ehdr_t*)ef->data;
}

u32 elf_file_get_entry_point(const elf_file_t* ef)
{
  return elf_file_get_header(ef)->e_entry;
}

const elf32_shdr_t* elf_file_get_section(const elf_file_t* ef, u32 idx)
{
  const elf32_ehdr_t* h = elf_file_get_header(ef);
  if (idx == ELF_SHN_UNDEF || idx >= h->e_shnum || h->e_shentsize < sizeof(elf32_shdr_t))
    return NULL;
  const size_t off = (size_t)h->e_shoff + (size_t)idx * h->e_shentsize;
  if (off + sizeof(elf32_shdr_t) > ef->size) return NULL;
  return (const elf32_shdr_t*)(ef->data + off);
}

void elf_file_get_section_name(const elf_file_t* ef, const elf32_shdr_t* sec,
                               const char** out_data, u32* out_len)
{
  *out_data = NULL;
  *out_len  = 0;
  const elf32_shdr_t* str = elf_file_get_section(ef, elf_file_get_header(ef)->e_shstrndx);
  if (!str || sec->sh_name >= str->sh_size) return;

  const size_t base = str->sh_offset;
  const u32    start = sec->sh_name;
  u32 cur = start;
  while (cur < str->sh_size && (cur + base) < ef->size) {
    if (ef->data[base + cur] == 0) break;
    cur++;
  }
  if (cur == start) return;
  *out_data = (const char*)(ef->data + base + start);
  *out_len  = cur - start;
}

u32 elf_file_get_section_count(const elf_file_t* ef)
{
  return elf_file_get_header(ef)->e_shnum;
}

const elf32_phdr_t* elf_file_get_program_header(const elf_file_t* ef, u32 idx)
{
  const elf32_ehdr_t* h = elf_file_get_header(ef);
  if (idx >= h->e_phnum || h->e_phentsize < sizeof(elf32_phdr_t)) return NULL;
  const size_t off = (size_t)h->e_phoff + (size_t)idx * h->e_phentsize;
  if (off + sizeof(elf32_phdr_t) > ef->size) return NULL;
  return (const elf32_phdr_t*)(ef->data + off);
}

u32 elf_file_get_program_header_count(const elf_file_t* ef)
{
  return elf_file_get_header(ef)->e_phnum;
}

bool elf_file_is_valid_header(const elf32_ehdr_t* hdr, Error* err)
{
  if (memcmp(hdr->e_ident, EXPECTED_ELF_HEADER, sizeof(EXPECTED_ELF_HEADER)) != 0) {
    Error_set_string(err, "Invalid header.");
    return false;
  }
  if (hdr->e_machine != ELF_EM_MIPS) {
    Error_set_string_fmt(err, "Unsupported machine type %u.", (unsigned)hdr->e_machine);
    return false;
  }
  return true;
}

bool elf_file_is_valid_header_buf(const u8* data, size_t len, Error* err)
{
  if (len < sizeof(elf32_ehdr_t)) {
    Error_set_string(err, "Invalid header.");
    return false;
  }
  return elf_file_is_valid_header((const elf32_ehdr_t*)data, err);
}

bool elf_file_open_path(elf_file_t* ef, const char* path, Error* err)
{
  FILE* fp = fs_open_file(path, "rb", err);
  if (!fp) return false;
  const s64 sz = fs_fsize64(fp, err);
  if (sz < 0) { fclose(fp); return false; }
  if (sz >= MAX_ELF_FILE_SIZE) {
    fclose(fp);
    Error_set_string(err, "File is too large.");
    return false;
  }
  u8* buf = (u8*)malloc((size_t)sz);
  if (!buf) {
    fclose(fp);
    Error_set_string(err, "out of memory");
    return false;
  }
  if (fread(buf, (size_t)sz, 1, fp) != 1) {
    fclose(fp);
    free(buf);
    Error_set_errno_prefix(err, "fread() failed: ", errno);
    return false;
  }
  fclose(fp);
  return elf_file_open_buffer(ef, buf, (size_t)sz, err);
}

bool elf_file_open_buffer(elf_file_t* ef, u8* data, size_t len, Error* err)
{
  free(ef->data);
  ef->data = data;
  ef->size = len;

  if (len < sizeof(elf32_ehdr_t) ||
      memcmp(data, EXPECTED_ELF_HEADER, sizeof(EXPECTED_ELF_HEADER)) != 0) {
    Error_set_string(err, "Invalid header.");
    return false;
  }
  const elf32_ehdr_t* h = elf_file_get_header(ef);
  if (h->e_machine != ELF_EM_MIPS) {
    Error_set_string_fmt(err, "Unsupported machine type %u.", (unsigned)h->e_machine);
    return false;
  }
  return true;
}

bool elf_file_load_executable_sections(const elf_file_t* ef, elf_file_load_section_cb_t cb,
                                       void* user, Error* err)
{
  const u32 entry = elf_file_get_header(ef)->e_entry;
  bool loaded_entry = false;

  const u32 nph = elf_file_get_program_header_count(ef);
  for (u32 i = 0; i < nph; i++) {
    const elf32_phdr_t* ph = elf_file_get_program_header(ef, i);
    if (!ph) {
      Error_set_string_fmt(err, "Failed to find program header %u", i);
      return false;
    }
    if (ph->p_type != ELF_PT_LOAD) continue;

    const u8* sec_data = NULL;
    if (ph->p_filesz > 0) {
      if ((size_t)ph->p_offset + ph->p_filesz > ef->size) {
        Error_set_string_fmt(err, "Program header %u out of range (off=%u sz=%u file=%zu)",
                             i, (unsigned)ph->p_offset, (unsigned)ph->p_filesz, ef->size);
        return false;
      }
      sec_data = ef->data + ph->p_offset;
    }

    const u32 dest_size = ph->p_memsz > ph->p_filesz ? ph->p_memsz : ph->p_filesz;
    if (!cb(user, sec_data, ph->p_filesz, ph->p_vaddr, dest_size, err))
      return false;

    if (entry >= ph->p_vaddr && entry < ph->p_vaddr + ph->p_memsz)
      loaded_entry = true;
  }
  if (!loaded_entry) {
    Error_set_string_fmt(err, "Entry point 0x%08X not loaded.", entry);
    return false;
  }
  return true;
}

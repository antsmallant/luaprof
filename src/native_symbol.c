#define _GNU_SOURCE

#include "native_symbol.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct lp_mapping_search {
	uintptr_t address;
	uintptr_t load_bias;
	lp_native_symbol *symbol;
	bool found;
} lp_mapping_search;

static void
copy_text(char *destination, size_t capacity, size_t *length,
	const char *source) {
	if (source == NULL) {
		*length = 0;
		destination[0] = '\0';
		return;
	}
	size_t size = strlen(source);
	if (size >= capacity) {
		size = capacity - 1u;
	}
	memcpy(destination, source, size);
	destination[size] = '\0';
	*length = size;
}

static int
find_mapping(struct dl_phdr_info *info, size_t size, void *userdata) {
	(void)size;
	lp_mapping_search *search = userdata;
	bool contains = false;
	uint64_t start = UINT64_MAX;
	uint64_t limit = 0;
	uint64_t offset = 0;
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t page_mask = page_size > 0 ? (uint64_t)page_size - 1u : 4095u;
	for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
		const ElfW(Phdr) *header = &info->dlpi_phdr[i];
		if (header->p_type != PT_LOAD) {
			continue;
		}
		uint64_t segment_start = (uint64_t)info->dlpi_addr +
			((uint64_t)header->p_vaddr & ~page_mask);
		uint64_t segment_limit = (uint64_t)info->dlpi_addr +
			(((uint64_t)header->p_vaddr + header->p_memsz + page_mask) &
				~page_mask);
		if ((uint64_t)search->address >= segment_start &&
			(uint64_t)search->address < segment_limit) {
			contains = true;
		}
		if (segment_start < start) {
			start = segment_start;
			offset = (uint64_t)header->p_offset & ~page_mask;
		}
		if (segment_limit > limit) {
			limit = segment_limit;
		}
	}
	if (!contains) {
		return 0;
	}
	search->load_bias = (uintptr_t)info->dlpi_addr;
	search->symbol->mapping_start = start;
	search->symbol->mapping_limit = limit;
	search->symbol->mapping_offset = offset;
	search->symbol->has_mapping = start != UINT64_MAX && limit > start;
	if (info->dlpi_name != NULL && info->dlpi_name[0] != '\0') {
		copy_text(search->symbol->path, sizeof(search->symbol->path),
			&search->symbol->path_length, info->dlpi_name);
	}
	else {
		ssize_t length = readlink("/proc/self/exe", search->symbol->path,
			sizeof(search->symbol->path) - 1u);
		if (length > 0) {
			search->symbol->path[length] = '\0';
			search->symbol->path_length = (size_t)length;
		}
	}
	search->found = true;
	return 1;
}

static bool
range_valid(size_t offset, size_t count, size_t item_size, size_t total) {
	return item_size != 0 && offset <= total &&
		count <= (total - offset) / item_size;
}

static bool
valid_string(const char *strings, size_t size, size_t offset) {
	return offset < size && memchr(strings + offset, '\0', size - offset) != NULL;
}

static const char *
find_elf64_symbol(const unsigned char *data, size_t size, uint64_t address) {
	if (size < sizeof(Elf64_Ehdr)) {
		return NULL;
	}
	const Elf64_Ehdr *header = (const Elf64_Ehdr *)data;
	if (header->e_shentsize != sizeof(Elf64_Shdr) ||
		!range_valid((size_t)header->e_shoff, header->e_shnum,
			sizeof(Elf64_Shdr), size)) {
		return NULL;
	}
	const Elf64_Shdr *sections = (const Elf64_Shdr *)(data + header->e_shoff);
	const char *best = NULL;
	uint64_t best_distance = UINT64_MAX;
	for (Elf64_Half i = 0; i < header->e_shnum; ++i) {
		const Elf64_Shdr *section = &sections[i];
		if ((section->sh_type != SHT_SYMTAB &&
			section->sh_type != SHT_DYNSYM) ||
			section->sh_entsize != sizeof(Elf64_Sym) ||
			section->sh_link >= header->e_shnum ||
			!range_valid((size_t)section->sh_offset,
				(size_t)(section->sh_size / sizeof(Elf64_Sym)),
				sizeof(Elf64_Sym), size)) {
			continue;
		}
		const Elf64_Shdr *strings_section = &sections[section->sh_link];
		if (!range_valid((size_t)strings_section->sh_offset,
			(size_t)strings_section->sh_size, 1, size)) {
			continue;
		}
		const char *strings = (const char *)(data + strings_section->sh_offset);
		const Elf64_Sym *symbols =
			(const Elf64_Sym *)(data + section->sh_offset);
		size_t count = (size_t)(section->sh_size / sizeof(symbols[0]));
		for (size_t j = 0; j < count; ++j) {
			unsigned int type = ELF64_ST_TYPE(symbols[j].st_info);
			if ((type != STT_FUNC && type != STT_GNU_IFUNC) ||
				symbols[j].st_name == 0 || symbols[j].st_shndx == SHN_UNDEF ||
				address < symbols[j].st_value) {
				continue;
			}
			uint64_t distance = address - symbols[j].st_value;
			if ((symbols[j].st_size != 0 && distance >= symbols[j].st_size) ||
				distance > best_distance ||
				!valid_string(strings, (size_t)strings_section->sh_size,
					symbols[j].st_name)) {
				continue;
			}
			best = strings + symbols[j].st_name;
			best_distance = distance;
			if (distance == 0) {
				return best;
			}
		}
	}
	return best;
}

static const char *
find_elf32_symbol(const unsigned char *data, size_t size, uint32_t address) {
	if (size < sizeof(Elf32_Ehdr)) {
		return NULL;
	}
	const Elf32_Ehdr *header = (const Elf32_Ehdr *)data;
	if (header->e_shentsize != sizeof(Elf32_Shdr) ||
		!range_valid((size_t)header->e_shoff, header->e_shnum,
			sizeof(Elf32_Shdr), size)) {
		return NULL;
	}
	const Elf32_Shdr *sections = (const Elf32_Shdr *)(data + header->e_shoff);
	const char *best = NULL;
	uint32_t best_distance = UINT32_MAX;
	for (Elf32_Half i = 0; i < header->e_shnum; ++i) {
		const Elf32_Shdr *section = &sections[i];
		if ((section->sh_type != SHT_SYMTAB &&
			section->sh_type != SHT_DYNSYM) ||
			section->sh_entsize != sizeof(Elf32_Sym) ||
			section->sh_link >= header->e_shnum ||
			!range_valid(section->sh_offset,
				section->sh_size / sizeof(Elf32_Sym), sizeof(Elf32_Sym), size)) {
			continue;
		}
		const Elf32_Shdr *strings_section = &sections[section->sh_link];
		if (!range_valid(strings_section->sh_offset, strings_section->sh_size,
			1, size)) {
			continue;
		}
		const char *strings = (const char *)(data + strings_section->sh_offset);
		const Elf32_Sym *symbols =
			(const Elf32_Sym *)(data + section->sh_offset);
		size_t count = section->sh_size / sizeof(symbols[0]);
		for (size_t j = 0; j < count; ++j) {
			unsigned int type = ELF32_ST_TYPE(symbols[j].st_info);
			if ((type != STT_FUNC && type != STT_GNU_IFUNC) ||
				symbols[j].st_name == 0 || symbols[j].st_shndx == SHN_UNDEF ||
				address < symbols[j].st_value) {
				continue;
			}
			uint32_t distance = address - symbols[j].st_value;
			if ((symbols[j].st_size != 0 && distance >= symbols[j].st_size) ||
				distance > best_distance ||
				!valid_string(strings, strings_section->sh_size,
					symbols[j].st_name)) {
				continue;
			}
			best = strings + symbols[j].st_name;
			best_distance = distance;
			if (distance == 0) {
				return best;
			}
		}
	}
	return best;
}

static void
resolve_elf_name(lp_native_symbol *symbol, uintptr_t relative_address) {
	if (symbol->path_length == 0) {
		return;
	}
	int descriptor = open(symbol->path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		return;
	}
	struct stat status;
	if (fstat(descriptor, &status) != 0 || status.st_size <= 0) {
		close(descriptor);
		return;
	}
	size_t size = (size_t)status.st_size;
	unsigned char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE,
		descriptor, 0);
	close(descriptor);
	if (data == MAP_FAILED) {
		return;
	}
	const char *name = NULL;
	if (size >= EI_NIDENT && memcmp(data, ELFMAG, SELFMAG) == 0) {
		if (data[EI_CLASS] == ELFCLASS64) {
			name = find_elf64_symbol(data, size, (uint64_t)relative_address);
		}
		else if (data[EI_CLASS] == ELFCLASS32 &&
			relative_address <= UINT32_MAX) {
			name = find_elf32_symbol(data, size,
				(uint32_t)relative_address);
		}
	}
	if (name != NULL) {
		copy_text(symbol->name, sizeof(symbol->name), &symbol->name_length,
			name);
	}
	munmap(data, size);
}

bool
lp_native_symbol_resolve(const void *address, lp_native_symbol *symbol) {
	if (symbol == NULL) {
		return false;
	}
	memset(symbol, 0, sizeof(*symbol));
	if (address == NULL) {
		return false;
	}
	lp_mapping_search search = {
		.address = (uintptr_t)address,
		.symbol = symbol,
	};
	(void)dl_iterate_phdr(find_mapping, &search);

	Dl_info info;
	memset(&info, 0, sizeof(info));
	if (dladdr(address, &info) != 0) {
		if (symbol->path_length == 0 && info.dli_fname != NULL) {
			copy_text(symbol->path, sizeof(symbol->path),
				&symbol->path_length, info.dli_fname);
		}
		if (info.dli_sname != NULL) {
			copy_text(symbol->name, sizeof(symbol->name),
				&symbol->name_length, info.dli_sname);
		}
		if (!search.found && info.dli_fbase != NULL) {
			search.load_bias = (uintptr_t)info.dli_fbase;
		}
	}
	if (symbol->name_length == 0) {
		resolve_elf_name(symbol, (uintptr_t)address - search.load_bias);
	}
	return symbol->name_length != 0 || symbol->has_mapping;
}

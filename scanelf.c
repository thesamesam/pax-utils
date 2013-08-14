/*
 * Copyright 2003-2012 Gentoo Foundation
 * Distributed under the terms of the GNU General Public License v2
 * $Header: /var/cvsroot/gentoo-projects/pax-utils/scanelf.c,v 1.259 2013/08/14 21:09:57 vapier Exp $
 *
 * Copyright 2003-2012 Ned Ludd        - <solar@gentoo.org>
 * Copyright 2004-2012 Mike Frysinger  - <vapier@gentoo.org>
 */

static const char rcsid[] = "$Id: scanelf.c,v 1.259 2013/08/14 21:09:57 vapier Exp $";
const char argv0[] = "scanelf";

#include "paxinc.h"

#define IS_MODIFIER(c) (c == '%' || c == '#' || c == '+')

/* prototypes */
static int file_matches_list(const char *filename, char **matchlist);

/* variables to control behavior */
static char *match_etypes = NULL;
static array_t _ldpaths = array_init_decl, *ldpaths = &_ldpaths;
static char scan_ldpath = 0;
static char scan_envpath = 0;
static char scan_symlink = 1;
static char scan_archives = 0;
static char dir_recurse = 0;
static char dir_crossmount = 1;
static char show_pax = 0;
static char show_perms = 0;
static char show_size = 0;
static char show_phdr = 0;
static char show_textrel = 0;
static char show_rpath = 0;
static char show_needed = 0;
static char show_interp = 0;
static char show_bind = 0;
static char show_soname = 0;
static char show_textrels = 0;
static char show_banner = 1;
static char show_endian = 0;
static char show_osabi = 0;
static char show_eabi = 0;
static char be_quiet = 0;
static char be_verbose = 0;
static char be_wewy_wewy_quiet = 0;
static char be_semi_verbose = 0;
static char *find_sym = NULL;
static array_t _find_sym_arr = array_init_decl, *find_sym_arr = &_find_sym_arr;
static array_t _find_sym_regex_arr = array_init_decl, *find_sym_regex_arr = &_find_sym_regex_arr;
static char *find_lib = NULL;
static array_t _find_lib_arr = array_init_decl, *find_lib_arr = &_find_lib_arr;
static char *find_section = NULL;
static array_t _find_section_arr = array_init_decl, *find_section_arr = &_find_section_arr;
static char *out_format = NULL;
static char *search_path = NULL;
static char fix_elf = 0;
static char g_match = 0;
static char use_ldcache = 0;
static char use_ldpath = 0;

static char **qa_textrels = NULL;
static char **qa_execstack = NULL;
static char **qa_wx_load = NULL;
static int root_fd = AT_FDCWD;

static int match_bits = 0;
static unsigned int match_perms = 0;
static void *ldcache = NULL;
static size_t ldcache_size = 0;
static unsigned long setpax = 0UL;

static int has_objdump = 0;

/* find the path to a file by name */
static int bin_in_path(const char *fname)
{
	char fullpath[__PAX_UTILS_PATH_MAX];
	char *path, *p;

	path = getenv("PATH");
	if (!path)
		return 0;

	while ((p = strrchr(path, ':')) != NULL) {
		snprintf(fullpath, sizeof(fullpath), "%s/%s", p + 1, fname);
		*p = 0;
		if (access(fullpath, R_OK) != -1)
			return 1;
	}

	return 0;
}

static FILE *fopenat_r(int dir_fd, const char *path)
{
	int fd = openat(dir_fd, path, O_RDONLY|O_CLOEXEC);
	if (fd == -1)
		return NULL;
	return fdopen(fd, "re");
}

static const char *root_rel_path(const char *path)
{
	/*
	 * openat() will ignore the dirfd if path starts with
	 * a /, so consume all of that noise
	 *
	 * XXX: we don't handle relative paths like ../ that
	 * break out of the --root option, but for now, just
	 * don't do that :P.
	 */
	if (root_fd != AT_FDCWD) {
		while (*path == '/')
			++path;
		if (*path == '\0')
			path = ".";
	}

	return path;
}

/* sub-funcs for scanelf_fileat() */
static void scanelf_file_get_symtabs(elfobj *elf, void **sym, void **str)
{
	/* find the best SHT_DYNSYM and SHT_STRTAB sections */

	/* debug sections */
	void *symtab = elf_findsecbyname(elf, ".symtab");
	void *strtab = elf_findsecbyname(elf, ".strtab");
	/* runtime sections */
	void *dynsym = elf_findsecbyname(elf, ".dynsym");
	void *dynstr = elf_findsecbyname(elf, ".dynstr");

	/*
	 * If the sections are marked NOBITS, then they don't exist, so we just
	 * skip them.  This let's us work sanely with splitdebug ELFs (rather
	 * than spewing a lot of "corrupt ELF" messages later on).  In malformed
	 * ELFs, the section might be wrongly set to NOBITS, but screw em.
	 */
#define GET_SYMTABS(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
	Elf ## B ## _Shdr *esymtab = symtab; \
	Elf ## B ## _Shdr *estrtab = strtab; \
	Elf ## B ## _Shdr *edynsym = dynsym; \
	Elf ## B ## _Shdr *edynstr = dynstr; \
	\
	if (symtab && EGET(esymtab->sh_type) == SHT_NOBITS) \
		symtab = NULL; \
	if (dynsym && EGET(edynsym->sh_type) == SHT_NOBITS) \
		dynsym = NULL; \
	if (symtab && dynsym) \
		*sym = (EGET(esymtab->sh_size) > EGET(edynsym->sh_size)) ? symtab : dynsym; \
	else \
		*sym = symtab ? symtab : dynsym; \
	\
	if (strtab && EGET(estrtab->sh_type) == SHT_NOBITS) \
		strtab = NULL; \
	if (dynstr && EGET(edynstr->sh_type) == SHT_NOBITS) \
		dynstr = NULL; \
	if (strtab && dynstr) \
		*str = (EGET(estrtab->sh_size) > EGET(edynstr->sh_size)) ? strtab : dynstr; \
	else \
		*str = strtab ? strtab : dynstr; \
	}
	GET_SYMTABS(32)
	GET_SYMTABS(64)

	if (*sym && *str)
		return;

	/*
	 * damn, they're really going to make us work for it huh?
	 * reconstruct the section header info out of the dynamic
	 * tags so we can see what symbols this guy uses at runtime.
	 */
#define GET_SYMTABS_DT(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
	size_t i; \
	static Elf ## B ## _Shdr sym_shdr, str_shdr; \
	Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
	Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
	Elf ## B ## _Addr vsym, vstr, vhash, vgnu_hash; \
	Elf ## B ## _Dyn *dyn; \
	Elf ## B ## _Off offset; \
	\
	/* lookup symbols used at runtime with DT_SYMTAB / DT_STRTAB */ \
	vsym = vstr = vhash = vgnu_hash = 0; \
	memset(&sym_shdr, 0, sizeof(sym_shdr)); \
	memset(&str_shdr, 0, sizeof(str_shdr)); \
	for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
		if (EGET(phdr[i].p_type) != PT_DYNAMIC) \
			continue; \
		\
		offset = EGET(phdr[i].p_offset); \
		if (offset >= elf->len - sizeof(Elf ## B ## _Dyn)) \
			continue; \
		\
		dyn = DYN ## B (elf->vdata + offset); \
		while (EGET(dyn->d_tag) != DT_NULL) { \
			switch (EGET(dyn->d_tag)) { \
			case DT_SYMTAB:   vsym = EGET(dyn->d_un.d_val); break; \
			case DT_SYMENT:   sym_shdr.sh_entsize = dyn->d_un.d_val; break; \
			case DT_STRTAB:   vstr = EGET(dyn->d_un.d_val); break; \
			case DT_STRSZ:    str_shdr.sh_size = dyn->d_un.d_val; break; \
			case DT_HASH:     vhash = EGET(dyn->d_un.d_val); break; \
			/*case DT_GNU_HASH: vgnu_hash = EGET(dyn->d_un.d_val); break;*/ \
			} \
			++dyn; \
		} \
		if (vsym && vstr) \
			break; \
	} \
	if (!vsym || !vstr || !(vhash || vgnu_hash)) \
		return; \
	\
	/* calc offset into the ELF by finding the load addr of the syms */ \
	for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
		Elf ## B ## _Addr vaddr = EGET(phdr[i].p_vaddr); \
		Elf ## B ## _Addr filesz = EGET(phdr[i].p_filesz); \
		offset = EGET(phdr[i].p_offset); \
		\
		if (EGET(phdr[i].p_type) != PT_LOAD) \
			continue; \
		\
		if (vhash >= vaddr && vhash < vaddr + filesz) { \
			/* Scan the hash table to see how many entries we have */ \
			Elf32_Word max_sym_idx = 0; \
			Elf32_Word *hashtbl = elf->vdata + offset + (vhash - vaddr); \
			Elf32_Word b, nbuckets = EGET(hashtbl[0]); \
			Elf32_Word nchains = EGET(hashtbl[1]); \
			Elf32_Word *buckets = &hashtbl[2]; \
			Elf32_Word *chains = &buckets[nbuckets]; \
			Elf32_Word sym_idx; \
			\
			for (b = 0; b < nbuckets; ++b) { \
				if (!buckets[b]) \
					continue; \
				for (sym_idx = buckets[b]; sym_idx < nchains && sym_idx; sym_idx = chains[sym_idx]) \
					if (max_sym_idx < sym_idx) \
						max_sym_idx = sym_idx; \
			} \
			ESET(sym_shdr.sh_size, sym_shdr.sh_entsize * max_sym_idx); \
		} \
		\
		if (vsym >= vaddr && vsym < vaddr + filesz) { \
			ESET(sym_shdr.sh_offset, offset + (vsym - vaddr)); \
			*sym = &sym_shdr; \
		} \
		\
		if (vstr >= vaddr && vstr < vaddr + filesz) { \
			ESET(str_shdr.sh_offset, offset + (vstr - vaddr)); \
			*str = &str_shdr; \
		} \
	} \
	}
	GET_SYMTABS_DT(32)
	GET_SYMTABS_DT(64)
}

static char *scanelf_file_pax(elfobj *elf, char *found_pax)
{
	static char ret[7];
	unsigned long i, shown;

	if (!show_pax) return NULL;

	shown = 0;
	memset(&ret, 0, sizeof(ret));

	if (elf->phdr) {
#define SHOW_PAX(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
	Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
	Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
	for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
		if (EGET(phdr[i].p_type) != PT_PAX_FLAGS) \
			continue; \
		if (fix_elf && setpax) { \
			/* set the paxctl flags */ \
			ESET(phdr[i].p_flags, setpax); \
		} \
		if (be_quiet && (EGET(phdr[i].p_flags) == (PF_NOEMUTRAMP | PF_NORANDEXEC))) \
			continue; \
		memcpy(ret, pax_short_pf_flags(EGET(phdr[i].p_flags)), 6); \
		*found_pax = 1; \
		++shown; \
		break; \
	} \
	}
	SHOW_PAX(32)
	SHOW_PAX(64)
	}

	/* Note: We do not support setting EI_PAX if not PT_PAX_FLAGS
	 * was found.  This is known to break ELFs on glibc systems,
	 * and mainline PaX has deprecated use of this for a long time.
	 * We could support changing PT_GNU_STACK, but that doesn't
	 * seem like it's worth the effort. #411919
	 */

	/* fall back to EI_PAX if no PT_PAX was found */
	if (!*ret) {
		static char *paxflags;
		paxflags = pax_short_hf_flags(EI_PAX_FLAGS(elf));
		if (!be_quiet || (be_quiet && EI_PAX_FLAGS(elf))) {
			*found_pax = 1;
			return (be_wewy_wewy_quiet ? NULL : paxflags);
		}
		strncpy(ret, paxflags, sizeof(ret));
	}

	if (be_wewy_wewy_quiet || (be_quiet && !shown))
		return NULL;
	else
		return ret;
}

static char *scanelf_file_phdr(elfobj *elf, char *found_phdr, char *found_relro, char *found_load)
{
	static char ret[12];
	char *found;
	unsigned long i, shown, multi_stack, multi_relro, multi_load;

	if (!show_phdr) return NULL;

	memcpy(ret, "--- --- ---\0", 12);

	shown = 0;
	multi_stack = multi_relro = multi_load = 0;

#define NOTE_GNU_STACK ".note.GNU-stack"
#define SHOW_PHDR(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
	Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
	Elf ## B ## _Off offset; \
	uint32_t flags, check_flags; \
	if (elf->phdr != NULL) { \
		Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
		for (i = 0; i < EGET(ehdr->e_phnum); ++i) { \
			if (EGET(phdr[i].p_type) == PT_GNU_STACK) { \
				if (multi_stack++) \
					warnf("%s: multiple PT_GNU_STACK's !?", elf->filename); \
				if (file_matches_list(elf->filename, qa_execstack)) \
					continue; \
				found = found_phdr; \
				offset = 0; \
				check_flags = PF_X; \
			} else if (EGET(phdr[i].p_type) == PT_GNU_RELRO) { \
				if (multi_relro++) \
					warnf("%s: multiple PT_GNU_RELRO's !?", elf->filename); \
				found = found_relro; \
				offset = 4; \
				check_flags = PF_X; \
			} else if (EGET(phdr[i].p_type) == PT_LOAD) { \
				if (file_matches_list(elf->filename, qa_wx_load)) \
					continue; \
				found = found_load; \
				offset = 8; \
				check_flags = PF_W|PF_X; \
			} else \
				continue; \
			flags = EGET(phdr[i].p_flags); \
			if (be_quiet && ((flags & check_flags) != check_flags)) \
				continue; \
			if ((EGET(phdr[i].p_type) != PT_LOAD) && (fix_elf && ((flags & PF_X) != flags))) { \
				ESET(phdr[i].p_flags, flags & (PF_X ^ (size_t)-1)); \
				ret[3] = ret[7] = '!'; \
				flags = EGET(phdr[i].p_flags); \
			} \
			memcpy(ret+offset, gnu_short_stack_flags(flags), 3); \
			*found = 1; \
			++shown; \
		} \
	} else if (elf->shdr != NULL) { \
		/* no program headers which means this is prob an object file */ \
		Elf ## B ## _Shdr *shdr = SHDR ## B (elf->shdr); \
		Elf ## B ## _Shdr *strtbl = shdr + EGET(ehdr->e_shstrndx); \
		char *str; \
		if ((void*)strtbl > elf->data_end) \
			goto skip_this_shdr##B; \
		/* let's flag -w/+x object files since the final ELF will most likely
		 * need write access to the stack (who doesn't !?).  so the combined
		 * output will bring in +w automatically and that's bad.
		 */
		check_flags = /*SHF_WRITE|*/SHF_EXECINSTR; \
		for (i = 0; i < EGET(ehdr->e_shnum); ++i) { \
			if (EGET(shdr[i].sh_type) != SHT_PROGBITS) continue; \
			offset = EGET(strtbl->sh_offset) + EGET(shdr[i].sh_name); \
			str = elf->data + offset; \
			if (str > elf->data + offset + sizeof(NOTE_GNU_STACK)) continue; \
			if (!strcmp(str, NOTE_GNU_STACK)) { \
				if (multi_stack++) warnf("%s: multiple .note.GNU-stack's !?", elf->filename); \
				flags = EGET(shdr[i].sh_flags); \
				if (be_quiet && ((flags & check_flags) != check_flags)) \
					continue; \
				++*found_phdr; \
				shown = 1; \
				if (flags & SHF_WRITE)     ret[0] = 'W'; \
				if (flags & SHF_ALLOC)     ret[1] = 'A'; \
				if (flags & SHF_EXECINSTR) ret[2] = 'X'; \
				if (flags & 0xFFFFFFF8)    warn("Invalid section flags for GNU-stack"); \
				break; \
			} \
		} \
		skip_this_shdr##B: \
		if (!multi_stack) { \
			if (file_matches_list(elf->filename, qa_execstack)) \
				return NULL; \
			*found_phdr = 1; \
			shown = 1; \
			memcpy(ret, "!WX", 3); \
		} \
	} \
	}
	SHOW_PHDR(32)
	SHOW_PHDR(64)

	if (be_wewy_wewy_quiet || (be_quiet && !shown))
		return NULL;
	else
		return ret;
}

/*
 * See if this ELF contains a DT_TEXTREL tag in any of its
 * PT_DYNAMIC sections.
 */
static const char *scanelf_file_textrel(elfobj *elf, char *found_textrel)
{
	static const char *ret = "TEXTREL";
	unsigned long i;

	if (!show_textrel && !show_textrels) return NULL;

	if (file_matches_list(elf->filename, qa_textrels)) return NULL;

	if (elf->phdr) {
#define SHOW_TEXTREL(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
	Elf ## B ## _Dyn *dyn; \
	Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
	Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
	Elf ## B ## _Off offset; \
	for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
		if (EGET(phdr[i].p_type) != PT_DYNAMIC || EGET(phdr[i].p_filesz) == 0) continue; \
		offset = EGET(phdr[i].p_offset); \
		if (offset >= elf->len - sizeof(Elf ## B ## _Dyn)) continue; \
		dyn = DYN ## B (elf->vdata + offset); \
		while (EGET(dyn->d_tag) != DT_NULL) { \
			if (EGET(dyn->d_tag) == DT_TEXTREL) { /*dyn->d_tag != DT_FLAGS)*/ \
				*found_textrel = 1; \
				/*if (dyn->d_un.d_val & DF_TEXTREL)*/ \
				return (be_wewy_wewy_quiet ? NULL : ret); \
			} \
			++dyn; \
		} \
	} }
	SHOW_TEXTREL(32)
	SHOW_TEXTREL(64)
	}

	if (be_quiet || be_wewy_wewy_quiet)
		return NULL;
	else
		return "   -   ";
}

/*
 * Scan the .text section to see if there are any relocations in it.
 * Should rewrite this to check PT_LOAD sections that are marked
 * Executable rather than the section named '.text'.
 */
static char *scanelf_file_textrels(elfobj *elf, char *found_textrels, char *found_textrel)
{
	unsigned long s, r, rmax;
	void *symtab_void, *strtab_void, *text_void;

	if (!show_textrels) return NULL;

	/* don't search for TEXTREL's if the ELF doesn't have any */
	if (!*found_textrel) scanelf_file_textrel(elf, found_textrel);
	if (!*found_textrel) return NULL;

	scanelf_file_get_symtabs(elf, &symtab_void, &strtab_void);
	text_void = elf_findsecbyname(elf, ".text");

	if (symtab_void && strtab_void && text_void && elf->shdr) {
#define SHOW_TEXTRELS(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
	Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
	Elf ## B ## _Shdr *shdr = SHDR ## B (elf->shdr); \
	Elf ## B ## _Shdr *symtab = SHDR ## B (symtab_void); \
	Elf ## B ## _Shdr *strtab = SHDR ## B (strtab_void); \
	Elf ## B ## _Shdr *text = SHDR ## B (text_void); \
	Elf ## B ## _Addr vaddr = EGET(text->sh_addr); \
	uint ## B ## _t memsz = EGET(text->sh_size); \
	Elf ## B ## _Rel *rel; \
	Elf ## B ## _Rela *rela; \
	/* search the section headers for relocations */ \
	for (s = 0; s < EGET(ehdr->e_shnum); ++s) { \
		uint32_t sh_type = EGET(shdr[s].sh_type); \
		if (sh_type == SHT_REL) { \
			rel = REL ## B (elf->vdata + EGET(shdr[s].sh_offset)); \
			rela = NULL; \
			rmax = EGET(shdr[s].sh_size) / sizeof(*rel); \
		} else if (sh_type == SHT_RELA) { \
			rel = NULL; \
			rela = RELA ## B (elf->vdata + EGET(shdr[s].sh_offset)); \
			rmax = EGET(shdr[s].sh_size) / sizeof(*rela); \
		} else \
			continue; \
		/* now see if any of the relocs are in the .text */ \
		for (r = 0; r < rmax; ++r) { \
			unsigned long sym_max; \
			Elf ## B ## _Addr offset_tmp; \
			Elf ## B ## _Sym *func; \
			Elf ## B ## _Sym *sym; \
			Elf ## B ## _Addr r_offset; \
			uint ## B ## _t r_info; \
			if (sh_type == SHT_REL) { \
				r_offset = EGET(rel[r].r_offset); \
				r_info = EGET(rel[r].r_info); \
			} else { \
				r_offset = EGET(rela[r].r_offset); \
				r_info = EGET(rela[r].r_info); \
			} \
			/* make sure this relocation is inside of the .text */ \
			if (r_offset < vaddr || r_offset >= vaddr + memsz) { \
				if (be_verbose <= 2) continue; \
			} else \
				*found_textrels = 1; \
			/* locate this relocation symbol name */ \
			sym = SYM ## B (elf->vdata + EGET(symtab->sh_offset)); \
			if ((void*)sym > elf->data_end) { \
				warn("%s: corrupt ELF symbol", elf->filename); \
				continue; \
			} \
			sym_max = ELF ## B ## _R_SYM(r_info); \
			if (sym_max * EGET(symtab->sh_entsize) < symtab->sh_size) \
				sym += sym_max; \
			else \
				sym = NULL; \
			sym_max = EGET(symtab->sh_size) / EGET(symtab->sh_entsize); \
			/* show the raw details about this reloc */ \
			printf("  %s: ", elf->base_filename); \
			if (sym && sym->st_name) \
				printf("%s", elf->data + EGET(strtab->sh_offset) + EGET(sym->st_name)); \
			else \
				printf("(memory/data?)"); \
			printf(" [0x%lX]", (unsigned long)r_offset); \
			/* now try to find the closest symbol that this rel is probably in */ \
			sym = SYM ## B (elf->vdata + EGET(symtab->sh_offset)); \
			func = NULL; \
			offset_tmp = 0; \
			while (sym_max--) { \
				if (EGET(sym->st_value) < r_offset && EGET(sym->st_value) > offset_tmp) { \
					func = sym; \
					offset_tmp = EGET(sym->st_value); \
				} \
				++sym; \
			} \
			printf(" in "); \
			if (func && func->st_name) { \
				const char *func_name = elf->data + EGET(strtab->sh_offset) + EGET(func->st_name); \
				if (r_offset > EGET(func->st_size)) \
					printf("(optimized out: previous %s)", func_name); \
				else \
					printf("%s", func_name); \
			} else \
				printf("(optimized out)"); \
			printf(" [0x%lX]\n", (unsigned long)offset_tmp); \
			if (be_verbose && has_objdump) { \
				Elf ## B ## _Addr end_addr = offset_tmp + EGET(func->st_size); \
				char *sysbuf; \
				size_t syslen; \
				const char sysfmt[] = "objdump -r -R -d -w -l --start-address=0x%lX --stop-address=0x%lX %s | grep --color -i -C 3 '.*[[:space:]]%lX:[[:space:]]*R_.*'\n"; \
				syslen = sizeof(sysfmt) + strlen(elf->filename) + 3 * sizeof(unsigned long) + 1; \
				sysbuf = xmalloc(syslen); \
				if (end_addr < r_offset) \
					/* not uncommon when things are optimized out */ \
					end_addr = r_offset + 0x100; \
				snprintf(sysbuf, syslen, sysfmt, \
					(unsigned long)offset_tmp, \
					(unsigned long)end_addr, \
					elf->filename, \
					(unsigned long)r_offset); \
				fflush(stdout); \
				if (system(sysbuf)) {/* don't care */} \
				fflush(stdout); \
				free(sysbuf); \
			} \
		} \
	} }
	SHOW_TEXTRELS(32)
	SHOW_TEXTRELS(64)
	}
	if (!*found_textrels)
		warnf("ELF %s has TEXTREL markings but doesnt appear to have any real TEXTREL's !?", elf->filename);

	return NULL;
}

static void rpath_security_checks(elfobj *elf, char *item, const char *dt_type)
{
	struct stat st;
	switch (*item) {
		case '/': break;
		case '.':
			warnf("Security problem with relative %s '%s' in %s", dt_type, item, elf->filename);
			break;
		case ':':
		case '\0':
			warnf("Security problem NULL %s in %s", dt_type, elf->filename);
			break;
		case '$':
			if (fstat(elf->fd, &st) != -1)
				if ((st.st_mode & S_ISUID) || (st.st_mode & S_ISGID))
					warnf("Security problem with %s='%s' in %s with mode set of %o",
					      dt_type, item, elf->filename, (unsigned int) st.st_mode & 07777);
			break;
		default:
			warnf("Maybe? sec problem with %s='%s' in %s", dt_type, item, elf->filename);
			break;
	}
}
static void scanelf_file_rpath(elfobj *elf, char *found_rpath, char **ret, size_t *ret_len)
{
	unsigned long i;
	char *rpath, *runpath, **r;
	void *strtbl_void;

	if (!show_rpath) return;

	strtbl_void = elf_findsecbyname(elf, ".dynstr");
	rpath = runpath = NULL;

	if (elf->phdr && strtbl_void) {
#define SHOW_RPATH(B) \
		if (elf->elf_class == ELFCLASS ## B) { \
		Elf ## B ## _Dyn *dyn; \
		Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
		Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
		Elf ## B ## _Shdr *strtbl = SHDR ## B (strtbl_void); \
		Elf ## B ## _Off offset; \
		Elf ## B ## _Xword word; \
		/* Scan all the program headers */ \
		for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
			/* Just scan dynamic headers */ \
			if (EGET(phdr[i].p_type) != PT_DYNAMIC || EGET(phdr[i].p_filesz) == 0) continue; \
			offset = EGET(phdr[i].p_offset); \
			if (offset >= elf->len - sizeof(Elf ## B ## _Dyn)) continue; \
			/* Just scan dynamic RPATH/RUNPATH headers */ \
			dyn = DYN ## B (elf->vdata + offset); \
			while ((word=EGET(dyn->d_tag)) != DT_NULL) { \
				if (word == DT_RPATH) { \
					r = &rpath; \
				} else if (word == DT_RUNPATH) { \
					r = &runpath; \
				} else { \
					++dyn; \
					continue; \
				} \
				/* Verify the memory is somewhat sane */ \
				offset = EGET(strtbl->sh_offset) + EGET(dyn->d_un.d_ptr); \
				if (offset < (Elf ## B ## _Off)elf->len) { \
					if (*r) warn("ELF has multiple %s's !?", get_elfdtype(word)); \
					*r = elf->data + offset; \
					/* cache the length in case we need to nuke this section later on */ \
					if (fix_elf) \
						offset = strlen(*r); \
					/* If quiet, don't output paths in ld.so.conf */ \
					if (be_quiet) { \
						size_t len; \
						char *start, *end; \
						/* note that we only 'chop' off leading known paths. */ \
						/* since *r is read-only memory, we can only move the ptr forward. */ \
						start = *r; \
						/* scan each path in : delimited list */ \
						while (start) { \
							rpath_security_checks(elf, start, get_elfdtype(word)); \
							end = strchr(start, ':'); \
							len = (end ? abs(end - start) : strlen(start)); \
							if (use_ldcache) { \
								size_t n; \
								const char *ldpath; \
								array_for_each(ldpaths, n, ldpath) \
									if (!strncmp(ldpath, start, len) && !ldpath[len]) { \
										*r = end; \
										/* corner case ... if RPATH reads "/usr/lib:", we want \
										 * to show ':' rather than '' */ \
										if (end && end[1] != '\0') \
											(*r)++; \
										break; \
									} \
							} \
							if (!*r || !end) \
								break; \
							else \
								start = start + len + 1; \
						} \
					} \
					if (*r) { \
						if (fix_elf > 2 || (fix_elf && **r == '\0')) { \
							/* just nuke it */ \
							nuke_it##B: \
							memset(*r, 0x00, offset); \
							*r = NULL; \
							ESET(dyn->d_tag, DT_DEBUG); \
							ESET(dyn->d_un.d_ptr, 0); \
						} else if (fix_elf) { \
							/* try to clean "bad" paths */ \
							size_t len, tmpdir_len; \
							char *start, *end; \
							const char *tmpdir; \
							start = *r; \
							tmpdir = (getenv("TMPDIR") ? : "."); \
							tmpdir_len = strlen(tmpdir); \
							while (1) { \
								end = strchr(start, ':'); \
								if (start == end) { \
									eat_this_path##B: \
									len = strlen(end); \
									memmove(start, end+1, len); \
									start[len-1] = '\0'; \
									end = start - 1; \
								} else if (tmpdir && !strncmp(start, tmpdir, tmpdir_len)) { \
									if (!end) { \
										if (start == *r) \
											goto nuke_it##B; \
										*--start = '\0'; \
									} else \
										goto eat_this_path##B; \
								} \
								if (!end) \
									break; \
								start = end + 1; \
							} \
							if (**r == '\0') \
								goto nuke_it##B; \
						} \
						if (*r) \
							*found_rpath = 1; \
					} \
				} \
				++dyn; \
			} \
		} }
		SHOW_RPATH(32)
		SHOW_RPATH(64)
	}

	if (be_wewy_wewy_quiet) return;

	if (rpath && runpath) {
		if (!strcmp(rpath, runpath)) {
			xstrcat(ret, runpath, ret_len);
		} else {
			fprintf(stderr, "RPATH [%s] != RUNPATH [%s]\n", rpath, runpath);
			xchrcat(ret, '{', ret_len);
			xstrcat(ret, rpath, ret_len);
			xchrcat(ret, ',', ret_len);
			xstrcat(ret, runpath, ret_len);
			xchrcat(ret, '}', ret_len);
		}
	} else if (rpath || runpath)
		xstrcat(ret, (runpath ? runpath : rpath), ret_len);
	else if (!be_quiet)
		xstrcat(ret, "  -  ", ret_len);
}

/* Defines can be seen in glibc's sysdeps/generic/ldconfig.h */
#define LDSO_CACHE_MAGIC "ld.so-"
#define LDSO_CACHE_MAGIC_LEN (sizeof LDSO_CACHE_MAGIC -1)
#define LDSO_CACHE_VER "1.7.0"
#define LDSO_CACHE_VER_LEN (sizeof LDSO_CACHE_VER -1)
#define FLAG_ANY            -1
#define FLAG_TYPE_MASK      0x00ff
#define FLAG_LIBC4          0x0000
#define FLAG_ELF            0x0001
#define FLAG_ELF_LIBC5      0x0002
#define FLAG_ELF_LIBC6      0x0003
#define FLAG_REQUIRED_MASK  0xff00
#define FLAG_SPARC_LIB64    0x0100
#define FLAG_IA64_LIB64     0x0200
#define FLAG_X8664_LIB64    0x0300
#define FLAG_S390_LIB64     0x0400
#define FLAG_POWERPC_LIB64  0x0500
#define FLAG_MIPS64_LIBN32  0x0600
#define FLAG_MIPS64_LIBN64  0x0700
#define FLAG_X8664_LIBX32   0x0800
#define FLAG_ARM_LIBHF      0x0900
#define FLAG_AARCH64_LIB64  0x0a00

#if defined(__GLIBC__) || defined(__UCLIBC__)

static char *lookup_cache_lib(elfobj *elf, const char *fname)
{
	int fd;
	char *strs;
	static char buf[__PAX_UTILS_PATH_MAX] = "";
	const char *cachefile = root_rel_path("/etc/ld.so.cache");
	struct stat st;

	typedef struct {
		char magic[LDSO_CACHE_MAGIC_LEN];
		char version[LDSO_CACHE_VER_LEN];
		int nlibs;
	} header_t;
	header_t *header;

	typedef struct {
		int flags;
		int sooffset;
		int liboffset;
	} libentry_t;
	libentry_t *libent;

	if (fname == NULL)
		return NULL;

	if (ldcache == NULL) {
		if (fstatat(root_fd, cachefile, &st, 0))
			return NULL;

		fd = openat(root_fd, cachefile, O_RDONLY);
		if (fd == -1)
			return NULL;

		/* cache these values so we only map/unmap the cache file once */
		ldcache_size = st.st_size;
		header = ldcache = mmap(0, ldcache_size, PROT_READ, MAP_SHARED, fd, 0);
		close(fd);

		if (ldcache == MAP_FAILED) {
			ldcache = NULL;
			return NULL;
		}

		if (memcmp(header->magic, LDSO_CACHE_MAGIC, LDSO_CACHE_MAGIC_LEN) ||
		    memcmp(header->version, LDSO_CACHE_VER, LDSO_CACHE_VER_LEN))
		{
			munmap(ldcache, ldcache_size);
			ldcache = NULL;
			return NULL;
		}
	} else
		header = ldcache;

	libent = ldcache + sizeof(header_t);
	strs = (char *) &libent[header->nlibs];

	for (fd = 0; fd < header->nlibs; ++fd) {
		/* This should be more fine grained, but for now we assume that
		 * diff arches will not be cached together, and we ignore the
		 * the different multilib mips cases.
		 */
		if (elf->elf_class == ELFCLASS64 && !(libent[fd].flags & FLAG_REQUIRED_MASK))
			continue;
		if (elf->elf_class == ELFCLASS32 && (libent[fd].flags & FLAG_REQUIRED_MASK))
			continue;

		if (strcmp(fname, strs + libent[fd].sooffset) != 0)
			continue;

		/* Return first hit because that is how the ldso rolls */
		strncpy(buf, strs + libent[fd].liboffset, sizeof(buf));
		break;
	}

	return buf;
}

#elif defined(__NetBSD__)
static char *lookup_cache_lib(elfobj *elf, const char *fname)
{
	static char buf[__PAX_UTILS_PATH_MAX] = "";
	static struct stat st;
	size_t n;
	char *ldpath;

	array_for_each(ldpath, n, ldpath) {
		if ((unsigned) snprintf(buf, sizeof(buf), "%s/%s", ldpath, fname) >= sizeof(buf))
			continue; /* if the pathname is too long, or something went wrong, ignore */

		if (stat(buf, &st) != 0)
			continue; /* if the lib doesn't exist in *ldpath, look further */

		/* NetBSD doesn't actually do sanity checks, it just loads the file
		 * and if that doesn't work, continues looking in other directories.
		 * This cannot easily be safely emulated, unfortunately. For now,
		 * just assume that if it exists, it's a valid library. */

		return buf;
	}

	/* not found in any path */
	return NULL;
}
#else
#ifdef __ELF__
#warning Cache support not implemented for your target
#endif
static char *lookup_cache_lib(elfobj *elf, const char *fname)
{
	return NULL;
}
#endif

static char *lookup_config_lib(const char *fname)
{
	static char buf[__PAX_UTILS_PATH_MAX] = "";
	const char *ldpath;
	size_t n;

	array_for_each(ldpaths, n, ldpath) {
		snprintf(buf, sizeof(buf), "%s/%s", root_rel_path(ldpath), fname);
		if (faccessat(root_fd, buf, F_OK, AT_SYMLINK_NOFOLLOW) == 0)
			return buf;
	}

	return NULL;
}

static const char *scanelf_file_needed_lib(elfobj *elf, char *found_needed, char *found_lib, int op, char **ret, size_t *ret_len)
{
	unsigned long i;
	char *needed;
	void *strtbl_void;
	char *p;

	/*
	 * -n -> op==0 -> print all
	 * -N -> op==1 -> print requested
	 */
	if ((op == 0 && !show_needed) || (op == 1 && !find_lib))
		return NULL;

	strtbl_void = elf_findsecbyname(elf, ".dynstr");

	if (elf->phdr && strtbl_void) {
#define SHOW_NEEDED(B) \
		if (elf->elf_class == ELFCLASS ## B) { \
		Elf ## B ## _Dyn *dyn; \
		Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
		Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
		Elf ## B ## _Shdr *strtbl = SHDR ## B (strtbl_void); \
		Elf ## B ## _Off offset; \
		size_t matched = 0; \
		/* Walk all the program headers to find the PT_DYNAMIC */ \
		for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
			if (EGET(phdr[i].p_type) != PT_DYNAMIC || EGET(phdr[i].p_filesz) == 0) \
				continue; \
			offset = EGET(phdr[i].p_offset); \
			if (offset >= elf->len - sizeof(Elf ## B ## _Dyn)) \
				continue; \
			/* Walk all the dynamic tags to find NEEDED entries */ \
			dyn = DYN ## B (elf->vdata + offset); \
			while (EGET(dyn->d_tag) != DT_NULL) { \
				if (EGET(dyn->d_tag) == DT_NEEDED) { \
					offset = EGET(strtbl->sh_offset) + EGET(dyn->d_un.d_ptr); \
					if (offset >= (Elf ## B ## _Off)elf->len) { \
						++dyn; \
						continue; \
					} \
					needed = elf->data + offset; \
					if (op == 0) { \
						/* -n -> print all entries */ \
						if (!be_wewy_wewy_quiet) { \
							if (*found_needed) xchrcat(ret, ',', ret_len); \
							if (use_ldpath) { \
								if ((p = lookup_config_lib(needed)) != NULL) \
									needed = p; \
							} else if (use_ldcache) { \
								if ((p = lookup_cache_lib(elf, needed)) != NULL) \
									needed = p; \
							} \
							xstrcat(ret, needed, ret_len); \
						} \
						*found_needed = 1; \
					} else { \
						/* -N -> print matching entries */ \
						size_t n; \
						const char *find_lib_name; \
						\
						array_for_each(find_lib_arr, n, find_lib_name) { \
							int invert = 1; \
							if (find_lib_name[0] == '!') \
								invert = 0, ++find_lib_name; \
							if (!strcmp(find_lib_name, needed) == invert) \
								++matched; \
						} \
						\
						if (matched == array_cnt(find_lib_arr)) { \
							*found_lib = 1; \
							return (be_wewy_wewy_quiet ? NULL : find_lib); \
						} \
					} \
				} \
				++dyn; \
			} \
		} }
		SHOW_NEEDED(32)
		SHOW_NEEDED(64)
		if (op == 0 && !*found_needed && be_verbose)
			warn("ELF lacks DT_NEEDED sections: %s", elf->filename);
	}

	return NULL;
}
static char *scanelf_file_interp(elfobj *elf, char *found_interp)
{
	void *strtbl_void;

	if (!show_interp) return NULL;

	strtbl_void = elf_findsecbyname(elf, ".interp");

	if (strtbl_void) {
#define SHOW_INTERP(B) \
		if (elf->elf_class == ELFCLASS ## B) { \
			Elf ## B ## _Shdr *strtbl = SHDR ## B (strtbl_void); \
			*found_interp = 1; \
			return (be_wewy_wewy_quiet ? NULL : elf->data + EGET(strtbl->sh_offset)); \
		}
		SHOW_INTERP(32)
		SHOW_INTERP(64)
	} else {
		/* Walk all the program headers to find the PT_INTERP */
#define SHOW_PT_INTERP(B) \
		if (elf->elf_class == ELFCLASS ## B) { \
		unsigned long i; \
		Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
		Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
		for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
			if (EGET(phdr[i].p_type) != PT_INTERP) \
				continue; \
			*found_interp = 1; \
			return (be_wewy_wewy_quiet ? NULL : elf->data + EGET(phdr[i].p_offset)); \
		} \
		}
		SHOW_PT_INTERP(32)
		SHOW_PT_INTERP(64)
	}

	return NULL;
}
static const char *scanelf_file_bind(elfobj *elf, char *found_bind)
{
	unsigned long i;
	struct stat s;
	bool dynamic = false;

	if (!show_bind) return NULL;
	if (!elf->phdr) return NULL;

#define SHOW_BIND(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
		Elf ## B ## _Dyn *dyn; \
		Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
		Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
		Elf ## B ## _Off offset; \
		for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
			if (EGET(phdr[i].p_type) != PT_DYNAMIC || EGET(phdr[i].p_filesz) == 0) continue; \
			dynamic = true; \
			offset = EGET(phdr[i].p_offset); \
			if (offset >= elf->len - sizeof(Elf ## B ## _Dyn)) continue; \
			dyn = DYN ## B (elf->vdata + offset); \
			while (EGET(dyn->d_tag) != DT_NULL) { \
				if (EGET(dyn->d_tag) == DT_BIND_NOW || \
				   (EGET(dyn->d_tag) == DT_FLAGS && EGET(dyn->d_un.d_val) & DF_BIND_NOW)) \
				{ \
					if (be_quiet) return NULL; \
					*found_bind = 1; \
					return (char *)(be_wewy_wewy_quiet ? NULL : "NOW"); \
				} \
				++dyn; \
			} \
		} \
	}
	SHOW_BIND(32)
	SHOW_BIND(64)

	if (be_wewy_wewy_quiet) return NULL;

	/* don't output anything if quiet mode and the ELF is static or not setuid */
	if (be_quiet && (!dynamic || (!fstat(elf->fd, &s) && !(s.st_mode & (S_ISUID|S_ISGID))))) {
		return NULL;
	} else {
		*found_bind = 1;
		return dynamic ? "LAZY" : "STATIC";
	}
}
static char *scanelf_file_soname(elfobj *elf, char *found_soname)
{
	unsigned long i;
	char *soname;
	void *strtbl_void;

	if (!show_soname) return NULL;

	strtbl_void = elf_findsecbyname(elf, ".dynstr");

	if (elf->phdr && strtbl_void) {
#define SHOW_SONAME(B) \
		if (elf->elf_class == ELFCLASS ## B) { \
		Elf ## B ## _Dyn *dyn; \
		Elf ## B ## _Ehdr *ehdr = EHDR ## B (elf->ehdr); \
		Elf ## B ## _Phdr *phdr = PHDR ## B (elf->phdr); \
		Elf ## B ## _Shdr *strtbl = SHDR ## B (strtbl_void); \
		Elf ## B ## _Off offset; \
		/* only look for soname in shared objects */ \
		if (EGET(ehdr->e_type) != ET_DYN) \
			return NULL; \
		for (i = 0; i < EGET(ehdr->e_phnum); i++) { \
			if (EGET(phdr[i].p_type) != PT_DYNAMIC || EGET(phdr[i].p_filesz) == 0) continue; \
			offset = EGET(phdr[i].p_offset); \
			if (offset >= elf->len - sizeof(Elf ## B ## _Dyn)) continue; \
			dyn = DYN ## B (elf->vdata + offset); \
			while (EGET(dyn->d_tag) != DT_NULL) { \
				if (EGET(dyn->d_tag) == DT_SONAME) { \
					offset = EGET(strtbl->sh_offset) + EGET(dyn->d_un.d_ptr); \
					if (offset >= (Elf ## B ## _Off)elf->len) { \
						++dyn; \
						continue; \
					} \
					soname = elf->data + offset; \
					*found_soname = 1; \
					return (be_wewy_wewy_quiet ? NULL : soname); \
				} \
				++dyn; \
			} \
		} }
		SHOW_SONAME(32)
		SHOW_SONAME(64)
	}

	return NULL;
}

/*
 * We support the symbol form:
 *    [%[modifiers]%][[+-]<symbol name>][,[.....]]
 * If the symbol name is empty, then all symbols are matched.
 * If the symbol name is a glob ("*"), then all symbols are dumped (debug).
 *    Do not rely on this output format at all.
 * Otherwise the symbol name is used to search (either regex or string compare).
 * If the first char of the symbol name is a plus ("+"), then only match
 *    defined symbols.  If it's a minus ("-"), only match undefined symbols.
 * Putting modifiers in between the percent signs allows for more in depth
 *    filters.  There are groups of modifiers.  If you don't specify a member
 *    of a group, then all types in that group are matched.  The current
 *    groups and their types are:
 *        STT group: STT_NOTYPE:n STT_OBJECT:o STT_FUNC:f STT_FILE:F
 *        STB group: STB_LOCAL:l STB_GLOBAL:g STB_WEAK:w
 *        SHN group: SHN_UNDEF:u SHN_ABS:a SHN_COMMON:c {defined}:d
 *    The "defined" value in the SHN group does not correspond to a SHN_xxx define.
 * You can search for multiple symbols at once by seperating with a comma (",").
 *
 * Some examples:
 *    ELFs with a weak function "foo":
 *        scanelf -s %wf%foo <ELFs>
 *    ELFs that define the symbol "main":
 *        scanelf -s +main <ELFs>
 *        scanelf -s %d%main <ELFs>
 *    ELFs that refer to the undefined symbol "brk":
 *        scanelf -s -brk <ELFs>
 *        scanelf -s %u%brk <ELFs>
 *    All global defined objects in an ELF:
 *        scanelf -s %ogd% <ELF>
 */
static void
scanelf_match_symname(elfobj *elf, char *found_sym, char **ret, size_t *ret_len, const char *symname,
	unsigned int stt, unsigned int stb, unsigned int shn, unsigned long size)
{
	const char *this_sym;
	size_t n;

	array_for_each(find_sym_arr, n, this_sym) {
		bool inc_notype, inc_object, inc_func, inc_file,
		     inc_local, inc_global, inc_weak,
		     inc_def, inc_undef, inc_abs, inc_common;

		/* symbol selection! */
		inc_notype = inc_object = inc_func = inc_file = \
		inc_local = inc_global = inc_weak = \
		inc_def = inc_undef = inc_abs = inc_common = \
			(*this_sym != '%');

		/* parse the contents of %...% */
		if (!inc_notype) {
			while (*(this_sym++)) {
				if (*this_sym == '%') {
					++this_sym;
					break;
				}
				switch (*this_sym) {
					case 'n': inc_notype = true; break;
					case 'o': inc_object = true; break;
					case 'f': inc_func   = true; break;
					case 'F': inc_file   = true; break;
					case 'l': inc_local  = true; break;
					case 'g': inc_global = true; break;
					case 'w': inc_weak   = true; break;
					case 'd': inc_def    = true; break;
					case 'u': inc_undef  = true; break;
					case 'a': inc_abs    = true; break;
					case 'c': inc_common = true; break;
					default:  err("invalid symbol selector '%c'", *this_sym);
				}
			}

			/* If no types are matched, not match all */
			if (!inc_notype && !inc_object && !inc_func && !inc_file)
				inc_notype = inc_object = inc_func = inc_file = true;
			if (!inc_local && !inc_global && !inc_weak)
				inc_local = inc_global = inc_weak = true;
			if (!inc_def && !inc_undef && !inc_abs && !inc_common)
				inc_def = inc_undef = inc_abs = inc_common = true;

		/* backwards compat for defined/undefined short hand */
		} else if (*this_sym == '+') {
			inc_undef = false;
			++this_sym;
		} else if (*this_sym == '-') {
			inc_def = inc_abs = inc_common = false;
			++this_sym;
		}

		/* filter symbols */
		if ((!inc_notype && stt == STT_NOTYPE) || \
		    (!inc_object && stt == STT_OBJECT) || \
		    (!inc_func   && stt == STT_FUNC  ) || \
		    (!inc_file   && stt == STT_FILE  ) || \
		    (!inc_local  && stb == STB_LOCAL ) || \
		    (!inc_global && stb == STB_GLOBAL) || \
		    (!inc_weak   && stb == STB_WEAK  ) || \
		    (!inc_def    && shn && shn < SHN_LORESERVE) || \
		    (!inc_undef  && shn == SHN_UNDEF ) || \
		    (!inc_abs    && shn == SHN_ABS   ) || \
		    (!inc_common && shn == SHN_COMMON))
			continue;

		if (*this_sym == '*') {
			/* a "*" symbol gets you debug output */
			printf("%s(%s) %5lX %15s %15s %15s %s\n",
			       ((*found_sym == 0) ? "\n\t" : "\t"),
			       elf->base_filename,
			       size,
			       get_elfstttype(stt),
			       get_elfstbtype(stb),
			       get_elfshntype(shn),
			       symname);
			goto matched;

		} else {
			if (g_match) {
				/* regex match the symbol */
				if (regexec(find_sym_regex_arr->eles[n], symname, 0, NULL, 0) == REG_NOMATCH)
					continue;

			} else if (*this_sym) {
				/* give empty symbols a "pass", else do a normal compare */
				const size_t len = strlen(this_sym);
				if (!(strncmp(this_sym, symname, len) == 0 &&
				      /* Accept unversioned symbol names */
				      (symname[len] == '\0' || symname[len] == '@')))
					continue;
			}

			if (be_semi_verbose) {
				char buf[1024];
				snprintf(buf, sizeof(buf), "%lX %s %s",
					size,
					get_elfstttype(stt),
					this_sym);
				*ret = xstrdup(buf);
			} else {
				if (*ret) xchrcat(ret, ',', ret_len);
				xstrcat(ret, symname, ret_len);
			}

			goto matched;
		}
	}

	return;

 matched:
	*found_sym = 1;
}

static const char *scanelf_file_sym(elfobj *elf, char *found_sym)
{
	char *ret;
	void *symtab_void, *strtab_void;

	if (!find_sym) return NULL;
	ret = NULL;

	scanelf_file_get_symtabs(elf, &symtab_void, &strtab_void);

	if (symtab_void && strtab_void) {
#define FIND_SYM(B) \
		if (elf->elf_class == ELFCLASS ## B) { \
		Elf ## B ## _Shdr *symtab = SHDR ## B (symtab_void); \
		Elf ## B ## _Shdr *strtab = SHDR ## B (strtab_void); \
		Elf ## B ## _Sym *sym = SYM ## B (elf->vdata + EGET(symtab->sh_offset)); \
		Elf ## B ## _Word i, cnt = EGET(symtab->sh_entsize); \
		char *symname; \
		size_t ret_len = 0; \
		if (cnt) \
			cnt = EGET(symtab->sh_size) / cnt; \
		for (i = 0; i < cnt; ++i) { \
			if ((void*)sym > elf->data_end) { \
				warnf("%s: corrupt ELF symbols - aborting", elf->filename); \
				goto break_out;	\
			} \
			if (sym->st_name) { \
				/* make sure the symbol name is in acceptable memory range */ \
				symname = elf->data + EGET(strtab->sh_offset) + EGET(sym->st_name); \
				if ((void*)symname > elf->data_end) { \
					warnf("%s: corrupt ELF symbols", elf->filename); \
					++sym; \
					continue; \
				} \
				scanelf_match_symname(elf, found_sym, \
			                          &ret, &ret_len, symname, \
			                          ELF##B##_ST_TYPE(EGET(sym->st_info)), \
			                          ELF##B##_ST_BIND(EGET(sym->st_info)), \
			                          EGET(sym->st_shndx), \
			    /* st_size can be 64bit, but no one is really that big, so screw em */ \
			                          EGET(sym->st_size)); \
			} \
			++sym; \
		} \
		}
		FIND_SYM(32)
		FIND_SYM(64)
	}

break_out:
	if (be_wewy_wewy_quiet) return NULL;

	if (*find_sym != '*' && *found_sym)
		return ret;
	if (be_quiet)
		return NULL;
	else
		return " - ";
}

static const char *scanelf_file_sections(elfobj *elf, char *found_section)
{
	if (!find_section)
		 return NULL;

#define FIND_SECTION(B) \
	if (elf->elf_class == ELFCLASS ## B) { \
		size_t matched, n; \
		int invert; \
		const char *section_name; \
		Elf ## B ## _Shdr *section; \
		\
		matched = 0; \
		array_for_each(find_section_arr, n, section_name) { \
			invert = (*section_name == '!' ? 1 : 0); \
			section = SHDR ## B (elf_findsecbyname(elf, section_name + invert)); \
			if ((section == NULL && invert) || (section != NULL && !invert)) \
				++matched; \
		} \
		\
		if (matched == array_cnt(find_section_arr)) \
			*found_section = 1; \
	}
	FIND_SECTION(32)
	FIND_SECTION(64)

	if (be_wewy_wewy_quiet)
		return NULL;

	if (*found_section)
		return find_section;

	if (be_quiet)
		return NULL;
	else
		return " - ";
}

/* scan an elf file and show all the fun stuff */
#define prints(str) ({ ssize_t ret = write(fileno(stdout), str, strlen(str)); ret; })
static int scanelf_elfobj(elfobj *elf)
{
	unsigned long i;
	char found_pax, found_phdr, found_relro, found_load, found_textrel,
	     found_rpath, found_needed, found_interp, found_bind, found_soname,
	     found_sym, found_lib, found_file, found_textrels, found_section;
	static char *out_buffer = NULL;
	static size_t out_len;

	found_pax = found_phdr = found_relro = found_load = found_textrel = \
	found_rpath = found_needed = found_interp = found_bind = found_soname = \
	found_sym = found_lib = found_file = found_textrels = found_section = 0;

	if (be_verbose > 2)
		printf("%s: scanning file {%s,%s}\n", elf->filename,
		       get_elfeitype(EI_CLASS, elf->elf_class),
		       get_elfeitype(EI_DATA, elf->data[EI_DATA]));
	else if (be_verbose > 1)
		printf("%s: scanning file\n", elf->filename);

	/* init output buffer */
	if (!out_buffer) {
		out_len = sizeof(char) * 80;
		out_buffer = xmalloc(out_len);
	}
	*out_buffer = '\0';

	/* show the header */
	if (!be_quiet && show_banner) {
		for (i = 0; out_format[i]; ++i) {
			if (!IS_MODIFIER(out_format[i])) continue;

			switch (out_format[++i]) {
			case '+': break;
			case '%': break;
			case '#': break;
			case 'F':
			case 'p':
			case 'f': prints("FILE "); found_file = 1; break;
			case 'o': prints(" TYPE   "); break;
			case 'x': prints(" PAX   "); break;
			case 'e': prints("STK/REL/PTL "); break;
			case 't': prints("TEXTREL "); break;
			case 'r': prints("RPATH "); break;
			case 'M': prints("CLASS "); break;
			case 'l':
			case 'n': prints("NEEDED "); break;
			case 'i': prints("INTERP "); break;
			case 'b': prints("BIND "); break;
			case 'Z': prints("SIZE "); break;
			case 'S': prints("SONAME "); break;
			case 's': prints("SYM "); break;
			case 'N': prints("LIB "); break;
			case 'T': prints("TEXTRELS "); break;
			case 'k': prints("SECTION "); break;
			case 'a': prints("ARCH "); break;
			case 'I': prints("OSABI "); break;
			case 'Y': prints("EABI "); break;
			case 'O': prints("PERM "); break;
			case 'D': prints("ENDIAN "); break;
			default: warnf("'%c' has no title ?", out_format[i]);
			}
		}
		if (!found_file) prints("FILE ");
		prints("\n");
		found_file = 0;
		show_banner = 0;
	}

	/* dump all the good stuff */
	for (i = 0; out_format[i]; ++i) {
		const char *out;
		const char *tmp;
		static char ubuf[sizeof(unsigned long)*2];
		if (!IS_MODIFIER(out_format[i])) {
			xchrcat(&out_buffer, out_format[i], &out_len);
			continue;
		}

		out = NULL;
		be_wewy_wewy_quiet = (out_format[i] == '#');
		be_semi_verbose = (out_format[i] == '+');
		switch (out_format[++i]) {
		case '+':
		case '%':
		case '#':
			xchrcat(&out_buffer, out_format[i], &out_len); break;
		case 'F':
			found_file = 1;
			if (be_wewy_wewy_quiet) break;
			xstrcat(&out_buffer, elf->filename, &out_len);
			break;
		case 'p':
			found_file = 1;
			if (be_wewy_wewy_quiet) break;
			tmp = elf->filename;
			if (search_path) {
				ssize_t len_search = strlen(search_path);
				ssize_t len_file = strlen(elf->filename);
				if (!strncmp(elf->filename, search_path, len_search) && \
				    len_file > len_search)
					tmp += len_search;
				if (*tmp == '/' && search_path[len_search-1] == '/') tmp++;
			}
			xstrcat(&out_buffer, tmp, &out_len);
			break;
		case 'f':
			found_file = 1;
			if (be_wewy_wewy_quiet) break;
			tmp = strrchr(elf->filename, '/');
			tmp = (tmp == NULL ? elf->filename : tmp+1);
			xstrcat(&out_buffer, tmp, &out_len);
			break;
		case 'o': out = get_elfetype(elf); break;
		case 'x': out = scanelf_file_pax(elf, &found_pax); break;
		case 'e': out = scanelf_file_phdr(elf, &found_phdr, &found_relro, &found_load); break;
		case 't': out = scanelf_file_textrel(elf, &found_textrel); break;
		case 'T': out = scanelf_file_textrels(elf, &found_textrels, &found_textrel); break;
		case 'r': scanelf_file_rpath(elf, &found_rpath, &out_buffer, &out_len); break;
		case 'M': out = get_elfeitype(EI_CLASS, elf->data[EI_CLASS]); break;
		case 'D': out = get_endian(elf); break;
		case 'O': out = strfileperms(elf->filename); break;
		case 'n':
		case 'N': out = scanelf_file_needed_lib(elf, &found_needed, &found_lib, (out_format[i]=='N'), &out_buffer, &out_len); break;
		case 'i': out = scanelf_file_interp(elf, &found_interp); break;
		case 'b': out = scanelf_file_bind(elf, &found_bind); break;
		case 'S': out = scanelf_file_soname(elf, &found_soname); break;
		case 's': out = scanelf_file_sym(elf, &found_sym); break;
		case 'k': out = scanelf_file_sections(elf, &found_section); break;
		case 'a': out = get_elfemtype(elf); break;
		case 'I': out = get_elfosabi(elf); break;
		case 'Y': out = get_elf_eabi(elf); break;
		case 'Z': snprintf(ubuf, sizeof(ubuf), "%lu", (unsigned long)elf->len); out = ubuf; break;;
		default: warnf("'%c' has no scan code?", out_format[i]);
		}
		if (out)
			xstrcat(&out_buffer, out, &out_len);
	}

#define FOUND_SOMETHING() \
	(found_pax || found_phdr || found_relro || found_load || found_textrel || \
	 found_rpath || found_needed || found_interp || found_bind || \
	 found_soname || found_sym || found_lib || found_textrels || found_section )

	if (!found_file && (!be_quiet || (be_quiet && FOUND_SOMETHING()))) {
		xchrcat(&out_buffer, ' ', &out_len);
		xstrcat(&out_buffer, elf->filename, &out_len);
	}
	if (!be_quiet || (be_quiet && FOUND_SOMETHING())) {
		puts(out_buffer);
		fflush(stdout);
	}

	return 0;
}

/* scan a single elf */
static int scanelf_elf(const char *filename, int fd, size_t len)
{
	int ret = 1;
	elfobj *elf;

	/* verify this is real ELF */
	if ((elf = _readelf_fd(filename, fd, len, !fix_elf)) == NULL) {
		if (be_verbose > 2) printf("%s: not an ELF\n", filename);
		return 2;
	}
	switch (match_bits) {
		case 32:
			if (elf->elf_class != ELFCLASS32)
				goto label_done;
			break;
		case 64:
			if (elf->elf_class != ELFCLASS64)
				goto label_done;
			break;
		default: break;
	}
	if (match_etypes) {
		char sbuf[126];
		strncpy(sbuf, match_etypes, sizeof(sbuf));
		if (strchr(match_etypes, ',') != NULL) {
			char *p;
			while ((p = strrchr(sbuf, ',')) != NULL) {
				*p = 0;
				if (etype_lookup(p+1) == get_etype(elf))
					goto label_ret;
			}
		}
		if (etype_lookup(sbuf) != get_etype(elf))
			goto label_done;
	}

label_ret:
	ret = scanelf_elfobj(elf);

label_done:
	unreadelf(elf);
	return ret;
}

/* scan an archive of elfs */
static int scanelf_archive(const char *filename, int fd, size_t len)
{
	archive_handle *ar;
	archive_member *m;
	char *ar_buffer;
	elfobj *elf;

	ar = ar_open_fd(filename, fd);
	if (ar == NULL)
		return 1;

	ar_buffer = mmap(0, len, PROT_READ | (fix_elf ? PROT_WRITE : 0), (fix_elf ? MAP_SHARED : MAP_PRIVATE), fd, 0);
	while ((m = ar_next(ar)) != NULL) {
		off_t cur_pos = lseek(fd, 0, SEEK_CUR);
		if (cur_pos == -1)
			errp("lseek() failed");
		elf = readelf_buffer(m->name, ar_buffer + cur_pos, m->size);
		if (elf) {
			scanelf_elfobj(elf);
			unreadelf(elf);
		}
	}
	munmap(ar_buffer, len);

	return 0;
}
/* scan a file which may be an elf or an archive or some other magical beast */
static int scanelf_fileat(int dir_fd, const char *filename, const struct stat *st_cache)
{
	const struct stat *st = st_cache;
	struct stat symlink_st;
	int fd;

	/* always handle regular files and handle symlinked files if no -y */
	if (S_ISLNK(st->st_mode)) {
		if (!scan_symlink)
			return 1;
		fstatat(dir_fd, filename, &symlink_st, 0);
		st = &symlink_st;
	}

	if (!S_ISREG(st->st_mode)) {
		if (be_verbose > 2) printf("%s: skipping non-file\n", filename);
		return 1;
	}

	if (match_perms) {
		if ((st->st_mode | match_perms) != st->st_mode)
			return 1;
	}
	fd = openat(dir_fd, filename, (fix_elf ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (fd == -1) {
		if (fix_elf && errno == ETXTBSY)
			warnp("%s: could not fix", filename);
		else if (be_verbose > 2)
			printf("%s: skipping file: %s\n", filename, strerror(errno));
		return 1;
	}

	if (scanelf_elf(filename, fd, st->st_size) == 2) {
		/* if it isn't an ELF, maybe it's an .a archive */
		if (scan_archives)
			scanelf_archive(filename, fd, st->st_size);

		/*
		 * unreadelf() implicitly closes its fd, so only close it
		 * when we are returning it in the non-ELF case
		 */
		close(fd);
	}

	return 0;
}

/* scan a directory for ET_EXEC files and print when we find one */
static int scanelf_dirat(int dir_fd, const char *path)
{
	register DIR *dir;
	register struct dirent *dentry;
	struct stat st_top, st;
	char buf[__PAX_UTILS_PATH_MAX], *subpath;
	size_t pathlen = 0, len = 0;
	int ret = 0;
	int subdir_fd;

	/* make sure path exists */
	if (fstatat(dir_fd, path, &st_top, AT_SYMLINK_NOFOLLOW) == -1) {
		if (be_verbose > 2) printf("%s: does not exist\n", path);
		return 1;
	}

	/* ok, if it isn't a directory, assume we can open it */
	if (!S_ISDIR(st_top.st_mode))
		return scanelf_fileat(dir_fd, path, &st_top);

	/* now scan the dir looking for fun stuff */
	subdir_fd = openat(dir_fd, path, O_RDONLY|O_CLOEXEC);
	if (subdir_fd == -1)
		dir = NULL;
	else
		dir = fdopendir(subdir_fd);
	if (dir == NULL) {
		if (subdir_fd != -1)
			close(subdir_fd);
		else if (be_verbose > 2)
			printf("%s: skipping dir: %s\n", path, strerror(errno));
		return 1;
	}
	if (be_verbose > 1) printf("%s: scanning dir\n", path);

	subpath = stpcpy(buf, path);
	if (subpath[-1] != '/')
		*subpath++ = '/';
	pathlen = subpath - buf;
	while ((dentry = readdir(dir))) {
		if (!strcmp(dentry->d_name, ".") || !strcmp(dentry->d_name, ".."))
			continue;

		if (fstatat(subdir_fd, dentry->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1)
			continue;

		len = strlen(dentry->d_name);
		if (len + pathlen + 1 >= sizeof(buf)) {
			warnf("Skipping '%s%s': len > sizeof(buf); %zu > %zu\n",
			      path, dentry->d_name, len + pathlen + 1, sizeof(buf));
			continue;
		}
		memcpy(subpath, dentry->d_name, len);
		subpath[len] = '\0';

		if (S_ISREG(st.st_mode))
			ret = scanelf_fileat(dir_fd, buf, &st);
		else if (dir_recurse && S_ISDIR(st.st_mode)) {
			if (dir_crossmount || (st_top.st_dev == st.st_dev))
				ret = scanelf_dirat(dir_fd, buf);
		}
	}
	closedir(dir);

	return ret;
}
static int scanelf_dir(const char *path)
{
	return scanelf_dirat(root_fd, root_rel_path(path));
}

static int scanelf_from_file(const char *filename)
{
	FILE *fp;
	char *p, *path;
	size_t len;
	int ret;

	if (strcmp(filename, "-") == 0)
		fp = stdin;
	else if ((fp = fopen(filename, "r")) == NULL)
		return 1;

	path = NULL;
	len = 0;
	ret = 0;
	while (getline(&path, &len, fp) != -1) {
		if ((p = strchr(path, '\n')) != NULL)
			*p = 0;
		search_path = path;
		ret = scanelf_dir(path);
	}
	free(path);

	if (fp != stdin)
		fclose(fp);

	return ret;
}

#if defined(__GLIBC__) || defined(__UCLIBC__) || defined(__NetBSD__)

static int _load_ld_cache_config(const char *fname)
{
	FILE *fp = NULL;
	char *p, *path;
	size_t len;
	int curr_fd = -1;

	fp = fopenat_r(root_fd, root_rel_path(fname));
	if (fp == NULL)
		return -1;

	path = NULL;
	len = 0;
	while (getline(&path, &len, fp) != -1) {
		if ((p = strrchr(path, '\r')) != NULL)
			*p = 0;
		if ((p = strchr(path, '\n')) != NULL)
			*p = 0;

		/* recursive includes of the same file will make this segfault. */
		if ((memcmp(path, "include", 7) == 0) && isblank(path[7])) {
			glob_t gl;
			size_t x;
			const char *gpath;

			/* re-use existing path buffer ... need to be creative */
			if (path[8] != '/')
				gpath = memcpy(path + 3, "/etc/", 5);
			else
				gpath = path + 8;
			if (root_fd != AT_FDCWD) {
				if (curr_fd == -1) {
					curr_fd = open(".", O_RDONLY|O_CLOEXEC);
					if (fchdir(root_fd))
						errp("unable to change to root dir");
				}
				gpath = root_rel_path(gpath);
			}

			if (glob(gpath, 0, NULL, &gl) == 0) {
				for (x = 0; x < gl.gl_pathc; ++x) {
					/* try to avoid direct loops */
					if (strcmp(gl.gl_pathv[x], fname) == 0)
						continue;
					_load_ld_cache_config(gl.gl_pathv[x]);
				}
				globfree(&gl);
			}

			/* failed globs are ignored by glibc */
			continue;
		}

		if (*path != '/')
			continue;

		xarraypush_str(ldpaths, path);
	}
	free(path);

	fclose(fp);

	if (curr_fd != -1) {
		if (fchdir(curr_fd))
			{/* don't care */}
		close(curr_fd);
	}

	return 0;
}

#elif defined(__FreeBSD__) || defined(__DragonFly__)

static int _load_ld_cache_config(const char *fname)
{
	FILE *fp = NULL;
	char *b = NULL, *p;
	struct elfhints_hdr hdr;

	fp = fopenat_r(root_fd, root_rel_path(fname));
	if (fp == NULL)
		return -1;

	if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
	    hdr.magic != ELFHINTS_MAGIC || hdr.version != 1 ||
	    fseek(fp, hdr.strtab + hdr.dirlist, SEEK_SET) == -1)
	{
		fclose(fp);
		return -1;
	}

	b = xmalloc(hdr.dirlistlen + 1);
	if (fread(b, 1, hdr.dirlistlen+1, fp) != hdr.dirlistlen+1) {
		fclose(fp);
		free(b);
		return -1;
	}

	while ((p = strsep(&b, ":"))) {
		if (*p == '\0')
			continue;
		xarraypush_str(ldpaths, p);
	}

	free(b);
	fclose(fp);
	return 0;
}

#else
#ifdef __ELF__
#warning Cache config support not implemented for your target
#endif
static int _load_ld_cache_config(const char *fname)
{
	return 0;
}
#endif

static void load_ld_cache_config(const char *fname)
{
	bool scan_l, scan_ul, scan_ull;
	size_t n;
	const char *ldpath;

	_load_ld_cache_config(fname);

	scan_l = scan_ul = scan_ull = false;
	if (array_cnt(ldpaths)) {
		array_for_each(ldpaths, n, ldpath) {
			if (!scan_l   && !strcmp(ldpath, "/lib"))           scan_l   = true;
			if (!scan_ul  && !strcmp(ldpath, "/usr/lib"))       scan_ul  = true;
			if (!scan_ull && !strcmp(ldpath, "/usr/local/lib")) scan_ull = true;
		}
	}

	if (!scan_l)   xarraypush_str(ldpaths, "/lib");
	if (!scan_ul)  xarraypush_str(ldpaths, "/usr/lib");
	if (!scan_ull) xarraypush_str(ldpaths, "/usr/local/lib");
}

/* scan /etc/ld.so.conf for paths */
static void scanelf_ldpath(void)
{
	size_t n;
	const char *ldpath;

	array_for_each(ldpaths, n, ldpath)
		scanelf_dir(ldpath);
}

/* scan env PATH for paths */
static void scanelf_envpath(void)
{
	char *path, *p;

	path = getenv("PATH");
	if (!path)
		err("PATH is not set in your env !");
	path = xstrdup(path);

	while ((p = strrchr(path, ':')) != NULL) {
		scanelf_dir(p + 1);
		*p = 0;
	}

	free(path);
}

/* usage / invocation handling functions */ /* Free Flags: c d j u w G H J K P Q U W */
#define PARSE_FLAGS "plRmyAXz:xetrnLibSs:k:gN:TaqvF:f:o:E:M:DIYO:ZCBhV"
#define a_argument required_argument
static struct option const long_opts[] = {
	{"path",      no_argument, NULL, 'p'},
	{"ldpath",    no_argument, NULL, 'l'},
	{"use-ldpath",no_argument, NULL, 129},
	{"root",       a_argument, NULL, 128},
	{"recursive", no_argument, NULL, 'R'},
	{"mount",     no_argument, NULL, 'm'},
	{"symlink",   no_argument, NULL, 'y'},
	{"archives",  no_argument, NULL, 'A'},
	{"ldcache",   no_argument, NULL, 'L'},
	{"fix",       no_argument, NULL, 'X'},
	{"setpax",     a_argument, NULL, 'z'},
	{"pax",       no_argument, NULL, 'x'},
	{"header",    no_argument, NULL, 'e'},
	{"textrel",   no_argument, NULL, 't'},
	{"rpath",     no_argument, NULL, 'r'},
	{"needed",    no_argument, NULL, 'n'},
	{"interp",    no_argument, NULL, 'i'},
	{"bind",      no_argument, NULL, 'b'},
	{"soname",    no_argument, NULL, 'S'},
	{"symbol",     a_argument, NULL, 's'},
	{"section",    a_argument, NULL, 'k'},
	{"lib",        a_argument, NULL, 'N'},
	{"gmatch",    no_argument, NULL, 'g'},
	{"textrels",  no_argument, NULL, 'T'},
	{"etype",      a_argument, NULL, 'E'},
	{"bits",       a_argument, NULL, 'M'},
	{"endian",    no_argument, NULL, 'D'},
	{"osabi",     no_argument, NULL, 'I'},
	{"eabi",      no_argument, NULL, 'Y'},
	{"perms",      a_argument, NULL, 'O'},
	{"size",      no_argument, NULL, 'Z'},
	{"all",       no_argument, NULL, 'a'},
	{"quiet",     no_argument, NULL, 'q'},
	{"verbose",   no_argument, NULL, 'v'},
	{"format",     a_argument, NULL, 'F'},
	{"from",       a_argument, NULL, 'f'},
	{"file",       a_argument, NULL, 'o'},
	{"nocolor",   no_argument, NULL, 'C'},
	{"nobanner",  no_argument, NULL, 'B'},
	{"help",      no_argument, NULL, 'h'},
	{"version",   no_argument, NULL, 'V'},
	{NULL,        no_argument, NULL, 0x0}
};

static const char * const opts_help[] = {
	"Scan all directories in PATH environment",
	"Scan all directories in /etc/ld.so.conf",
	"Use ld.so.conf to show full path (use with -r/-n)",
	"Root directory (use with -l or -p)",
	"Scan directories recursively",
	"Don't recursively cross mount points",
	"Don't scan symlinks",
	"Scan archives (.a files)",
	"Utilize ld.so.cache to show full path (use with -r/-n)",
	"Try and 'fix' bad things (use with -r/-e)",
	"Sets EI_PAX/PT_PAX_FLAGS to <arg> (use with -Xx)\n",
	"Print PaX markings",
	"Print GNU_STACK/PT_LOAD markings",
	"Print TEXTREL information",
	"Print RPATH information",
	"Print NEEDED information",
	"Print INTERP information",
	"Print BIND information",
	"Print SONAME information",
	"Find a specified symbol",
	"Find a specified section",
	"Find a specified library",
	"Use regex rather than string compare (with -s); specify twice for case insensitive",
	"Locate cause of TEXTREL",
	"Print only ELF files matching etype ET_DYN,ET_EXEC ...",
	"Print only ELF files matching numeric bits",
	"Print Endianness",
	"Print OSABI",
	"Print EABI (EM_ARM Only)",
	"Print only ELF files matching octal permissions",
	"Print ELF file size",
	"Print all useful/simple info\n",
	"Only output 'bad' things",
	"Be verbose (can be specified more than once)",
	"Use specified format for output",
	"Read input stream from a filename",
	"Write output stream to a filename",
	"Don't emit color in output",
	"Don't display the header",
	"Print this help and exit",
	"Print version and exit",
	NULL
};

/* display usage and exit */
static void usage(int status)
{
	const char a_arg[] = "<arg>";
	size_t a_arg_len = strlen(a_arg) + 2;
	size_t i;
	int optlen;
	printf("* Scan ELF binaries for stuff\n\n"
	       "Usage: %s [options] <dir1/file1> [dir2 dirN file2 fileN ...]\n\n", argv0);
	printf("Options: -[%s]\n", PARSE_FLAGS);

	/* prescan the --long opt length to auto-align */
	optlen = 0;
	for (i = 0; long_opts[i].name; ++i) {
		int l = strlen(long_opts[i].name);
		if (long_opts[i].has_arg == a_argument)
			l += a_arg_len;
		optlen = max(l, optlen);
	}

	for (i = 0; long_opts[i].name; ++i) {
		/* first output the short flag if it has one */
		if (long_opts[i].val > '~')
			printf("      ");
		else
			printf("  -%c, ", long_opts[i].val);

		/* then the long flag */
		if (long_opts[i].has_arg == no_argument)
			printf("--%-*s", optlen, long_opts[i].name);
		else
			printf("--%s %s %*s", long_opts[i].name, a_arg,
				(int)(optlen - strlen(long_opts[i].name) - a_arg_len), "");

		/* finally the help text */
		printf("* %s\n", opts_help[i]);
	}

	puts("\nFor more information, see the scanelf(1) manpage");
	exit(status);
}

/* parse command line arguments and preform needed actions */
#define do_pax_state(option, flag) \
	if (islower(option)) { \
		flags &= ~PF_##flag; \
		flags |= PF_NO##flag; \
	} else { \
		flags &= ~PF_NO##flag; \
		flags |= PF_##flag; \
	}
static int parseargs(int argc, char *argv[])
{
	int i;
	const char *from_file = NULL;
	int ret = 0;
	char load_cache_config = 0;

	opterr = 0;
	while ((i=getopt_long(argc, argv, PARSE_FLAGS, long_opts, NULL)) != -1) {
		switch (i) {

		case 'V':
			printf("pax-utils-%s: %s compiled %s\n%s\n"
			       "%s written for Gentoo by <solar and vapier @ gentoo.org>\n",
			       VERSION, __FILE__, __DATE__, rcsid, argv0);
			exit(EXIT_SUCCESS);
			break;
		case 'h': usage(EXIT_SUCCESS); break;
		case 'f':
			if (from_file) warn("You prob don't want to specify -f twice");
			from_file = optarg;
			break;
		case 'E':
			match_etypes = optarg;
			break;
		case 'M':
			match_bits = atoi(optarg);
			if (match_bits == 0) {
				if (strcmp(optarg, "ELFCLASS32") == 0)
					match_bits = 32;
				if (strcmp(optarg, "ELFCLASS64") == 0)
					match_bits = 64;
			}
			break;
		case 'O':
			if (sscanf(optarg, "%o", &match_perms) == -1)
				match_bits = 0;
			break;
		case 'o': {
			if (freopen(optarg, "w", stdout) == NULL)
				errp("Could not freopen(%s)", optarg);
			break;
		}
		case 'k':
			xarraypush_str(find_section_arr, optarg);
			break;
		case 's': {
			/* historically, this was comma delimited */
			char *this_sym = strtok(optarg, ",");
			if (!this_sym)	/* edge case: -s '' */
				xarraypush_str(find_sym_arr, "");
			while (this_sym) {
				xarraypush_str(find_sym_arr, this_sym);
				this_sym = strtok(NULL, ",");
			}
			break;
		}
		case 'N':
			xarraypush_str(find_lib_arr, optarg);
			break;
		case 'F': {
			if (out_format) warn("You prob don't want to specify -F twice");
			out_format = optarg;
			break;
		}
		case 'z': {
			unsigned long flags = (PF_NOEMUTRAMP | PF_NORANDEXEC);
			size_t x;

			for (x = 0; x < strlen(optarg); x++) {
				switch (optarg[x]) {
					case 'p':
					case 'P':
						do_pax_state(optarg[x], PAGEEXEC);
						break;
					case 's':
					case 'S':
						do_pax_state(optarg[x], SEGMEXEC);
						break;
					case 'm':
					case 'M':
						do_pax_state(optarg[x], MPROTECT);
						break;
					case 'e':
					case 'E':
						do_pax_state(optarg[x], EMUTRAMP);
						break;
					case 'r':
					case 'R':
						do_pax_state(optarg[x], RANDMMAP);
						break;
					case 'x':
					case 'X':
						do_pax_state(optarg[x], RANDEXEC);
						break;
					default:
						break;
				}
			}
			if (!(((flags & PF_PAGEEXEC) && (flags & PF_NOPAGEEXEC)) ||
				((flags & PF_SEGMEXEC) && (flags & PF_NOSEGMEXEC)) ||
				((flags & PF_RANDMMAP) && (flags & PF_NORANDMMAP)) ||
				((flags & PF_RANDEXEC) && (flags & PF_NORANDEXEC)) ||
				((flags & PF_EMUTRAMP) && (flags & PF_NOEMUTRAMP)) ||
				((flags & PF_RANDMMAP) && (flags & PF_NORANDMMAP))))
					setpax = flags;
			break;
		}
		case 'Z': show_size = 1; break;
		case 'g': ++g_match; break;
		case 'L': load_cache_config = use_ldcache = 1; break;
		case 'y': scan_symlink = 0; break;
		case 'A': scan_archives = 1; break;
		case 'C': color_init(true); break;
		case 'B': show_banner = 0; break;
		case 'l': load_cache_config = scan_ldpath = 1; break;
		case 'p': scan_envpath = 1; break;
		case 'R': dir_recurse = 1; break;
		case 'm': dir_crossmount = 0; break;
		case 'X': ++fix_elf; break;
		case 'x': show_pax = 1; break;
		case 'e': show_phdr = 1; break;
		case 't': show_textrel = 1; break;
		case 'r': show_rpath = 1; break;
		case 'n': show_needed = 1; break;
		case 'i': show_interp = 1; break;
		case 'b': show_bind = 1; break;
		case 'S': show_soname = 1; break;
		case 'T': show_textrels = 1; break;
		case 'q': be_quiet = min(be_quiet, 20) + 1; break;
		case 'v': be_verbose = min(be_verbose, 20) + 1; break;
		case 'a': show_perms = show_pax = show_phdr = show_textrel = show_rpath = show_bind = show_endian = 1; break;
		case 'D': show_endian = 1; break;
		case 'I': show_osabi = 1; break;
		case 'Y': show_eabi = 1; break;
		case 128:
			root_fd = open(optarg, O_RDONLY|O_CLOEXEC);
			if (root_fd == -1)
				err("Could not open root: %s", optarg);
			break;
		case 129: load_cache_config = use_ldpath = 1; break;
		case ':':
			err("Option '%c' is missing parameter", optopt);
		case '?':
			err("Unknown option '%c' or argument missing", optopt);
		default:
			err("Unhandled option '%c'; please report this", i);
		}
	}
	if (show_textrels && be_verbose)
		has_objdump = bin_in_path("objdump");
	/* precompile all the regexes */
	if (g_match) {
		regex_t preg;
		const char *this_sym;
		size_t n;
		int flags = REG_EXTENDED | REG_NOSUB | (g_match > 1 ? REG_ICASE : 0);

		array_for_each(find_sym_arr, n, this_sym) {
			/* see scanelf_match_symname for logic info */
			switch (this_sym[0]) {
			case '%':
				while (*(this_sym++))
					if (*this_sym == '%') {
						++this_sym;
						break;
					}
				break;
			case '+':
			case '-':
				++this_sym;
				break;
			}
			if (*this_sym == '*')
				++this_sym;

			ret = regcomp(&preg, this_sym, flags);
			if (ret) {
				char err[256];
				regerror(ret, &preg, err, sizeof(err));
				err("regcomp of %s failed: %s", this_sym, err);
			}
			xarraypush(find_sym_regex_arr, &preg, sizeof(preg));
		}
	}
	/* flatten arrays for display */
	if (array_cnt(find_sym_arr))
		find_sym = array_flatten_str(find_sym_arr);
	if (array_cnt(find_lib_arr))
		find_lib = array_flatten_str(find_lib_arr);
	if (array_cnt(find_section_arr))
		find_section = array_flatten_str(find_section_arr);
	/* let the format option override all other options */
	if (out_format) {
		show_pax = show_phdr = show_textrel = show_rpath = \
		show_needed = show_interp = show_bind = show_soname = \
		show_textrels = show_perms = show_endian = show_size = \
		show_osabi = show_eabi = 0;
		for (i = 0; out_format[i]; ++i) {
			if (!IS_MODIFIER(out_format[i])) continue;

			switch (out_format[++i]) {
			case '+': break;
			case '%': break;
			case '#': break;
			case 'F': break;
			case 'p': break;
			case 'f': break;
			case 'k': break;
			case 's': break;
			case 'N': break;
			case 'o': break;
			case 'a': break;
			case 'M': break;
			case 'Z': show_size = 1; break;
			case 'D': show_endian = 1; break;
			case 'I': show_osabi = 1; break;
			case 'Y': show_eabi = 1; break;
			case 'O': show_perms = 1; break;
			case 'x': show_pax = 1; break;
			case 'e': show_phdr = 1; break;
			case 't': show_textrel = 1; break;
			case 'r': show_rpath = 1; break;
			case 'n': show_needed = 1; break;
			case 'i': show_interp = 1; break;
			case 'b': show_bind = 1; break;
			case 'S': show_soname = 1; break;
			case 'T': show_textrels = 1; break;
			default:
				err("invalid format specifier '%c' (byte %i)",
				    out_format[i], i+1);
			}
		}

	/* construct our default format */
	} else {
		size_t fmt_len = 30;
		out_format = xmalloc(sizeof(char) * fmt_len);
		*out_format = '\0';
		if (!be_quiet)     xstrcat(&out_format, "%o ", &fmt_len);
		if (show_pax)      xstrcat(&out_format, "%x ", &fmt_len);
		if (show_perms)    xstrcat(&out_format, "%O ", &fmt_len);
		if (show_size)     xstrcat(&out_format, "%Z ", &fmt_len);
		if (show_endian)   xstrcat(&out_format, "%D ", &fmt_len);
		if (show_osabi)    xstrcat(&out_format, "%I ", &fmt_len);
		if (show_eabi)     xstrcat(&out_format, "%Y ", &fmt_len);
		if (show_phdr)     xstrcat(&out_format, "%e ", &fmt_len);
		if (show_textrel)  xstrcat(&out_format, "%t ", &fmt_len);
		if (show_rpath)    xstrcat(&out_format, "%r ", &fmt_len);
		if (show_needed)   xstrcat(&out_format, "%n ", &fmt_len);
		if (show_interp)   xstrcat(&out_format, "%i ", &fmt_len);
		if (show_bind)     xstrcat(&out_format, "%b ", &fmt_len);
		if (show_soname)   xstrcat(&out_format, "%S ", &fmt_len);
		if (show_textrels) xstrcat(&out_format, "%T ", &fmt_len);
		if (find_sym)      xstrcat(&out_format, "%s ", &fmt_len);
		if (find_section)  xstrcat(&out_format, "%k ", &fmt_len);
		if (find_lib)      xstrcat(&out_format, "%N ", &fmt_len);
		if (!be_quiet)     xstrcat(&out_format, "%F ", &fmt_len);
	}
	if (be_verbose > 2) printf("Format: %s\n", out_format);

	/* now lets actually do the scanning */
	if (load_cache_config)
		load_ld_cache_config(__PAX_UTILS_DEFAULT_LD_CACHE_CONFIG);
	if (scan_ldpath) scanelf_ldpath();
	if (scan_envpath) scanelf_envpath();
	if (!from_file && optind == argc && ttyname(0) == NULL && !scan_ldpath && !scan_envpath)
		from_file = "-";
	if (from_file) {
		scanelf_from_file(from_file);
		from_file = *argv;
	}
	if (optind == argc && !scan_ldpath && !scan_envpath && !from_file)
		err("Nothing to scan !?");
	while (optind < argc) {
		search_path = argv[optind++];
		ret = scanelf_dir(search_path);
	}

#ifdef __PAX_UTILS_CLEANUP
	/* clean up */
	xarrayfree(ldpaths);
	xarrayfree(find_sym_arr);
	xarrayfree(find_lib_arr);
	xarrayfree(find_section_arr);
	free(find_sym);
	free(find_lib);
	free(find_section);
	{
		size_t n;
		regex_t *preg;
		array_for_each(find_sym_regex_arr, n, preg)
			regfree(preg);
		xarrayfree(find_sym_regex_arr);
	}

	if (ldcache != 0)
		munmap(ldcache, ldcache_size);
#endif

	return ret;
}

static char **get_split_env(const char *envvar)
{
	const char *delims = " \t\n";
	char **envvals = NULL;
	char *env, *s;
	int nentry;

	if ((env = getenv(envvar)) == NULL)
		return NULL;

	env = xstrdup(env);
	if (env == NULL)
		return NULL;

	s = strtok(env, delims);
	if (s == NULL) {
		free(env);
		return NULL;
	}

	nentry = 0;
	while (s != NULL) {
		++nentry;
		envvals = xrealloc(envvals, sizeof(*envvals) * (nentry+1));
		envvals[nentry-1] = s;
		s = strtok(NULL, delims);
	}
	envvals[nentry] = NULL;

	/* don't want to free(env) as it contains the memory that backs
	 * the envvals array of strings */
	return envvals;
}

static void parseenv(void)
{
	color_init(false);
	qa_textrels = get_split_env("QA_TEXTRELS");
	qa_execstack = get_split_env("QA_EXECSTACK");
	qa_wx_load = get_split_env("QA_WX_LOAD");
}

#ifdef __PAX_UTILS_CLEANUP
static void cleanup(void)
{
	free(out_format);
	free(qa_textrels);
	free(qa_execstack);
	free(qa_wx_load);
}
#endif

int main(int argc, char *argv[])
{
	int ret;
	if (argc < 2)
		usage(EXIT_FAILURE);
	parseenv();
	ret = parseargs(argc, argv);
	fclose(stdout);
#ifdef __PAX_UTILS_CLEANUP
	cleanup();
	warn("The calls to add/delete heap should be off:\n"
	     "\t- 1 due to the out_buffer not being freed in scanelf_fileat()\n"
	     "\t- 1 per QA_TEXTRELS/QA_EXECSTACK/QA_WX_LOAD");
#endif
	return ret;
}

/* Match filename against entries in matchlist, return TRUE
 * if the file is listed */
static int file_matches_list(const char *filename, char **matchlist)
{
	char **file;
	char *match;
	char buf[__PAX_UTILS_PATH_MAX];

	if (matchlist == NULL)
		return 0;

	for (file = matchlist; *file != NULL; file++) {
		if (search_path) {
			snprintf(buf, sizeof(buf), "%s%s", search_path, *file);
			match = buf;
		} else {
			match = *file;
		}
		if (fnmatch(match, filename, 0) == 0)
			return 1;
	}
	return 0;
}

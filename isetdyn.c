/*
 * Copyright 2003 Ned Ludd <solar@gentoo.org>
 * Distributed under the terms of the GNU General Public License v2
 * $Header: /var/cvsroot/gentoo-projects/pax-utils/isetdyn.c,v 1.3 2003/10/26 23:42:10 solar Exp $
 *
 * On Gentoo Linux we need a simple way to detect if an ELF ehdr is of
 * type ET_DYN, we have a PT_INTERP phdr and also contains a symbol for main()
 * When these three conditions are true as we should be strip safe.
 ********************************************************************
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 ********************************************************************
 *
 * Date:
 *	20030908
 *	20031007
 * Compile:
 *	gcc -o isetdyn isetdyn.c -ldl
 * Note:
 *
 * This program has visible standard output only when the file is a
 * et_dyn and meets all requirments.
 *	
 * It's intended use is from shell scripts via return values 
 *
 */

/* 
 * <pappy-> # readelf -a filename | grep "program interpreter"
 * <pappy-> # readelf -a filename | grep "__libc_start_main"
 * <pappy-> # readelf -a filename | grep "Type:
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include "paxelf.h"

int main(int argc, char **argv)
{
   int i = 0;
   elfobj *elf = NULL;
   void *handle = NULL;
   int exit_val = 1;

   if (argc < 2) {
      fprintf(stderr, "Usage: %s <filename>\n", (*argv == NULL) ? "isetdyn" : *argv);
      return exit_val;
   }

   if ((elf = readelf(argv[1])) == NULL)
      return exit_val;

   if (!check_elf_header(elf->ehdr))
      if (IS_ELF_ET_DYN(elf))
	 for (i = 0; i < elf->ehdr->e_phnum; i++)
	    if (elf->phdr[i].p_type == PT_INTERP)
	       if ((handle = dlopen(argv[1], RTLD_NOW)) != NULL)
		  if ((dlsym(handle, "main")) != 0x0) {
		     puts(argv[1]);
		     exit_val = 0;
		  }

   if (handle != NULL)
      dlclose(handle);

   munmap(elf->data, elf->len);
   free(elf);
   return exit_val;
}

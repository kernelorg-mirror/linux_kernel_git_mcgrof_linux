#ifndef _TOOLS_LINUX_SECTIONS_H_

/* Mostly a copy of what we need only */

#define SECTION_INIT			.init.text
#define SECTION_RODATA			.rodata

#define ___SECTION_TBL_STR(section, name)				\
	#section ".tbl." #name
#define SECTION_TBL_ALL_STR(section)					\
	___SECTION_TBL_STR(section, *)

#endif /* _TOOLS_LINUX_SECTIONS_H_ */

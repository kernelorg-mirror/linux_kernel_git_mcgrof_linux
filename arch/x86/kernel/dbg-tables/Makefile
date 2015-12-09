# You should see all these compile but at run time you'd
# only see the ones that were linked in.

# This ensures we always compile but only link
# in alpha if it was enabled. This is typically what
# you are expected to use to avoid code bit-rot.
table-$(CONFIG_X86_DEBUG_LINKER_TABLE_ALPHA)	+= alpha.o

# This accomplishes the same, but requires 2 lines.
extra-y						+= beta.o
obj-$(CONFIG_X86_DEBUG_LINKER_TABLE_BETA)	+= beta.o

table-$(CONFIG_X86_DEBUG_LINKER_TABLE_GAMMA)	+= gamma.o

# If you *know* you only want to enable compilation of
# a feature when its selected you can just use the good
# ol' obj
obj-$(CONFIG_X86_DEBUG_LINKER_TABLE_DELTA)	+= delta.o

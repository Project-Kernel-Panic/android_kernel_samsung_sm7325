# SPDX-License-Identifier: GPL-2.0-only

zram-y	:=	zcomp.o zram_drv.o

zram-$(CONFIG_LZO_COMPRESS)		+= backend_lzorle.o backend_lzo.o
zram-$(CONFIG_LZ4_COMPRESS)		+= backend_lz4.o
zram-$(CONFIG_LZ4HC_COMPRESS)		+= backend_lz4hc.o
zram-$(CONFIG_ZSTD_COMPRESS)		+= backend_zstd.o
zram-$(CONFIG_ZLIB_DEFLATE)		+= backend_deflate.o
zram-$(CONFIG_842_COMPRESS)		+= backend_842.o

obj-$(CONFIG_ZRAM)	+=	zram.o

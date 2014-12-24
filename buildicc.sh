#!/bin/sh

cp -f scripts/pnglibconf.h.prebuilt pnglibconf.h

source /opt/icc2013/bin/compilervars.sh ia32

make -f GNUmakefile DEBUG=1 ICC=1 clean
make -f GNUmakefile DEBUG=0 ICC=1 clean

make -f GNUmakefile DEBUG=1 ICC=1 CC=icc AR=xiar
make -f GNUmakefile DEBUG=0 ICC=1 CC=icc AR=xiar

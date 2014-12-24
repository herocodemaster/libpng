#!/bin/sh

bakefile -f gnu png-linux.bkl

cp -f scripts/pnglibconf.h.prebuilt pnglibconf.h

make -f GNUmakefile DEBUG=1 clean
make -f GNUmakefile DEBUG=0 clean

make -f GNUmakefile DEBUG=1
make -f GNUmakefile DEBUG=0

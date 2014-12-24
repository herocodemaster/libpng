:: 
:: Copyright (c) Unary Corporation. All Rights Reserved.
::
:: Author: Master
:: 

@cd /d %~dp0 

@bakefile -f intelw png-nt.bkl

@copy /Y scripts\pnglibconf.h.prebuilt pnglibconf.h

@pushd .
@set VCEVNDIR=%UNADEVKIT_ROOT%\icc2013\setenv.bat
@call "%VCEVNDIR%" 
@popd

@nmake -f makefile.icw DEBUG=1 clean SIGN=1 TOOL_CHAIN=icc2013
@nmake -f makefile.icw DEBUG=0 clean SIGN=1 TOOL_CHAIN=icc2013

@nmake -f makefile.icw DEBUG=1 SIGN=1 UNABASE_PATH="%UNABASE_ROOT%" TOOL_CHAIN=icc2013
@nmake -f makefile.icw DEBUG=0 SIGN=1 UNABASE_PATH="%UNABASE_ROOT%" TOOL_CHAIN=icc2013

@PAUSE

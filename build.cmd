:: 
:: Copyright (c) Unary Corporation. All Rights Reserved.
::
:: Author: Master
:: 

@cd /d %~dp0 

@bakefile -f msvc png-nt.bkl

@copy /Y scripts\pnglibconf.h.prebuilt pnglibconf.h

@pushd .
@set VCEVNDIR=%UNADEVKIT_ROOT%\vc2008\setenv.bat
@call "%VCEVNDIR%" 
@popd

@nmake -f makefile.vc DEBUG=1 clean SIGN=1
@nmake -f makefile.vc DEBUG=0 clean SIGN=1

@nmake -f makefile.vc DEBUG=1 SIGN=1 UNABASE_PATH="%UNABASE_ROOT%"
@nmake -f makefile.vc DEBUG=0 SIGN=1 UNABASE_PATH="%UNABASE_ROOT%"

@PAUSE

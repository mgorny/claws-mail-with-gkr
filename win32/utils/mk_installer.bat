@echo off
rem create setup.iss from setup.iss.in and configure.ac
set ROOT=../..
set SETUP_ISS=sylpheed-setup.iss
set SETUP_ISS_IN=%SETUP_ISS%.in
set MKINST=mkinstaller.bat
rem @ECHO OFF > mkvertmp.bat
grep -e "^[^ ]*VERSION=" %ROOT%/configure.ac | sed -e "s/^/SET /;s/EXTRA_VERSION$/%%EXTRA_VERSION%% Win32 (GTK2)/" >> %MKINST%
echo SET SHORTVER=%%MAJOR_VERSION%%%%MINOR_VERSION%%%%MICRO_VERSION%%>> %MKINST%
echo if %%EXTRA_WIN32_VERSION%%==.0 set EXTRA_WIN32_VERSION=>> %MKINST%
echo if %%EXTRA_WIN32_VERSION%%==.1 set EXTRA_WIN32_VERSION=>> %MKINST%
echo if %%EXTRA_WIN32_VERSION%%x==x%%EXTRA_WIN32_VERSION%% set EXTRA_WIN32_VERSION=>> %MKINST%
echo if %%EXTRA_VERSION%%x==x%%EXTRA_VERSION%% goto RELEASE>> %MKINST%
echo if %%EXTRA_VERSION%%==0 goto RELEASE>> %MKINST%
echo set CVS_VERSION=cvs%%EXTRA_VERSION%%%%EXTRA_WIN32_VERSION%%>> %MKINST%
echo goto VERSIONEND>> %MKINST%
echo :RELEASE>> %MKINST%
echo SET EXTRA_VERSION=>> %MKINST%
echo set CVS_VERSION=%%EXTRA_WIN32_VERSION%%>> %MKINST%
echo :VERSIONEND>> %MKINST%
echo SET VERSION=%%MAJOR_VERSION%%.%%MINOR_VERSION%%.%%MICRO_VERSION%%%%CVS_VERSION%%>> %MKINST%
echo SET PAKDIR=d:\\\\_pak\\\\sylpheed-claws-%%VERSION%%>> %MKINST%
echo sed -e "s/@VERSION@/%%VERSION%%/;s/@MAJOR_VERSION@/%%MAJOR_VERSION%%/;s/@MINOR_VERSION@/%%MINOR_VERSION%%/;s/@MICRO_VERSION@/%%MICRO_VERSION%%/;s/@EXTRA_VERSION@/%%EXTRA_VERSION%%/;s/@PAKDIR@/%%PAKDIR%%/" %SETUP_ISS_IN% >> %MKINST%
type %MKINST%
call %MKINST% > %SETUP_ISS%
del %MKINST%
set ROOT=
start %SETUP_ISS%

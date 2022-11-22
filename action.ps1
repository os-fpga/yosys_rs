Write-Output "action from Yosys starts"

wget https://www.zlib.net/fossils/zlib-1.2.11.tar.gz
wget https://yosyshq.net/yosys/nogit/YosysVS-Tpl-v2.zip

unzip YosysVS-Tpl-v2.zip

$YosysDir = Get-Location
Set-Location ../Raptor_Tools/verific_rs/vhdl_packages
c:\cygwin64\bin\bash.exe -c "ln -sf vhdl_1987/bin vdbs_1987"
c:\cygwin64\bin\bash.exe -c "ln -sf vhdl_1993/bin vdbs_1993"
c:\cygwin64\bin\bash.exe -c "ln -sf vhdl_2008/bin vdbs_2008"
c:\cygwin64\bin\bash.exe -c "ln -sf vhdl_2019/bin vdbs_2019"
Set-Location $YosysDir


#curl https://www.zlib.net/fossils/zlib-1.2.11.tar.gz
#curl https://yosyshq.net/yosys/nogit/YosysVS-Tpl-v2.zip

c:\cygwin64\bin\make genfiles SHELL=C:\cygwin64\bin\bash

Write-Output "Yosys action end"

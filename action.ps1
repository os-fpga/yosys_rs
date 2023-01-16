Write-Output "action from Yosys starts"

$OldPATH = $PATH
$env:PATH = (Test-Path -Path "C:\cygwin64\bin") ? "C:\cygwin64\bin\" : "C:\cygwin\bin\"
$env:PATH -split ";"
$Cygwin = $env:PATH + "bash.exe"
$arg = "-c"

$YosysDir = Get-Location
Set-Location ../Raptor_Tools/verific_rs/vhdl_packages

& $Cygwin $arg "ln -sf vhdl_1987/bin vdbs_1987"
& $Cygwin $arg "ln -sf vhdl_1993/bin vdbs_1993"
& $Cygwin $arg "ln -sf vhdl_2008/bin vdbs_2008"
& $Cygwin $arg "ln -sf vhdl_2019/bin vdbs_2019"

Set-Location $YosysDir
& $Cygwin $arg "make genfiles " "SHELL=C:\\cygwin64\\bin\\bash"
$env:PATH = $OldPath

Write-Output "Yosys action end"


#!/usr/bin/bash


set -ex
msvc="$1-$2"
yosysver="$2"
gitsha="$3"

echo "namespace Yosys { extern const char *yosys_verific_version_str; const char *yosys_verific_version_str=\"Yosys_verific_rs Windows Version $yosysver(git hash $gitsha ) )\";
		extern const char *yosys_version_str; const char *yosys_version_str=\"$yosysver\";}" > kernel/version.cc

rm -rf YosysVS-Tpl-v2.zip YosysVS


wget https://yosyshq.net/yosys/nogit/YosysVS-Tpl-v2.zip
wget https://www.zlib.net/fossils/zlib-1.2.11.tar.gz

unzip YosysVS-Tpl-v2.zip
rm -f YosysVS-Tpl-v2.zip
tar xvfz zlib-1.2.11.tar.gz

mkdir -p ./libs/zlib
mv ./zlib-1.2.11/* ./libs/zlib/.
rm -rf ./zlib-1.2.11
pushd ./
ls libs/zlib/*.c | sed 's,.*:,,; s,//*,/,g; s,/[^/]*/\.\./,/,g; y, \\,\n\n,;' | grep '^[^/]'  >> ./srcfiles.txt


tar -cf - -T srcfiles.txt | tar -xf - -C ./

cat > readme-git.txt << EOT
Want to use a git working copy for the yosys source code?
Open "Git Bash" in this directory and run:

	mv yosys yosys.bak
	git clone https://github.com/YosysHQ/yosys.git yosys
	cd yosys
	git checkout -B master $(git rev-parse HEAD | cut -c1-10)
	unzip ../genfiles.zip
EOT

cat > ./readme-abc.txt << EOT
Yosys is using "ABC" for gate-level optimizations and technology
mapping. Download yosys-win32-mxebin-$yosysver.zip and copy the
following files from it into this directory:

	pthreadVC2.dll
	yosys-abc.exe
EOT

sed -i 's/$/\r/; s/\r\r*/\r/g;' ./YosysVS.vcxproj ./readme-git.txt ./readme-abc.txt


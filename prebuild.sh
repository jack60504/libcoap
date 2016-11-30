#!/bin/bash
cur=$(pwd)

if [ "$1"x = "linux"x ]
then
	test -d ./linux || mkdir -p ./linux
	path=$cur/linux
	host=
else 
	test -d ./rt5350 || mkdir -p ./rt5350
	path=$cur/rt5350
	host="--host=mipsel-linux"
fi
echo $host $path
./configure CFLAGS=-O2 $host --prefix=$path --disable-examples

#!/bin/bash
cur=$(pwd)

if [ "$1"x = "linux"x ]
then
	mkdir linux
	path=$(pwd)/linux
	host=
else 
	mkdir rt5350
	path=$(pwd)/rt5350
	host="--host=mipsel-linux"
fi
echo $host $path
./configure CFLAGS=-O2 $host --prefix=$path --disable-examples

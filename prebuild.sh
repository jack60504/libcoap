#!/bin/bash
cur=$(pwd)/libs/libcoap

if [ "$1"x = "linux"x ]
then
	mkdir -f $cur/linux
	path=$(pwd)/linux
	host=
else 
	mkdir -f $cur/rt5350
	path=$(pwd)/rt5350
	host="--host=mipsel-linux"
fi
echo $host $path
$cur/configure CFLAGS=-O2 $host --prefix=$path --disable-examples

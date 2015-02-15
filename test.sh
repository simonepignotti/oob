#!/bin/bash

./s 8 >>supserout 2>>supsererr &
sleep 2

for i in `seq 1 9`
do
	./c 5 8 20 >>cliout &
	./c 5 8 20 >>cliout &
	sleep 1
done

./c 5 8 20 >>cliout &
./c 5 8 20 >>cliout &

for i in `seq 1 5`
do
	sleep 10
	kill -2 `pidof s`
done

sleep 10
kill -2 `pidof s`
kill -2 `pidof s`
./misura.sh cliout supserout


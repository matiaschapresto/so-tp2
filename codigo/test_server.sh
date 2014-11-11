#!/bin/bash

i=1
while [ $i -le $1 ]
do
    python server_tester.py &
    i=$(($i + 1))
done

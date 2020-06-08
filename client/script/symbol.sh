#!/bin/bash

MODULE_NAME=pmdfc
MODULE_FILE=$(modinfo ../$MODULE_NAME.ko| awk '/filename/{print $2}')
DIR="/sys/module/${MODULE_NAME}/sections/"
echo add-symbol-file $MODULE_FILE $(cat "$DIR/.text") -s .bss $(cat "$DIR/.bss") -s .data $(cat "$DIR/.data")

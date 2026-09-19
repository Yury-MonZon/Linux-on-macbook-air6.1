#!/bin/bash
r=$(rdmsr 0x1FC)
f=$(( (0x$r) & 0xFFFFE ))
wrmsr 0x1FC $f
echo "BD PROCHOT disabled"

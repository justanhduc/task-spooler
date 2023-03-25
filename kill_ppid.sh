#!/bin/bash

# getting children generally resolves nicely at some point
get_child() {
    echo $(pgrep -laP $1 | awk '{print $1}')
}

get_children() {
    __RET=$(get_child $1)
    __CHILDREN=
    while [ -n "$__RET" ]; do
        __CHILDREN+="$__RET "
        __RET=$(get_child $__RET)
    done

    __CHILDREN=$(echo "${__CHILDREN}" | xargs | sort)

    echo "${__CHILDREN} $1"
}

if [ 1 -gt $# ]; 
then
    echo "not input PID"
    exit 1
fi

owner=`ps -o user= -p $1`
if [ -z "$owner" ]; 
then
    # echo "not a valid PID"
    exit 1
fi
pids=`get_children $1`

user=`whoami`

extra=""
if [[ "$owner" != "$user" ]]; then
    extra="sudo"
fi

for pid in ${pids}; 
do
    if [ -z "$2" ]
    then
        echo "${extra} ${pid}"
    else
        ${extra} $2 ${pid}
    fi
done

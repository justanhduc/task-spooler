#!/bin/bash

# getting children generally resolves nicely at some point
get_child() {
    echo $(pgrep -laP $1 | awk '{print $1}')
}

# recursively getting parents isn't that useful, 
# so single use function
get_parent() {
    echo $(ps -o ppid= -p 36700)
}

get_children() {
    __RET=$(get_child $1)
    __CHILDREN=
    while [ -n "$__RET" ]; do
        __CHILDREN+="$__RET "
        __RET=$(get_child $__RET)
    done

    __CHILDREN=$(echo "${__CHILDREN}" | xargs | sort)

    echo "${__CHILDREN}"
}

pids=`get_children $1`
for pid in ${pids}; 
do
    if [ -n $2 ]
    then
        # echo ${pid} $2 >> pid.txt
        sudo kill $2 ${pid}
    else
        echo ${pid}
    fi
done

if [ -n $2 ]
then
    # echo PPID= $1 $2 >> pid.txt
    sudo kill $2 $1
else
    echo PPID= $1
fi



#!/bin/bash

COUNT=${1:-300}

for ((i=1; i<=COUNT; i++)); do
    N=$((RANDOM % 6 + 1))
    Y=$((RANDOM % 31 + 5))

    echo "[$i/$COUNT] N=$N Y=$Y"

    task-spooler -N "$N" mpirun -np "$N" ../tools/mpi_pi "$Y"
    sleep 0.1
done

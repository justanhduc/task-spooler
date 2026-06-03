#!/bin/bash

COUNT=300

for ((i=1; i<=COUNT; i++)); do
    N=$((RANDOM % 6 + 1))
    Y=$((RANDOM % 31 + 5))

    echo "[$i/$COUNT] N=$N Y=$Y"

    task-spooler -N "$N" mpirun -np "$N" mpi_pi "$Y"
done

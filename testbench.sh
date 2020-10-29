#!/bin/bash

# Some simple tasks
./ts -K
./ts ls
./ts -n ls > /dev/null
./ts -f ls
./ts -nf ls > /dev/null
./ts ls
./ts cat
./ts -w

LINES=`./ts -l | grep finished | wc -l`
if [ $LINES -ne 5 ]; then
  echo "Error in simple tasks."
  exit 1
fi

./ts -K

# Check errorlevel 1
./ts -f ls
if [ $? -ne 0 ]; then
  echo "Error in errorlevel 1."
  exit 1
fi
# Check errorlevel 2
./ts -f patata
if [ $? -eq 0 ]; then
  echo "Error in errorlevel 2."
  exit 1
fi
# Check errorlevel 3
./ts patata
if [ $? -ne 0 ]; then
  echo "Error in errorlevel 3."
  exit 1
fi
# Check errorlevel 4
./ts ls
./ts -w
if [ $? -ne 0 ]; then
  echo "Error in errorlevel 4."
  exit 1
fi
# Check errorlevel 5
./ts patata
./ts -w
if [ $? -eq 0 ]; then
  echo "Error in errorlevel 5."
  exit 1
fi
./ts -K

# Check urgency
./ts sleep 1
./ts ls
./ts patata
./ts -w
if [ $? -eq 0 ]; then
  echo "Error in urgency 1."
  exit 1
fi

./ts sleep 1
./ts ls
./ts patata
./ts -u
./ts -w
if [ $? -ne 0 ]; then
  echo "Error in urgency 2."
  exit 1
fi

./ts -K
./ts sleep 1
./ts ls
./ts patata
./ts -u 2
./ts -w
if [ $? -ne 0 ]; then
  echo "Error in urgency 3."
  exit 1
fi

# Test remove job
./ts -K
./ts sleep 1 &&
./ts ls 1 &&
./ts ls 2 &&
./ts -r 1 &&
./ts -r &&
./ts sleep 1 &&
./ts -n ls > /dev/null &&
./ts -n ls 2 > /dev/null &&
./ts -r &&
./ts -w
if [ $? -ne 0 ]; then
  echo "Error in remove job."
  exit 1
fi

./ts -K

# Test not adding the job to finished.
./ts ls
./ts -w
LINES=`./ts -l | grep finished | wc -l`
if [ $LINES -ne 1 ]; then
  echo "Error in not adding the job to finished."
  exit 1
fi

./ts -nf ls > /dev/null
LINES=`./ts -l | grep finished | wc -l`
if [ $LINES -ne 1 ]; then
  echo "Error in not adding the job to finished."
  exit 1
fi

./ts -K

# Test clearing the finished jobs
./ts ls
./ts ls
./ts ls
./ts -nf ls > /dev/null
./ts -C

LINES=`./ts -l | wc -l`
if [ $LINES -ne 1 ]; then
  echo "Error clearing the finished jobs."
  exit 1
fi

./ts -K

# Test clearing the finished jobs
# We start the daemon
./ts > /dev/null
J1=`./ts sleep 1`
J2=`./ts sleep 2`
J3=`./ts sleep 3`
./ts -U $J2-$J3

if [ $? -ne 0 ]; then
  echo "Error clearing the finished jobs."
  exit 1
fi

./ts -K

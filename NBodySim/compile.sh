#!/bin/bash

drawWidth=$(tput cols)
drawHeight=$(tput lines)

let halfWidth=drawWidth/2

if [ $halfWidth -lt $drawHeight ]; then # the window is too narrow
	echo 1
	let drawHeight=$halfWidth
elif [ $halfWidth -gt $drawHeight ]; then # the window is too thicc
	echo "2 $halfWidth $drawHeight"
	let drawWidth=drawHeight*2
fi

echo "Will draw to conole at ${drawWidth}x$drawHeight character resolution"

args="-lm -std=gnu11 -DCONSOLE_W=$drawWidth -DCONSOLE_H=$drawHeight -lpthread"

if [ $mode = "debug" ]; then
	printf "Compiling debug version.\nTo compile for production, run: 'export mode=prod' then then recompile\n"
	# args="$args -g -O0 -DDEBUG"
	args="$args -g -O0"
elif [ $mode = "prof"]; then
		printf "Compiling profiling version."
		
		args="$args -g -O3"
else
	printf "Compiling runtime version.\nTo compile for debug, run: 'export mode=debug' then then recompile\n"
	args+=" -O3"
fi

if [ `uname` = "Darwin" ]; then
	args+=" -I /opt/X11/include/cairo -L /usr/lib -l cairo"
else
	args+=" -I /usr/include/cairo -L /usr/lib -l cairo"
fi

if [ -z "${debug+x}" ]; then debug="false"; fi

command="gcc ./main.c ./guiOutput.c ./TestCaseInitializers.c ./SaveState.c ./RNG.c -o ./data/simulator $args"
echo "Full compilation instruction is: $command"
eval "$command"

printf "Compilation complete\n"

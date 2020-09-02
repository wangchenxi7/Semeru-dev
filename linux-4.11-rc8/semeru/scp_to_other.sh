#!/bin/bash


dest="$1"

if [ -z "${dest}"  ]
then
	echo "Enter diestination, e.g. qemu, server_home"
	read dest
fi



if [ "${dest}" = "qemu"  ]
then
	echo "scp -r  ${HOME}/linux-4.11-rc8/semeru  qemu:~/linux-4.11-rc8/"
	scp -r  ${HOME}/linux-4.11-rc8/semeru  qemu:~/linux-4.11-rc8/

elif [ "${dest}" = "server_home"  ]
then

	echo "scp -r  ${HOME}/linux-4.11-rc8/semeru  server_home:~/linux-4.11-rc8/"
	scp -r  ${HOME}/linux-4.11-rc8/semeru  server_home:~/linux-4.11-rc8/
	
else

	echo"!! Wrong choice : ${dest} !!"
	exit 
fi

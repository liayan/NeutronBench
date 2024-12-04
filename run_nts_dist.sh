#i!/bin/bash
cur_dir=$(cd "$(dirname "$0")"; pwd)


cat hostfile| while read line; do
#ssh ${USER}@${WORKERS[$i]} "mkdir -p ${cur_dir}"
#echo ${line}
scp -r ./$2  ${USER}@${line}:${cur_dir}/
done
mpiexec --allow-run-as-root -hostfile hostfile -np $1 ./build/nts $2




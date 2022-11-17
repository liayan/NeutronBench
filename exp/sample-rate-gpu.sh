#!/bin/bash

# read hostfile
# hosts=()
# while read line
# do
#   hosts[${#hosts[@]}]=$line
# done < hostfile

# sync func (filename, dest_dir)
# host_num=${#hosts[@]}
# echo "host_num ${host_num}"

function sync(){
  for((i=1;i<${host_num};i++));  
  do   
    echo "scp -r $1  ${USER}@${hosts[$i]}:$2/"
    scp -r $1  ${USER}@${hosts[$i]}:$2/
  done
}

if [ ! -d "build" ]; then
  mkdir build && cd build && cmake ../.. && cd ..
fi

if [ ! -d "log" ]; then
  mkdir log
fi

# sync /root/neutron-sanzo /root
# sync 'hostfile' $(pwd)
# sync data $(pwd)
# exit

function new_cfg() {
  echo -e "VERTICES:$1" > tmp.cfg
  echo -e "LAYERS:$2" >> tmp.cfg
  echo -e "EDGE_FILE:$3.edge" >> tmp.cfg
  echo -e "FEATURE_FILE:$3.feat" >> tmp.cfg
  echo -e "LABEL_FILE:$3.label" >> tmp.cfg
  echo -e "MASK_FILE:$3.mask" >> tmp.cfg
  echo -e "ALGORITHM:$4" >> tmp.cfg
  echo -e "FANOUT:$5" >> tmp.cfg
  echo -e "BATCH_SIZE:$6" >> tmp.cfg
  echo -e "EPOCHS:$7" >> tmp.cfg
  echo -e "BATCH_TYPE:$8" >> tmp.cfg
  echo -e "LEARN_RATE:$9" >> tmp.cfg
  echo -e "WEIGHT_DECAY:${10}" >> tmp.cfg
  echo -e "DROP_RATE:${11}" >> tmp.cfg
  echo -e "MINI_PULL:${12}" >> tmp.cfg
  echo -e "BATCH_NORM:${13}" >> tmp.cfg
  echo -e "TIME_SKIP:3" >> tmp.cfg
  echo -e "RUNS:${14}" >> tmp.cfg
  echo -e "CLASSES:${15}" >> tmp.cfg
  echo -e "SAMPLE_RATE:${16}" >> tmp.cfg

  other_paras="PROC_OVERLAP:0\nPROC_LOCAL:0\nPROC_CUDA:0\nPROC_REP:0\nLOCK_FREE:1\nDECAY_EPOCH:100\nDECAY_RATE:0.97"
  echo -e ${other_paras} >> tmp.cfg
}

cd build && make -j $(nproc) && cd ..

log_path="./log/sample-rate/gpu"

function batch_size() {
  if [ ! -d ${log_path} ]; then
    mkdir -p ${log_path}
  fi

  dataset=${4##*/}
  echo "dataset: ${dataset}"

  for bs in $1; do
    echo "run ${dataset} size $bs..."
    new_cfg $2 $3 $4 $5 $6 $bs $7 $8 $9 ${10} ${11} ${12} ${13} ${14} ${15}
    echo "mpiexec -hostfile hostfile -np 1 ./build/nts tmp.cfg > "${log_path}/${dataset}_${bs}.log""
    mpiexec -hostfile hostfile -np 1 ./build/nts tmp.cfg > "${log_path}/${dataset}_${bs}.log"
  done
}


function sample_rate() {
  if [ ! -d ${log_path} ]; then
    mkdir -p ${log_path}
  fi

  dataset=${4##*/}
  echo "dataset: ${dataset}"

  for sr in ${16}; do
    echo "run ${dataset} size ${sr}..."
    new_cfg $2 $3 $4 $5 $6 $1 $7 $8 $9 ${10} ${11} ${12} ${13} ${14} ${15} ${sr}
    echo "mpiexec -hostfile hostfile -np 1 ./build/nts tmp.cfg > "${log_path}/${dataset}_${sr}.log""
    mpiexec -hostfile hostfile -np 1 ./build/nts tmp.cfg > "${log_path}/${dataset}_${sr}.log"
  done
}
# cora
# array=('32' '64' '140')
# 1024y[*]}" 2708 1433-128-7 ../data/cora/cora GCNNEIGHBOR 15,25 200 shuffle 0.01 0.0001 0.5 0 0 3 1  "${array[*]}"

# # citeseer
# array=('32' '64' '120')
# 1024y[*]}" 3327 3703-128-6 ../data/citeseer/citeseer GCNNEIGHBOR 15,25 200 shuffle 0.01 0.0001 0.5 0 0 3 1  "${array[*]}"

# # pubmed
# array=('32' '60')
# 1024y[*]}" 19717 500-128-3 ../data/pubmed/pubmed GCNNEIGHBOR 15,25 200 shuffle 0.01 0.0001 0.5 0 0 3 1  "${array[*]}"

# ppi
# # 44906/6514/5524
array=('0.1' '0.2' '0.3' '0.4' '0.5' '0.6' '0.7' '0.8' '0.9', '1.0')
sample_rate 1024 14755 50-128-121 ../data/ppi/ppi GCNNEIGHBORGPU 15,25 400 shuffle 0.01 0.0001 0.5 1 0 3 121 "${array[*]}"


# ppi-large
array=('0.1' '0.2' '0.3' '0.4' '0.5' '0.6' '0.7' '0.8' '0.9', '1.0')
sample_rate 1024 56944 50-128-121 ../data/ppi-large/ppi-large GCNNEIGHBORGPU 15,25 400 shuffle 0.01 0.0001 0.5 1 0 3 121  "${array[*]}"

# Flickr
array=('0.1' '0.2' '0.3' '0.4' '0.5' '0.6' '0.7' '0.8' '0.9', '1.0')
sample_rate 1024 89250 500-128-7 ../data/flickr/flickr GCNNEIGHBORGPU 15,25 400 shuffle 0.01 0.0001 0.5 1 0 3 1  "${array[*]}"

# AmazonCoBuyComputer
# 8250 4125 1377
array=('0.1' '0.2' '0.3' '0.4' '0.5' '0.6' '0.7' '0.8' '0.9', '1.0')
sample_rate 1024 13752 767-128-10 ../data/AmazonCoBuy_computers/AmazonCoBuy_computers GCNNEIGHBORGPU 15,25 400 shuffle 0.01 0.0001 0.5 1 0 3 1  "${array[*]}"

# AmazonCoBuyPhoto
# 4590
array=('0.1' '0.2' '0.3' '0.4' '0.5' '0.6' '0.7' '0.8' '0.9', '1.0')
sample_rate 1024 7650 745-128-8 ../data/AmazonCoBuy_photo/AmazonCoBuy_photo GCNNEIGHBORGPU 15,25 400 shuffle 0.01 0.0001 0.5 1 0 3 1  "${array[*]}"

# Reddit
# 512, 1k, 2k, 4k, 8k, 16k, 32k, 64k, 128k, full
array=('0.1' '0.2' '0.3' '0.4' '0.5' '0.6' '0.7' '0.8' '0.9', '1.0')
sample_rate 65536 232965 602-128-41 ../data/reddit/reddit GCNNEIGHBORGPU 15,25 400 shuffle 0.01 0.0001 0.5 1 0 3 1  "${array[*]}"

# Yelp
# array=('0.1' '0.2' '0.3' '0.4' '0.5' '0.6' '0.7' '0.8' '0.9', '1.0')
# 1024y[*]}" 716847 300-128-100 ../data/yelp/yelp GCNNEIGHBORGPU 15,25 100 shuffle 0.01 0.0001 0.5 1 0 3 100  "${array[*]}"

# # Arxiv
# array=('512' '1024' '2048' '4096' '8192' '16384' '32768' '65536' '90941')
# 1024y[*]}" 169343 128-128-40 ../data/ogbn-arxiv/ogbn-arxiv GCNNEIGHBORGPU 15,25 100 shuffle 0.01 0.0001 0.5 1 1 3 1  "${array[*]}"
#!/bin/bash

pip install gdown packaging
pip install "numpy<2"

python ./generate_nts_dataset.py --dataset ogbn-arxiv
python ./generate_nts_dataset.py --dataset ogbn-products
python ./generate_nts_dataset.py --dataset ogbn-papers100M
python ./generate_nts_dataset.py --dataset reddit

#amazon
gdown https://drive.google.com/uc?id=1G_q3J4FebZR-oRwU4V7zQFgm0Rzmz4lV

#lj-links
gdown https://drive.google.com/uc?id=1SZOAf4msx9eV04K9nLrd2bkJk4NG8h8O

#livejournal
gdown https://drive.google.com/uc?id=16fNnSnsX3Jav7-qpyx6-L7NmAY3mi_3p

#lj-large
gdown https://drive.google.com/uc?id=1YL1Ts5qR8KY\\\_USPXIeIehZMF-PZ8ipq\\_

#enwiki-links
gdown https://drive.google.com/uc?id=1FS4N24hCqmUwN5x5MK2taopDSnc_xStA